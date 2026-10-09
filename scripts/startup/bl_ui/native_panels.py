# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Engine-thread hierarchy snapshots. Properties comes from Blender's actual UI layouts."""

import json
import bpy
from bpy.props import StringProperty

_ack = 0
_error = ""


def _uid(item):
    return str(item.session_uid)


def _object_actions(context, obj):
    actions = ['visibility']
    if context.mode == 'OBJECT' and obj.is_editable:
        actions += ['rename', 'move']
        if obj.visible_get(view_layer=context.view_layer) and not obj.hide_select:
            actions.append('duplicate')
        if not obj.library and not obj.override_library:
            actions.append('delete')
    return actions


def _collection_actions(context, layer):
    # The scene collection cannot be hidden, renamed or deleted, as in the Outliner.
    if layer == context.view_layer.layer_collection:
        return []
    coll = layer.collection
    actions = ['visibility']
    if context.mode == 'OBJECT' and coll.is_editable and not coll.override_library:
        actions += ['rename', 'delete', 'move']
    return actions


def _accepts_drop(context, layer):
    """Whether objects and collections can be moved into this collection."""
    coll = layer.collection
    return (context.mode == 'OBJECT' and not layer.exclude and coll.is_editable and
            not coll.override_library)


def _scene_collections(context):
    """Every collection in the scene, the scene collection included."""
    found = [context.scene.collection]
    for coll in found:
        found += [child for child in coll.children if child not in found]
    return found


def _collection_by_uid(context, uid):
    return next((coll for coll in _scene_collections(context) if _uid(coll) == uid), None)


def _objects(context, request):
    """The objects a request is about: a single `id`, or several `ids` for a selection."""
    ids = request.get('ids') or [request.get('id')]
    lookup = {_uid(obj): obj for obj in context.view_layer.objects}
    objects = [lookup[uid] for uid in ids if uid in lookup]
    if not objects or len(objects) != len(ids):
        raise ValueError("Some of these objects are no longer in the current view layer.")
    return objects


def _keep_transform(obj, parent):
    matrix = obj.matrix_world.copy()
    obj.parent = parent
    obj.matrix_world = matrix


def _move_objects(context, objects, target):
    """Move objects into a collection, out of the scene's other collections. Children whose parent
    stays behind are unparented in place, so they show up where they were dropped."""
    for obj in objects:
        for coll in _scene_collections(context):
            if coll != target and obj.name in coll.objects:
                coll.objects.unlink(obj)
        if obj.name not in target.objects:
            target.objects.link(obj)
    for obj in objects:
        if obj.parent and obj.parent not in objects:
            _keep_transform(obj, None)


def _parent_objects(context, objects, parent):
    """Parent objects to another, keeping where they are, like dropping in the Outliner."""
    for obj in objects:
        ancestor = parent
        while ancestor:
            if ancestor == obj:
                raise ValueError("An object cannot be parented to itself or its children.")
            ancestor = ancestor.parent
    shared = [coll for coll in _scene_collections(context) if parent.name in coll.objects]
    for obj in objects:
        _keep_transform(obj, parent)
        # Children list under their parent only when they share a collection.
        if shared and not any(obj.name in coll.objects for coll in shared):
            _move_objects(context, [obj], shared[0])
            _keep_transform(obj, parent)


def _move_collections(context, collections, target):
    for coll in collections:
        if coll == target or target in _descendants(coll):
            raise ValueError("A collection cannot be moved into itself.")
    for coll in collections:
        for parent in _scene_collections(context):
            if parent != target and coll.name in parent.children:
                parent.children.unlink(coll)
        if coll.name not in target.children:
            target.children.link(coll)


def _descendants(coll):
    found = list(coll.children)
    for child in found:
        found += [grandchild for grandchild in child.children if grandchild not in found]
    return found


def _layer_collection(layer, coll):
    if layer.collection == coll:
        return layer
    for child in layer.children:
        found = _layer_collection(child, coll)
        if found:
            return found
    return None


def _delete_collection(context, coll):
    """Remove a collection but keep its contents, like the Outliner's Delete: objects and child
    collections move to every collection that held it, so nothing leaves the scene."""
    parents = [c for c in bpy.data.collections if coll.name in c.children]
    parents += [scene.collection for scene in bpy.data.scenes if coll.name in scene.collection.children]
    for parent in parents:
        for obj in coll.objects:
            if obj.name not in parent.objects:
                parent.objects.link(obj)
        for child in coll.children:
            if child.name not in parent.children:
                parent.children.link(child)
    bpy.data.collections.remove(coll)


def _hierarchy(context):
    rows = []
    active = context.view_layer.objects.active
    objects = {obj.name: obj for obj in context.view_layer.objects}

    def collection(layer, depth, parent):
        coll = layer.collection
        key = parent + "/c" + _uid(coll)
        rows.append({"key": key, "parent": parent, "depth": depth, "kind": "collection",
                     "id": _uid(coll), "name": coll.name, "icon": "OUTLINER_COLLECTION",
                     "enabled": not layer.exclude, "active": False, "hidden": layer.hide_viewport,
                     "actions": _collection_actions(context, layer),
                     "accepts": _accepts_drop(context, layer)})
        members = {obj.name: obj for obj in coll.objects if obj.name in objects}
        children = {}
        for obj in members.values():
            parent_name = obj.parent.name if obj.parent and obj.parent.name in members else ""
            children.setdefault(parent_name, []).append(obj)

        def object_rows(parent_name, level, parent_key):
            for obj in sorted(children.get(parent_name, []), key=lambda item: item.name.casefold()):
                obj_key = parent_key + "/o" + _uid(obj)
                rows.append({"key": obj_key, "parent": parent_key, "depth": level,
                             "kind": "object", "id": _uid(obj), "name": obj.name,
                             "icon": 'OUTLINER_OB_' + obj.type, "active": obj == active,
                             "selected": obj.select_get(view_layer=context.view_layer),
                             "hidden": obj.hide_get(view_layer=context.view_layer),
                             "actions": _object_actions(context, obj),
                             "enabled": obj.visible_get(view_layer=context.view_layer)
                             and not obj.hide_select and context.mode == 'OBJECT'})
                object_rows(obj.name, level + 1, obj_key)

        object_rows("", depth + 1, key)
        for child in layer.children:
            collection(child, depth + 1, key)

    collection(context.view_layer.layer_collection, 0, "")
    parents = {row['parent'] for row in rows}
    for row in rows:
        row['children'] = row['key'] in parents
    return rows


def snapshot_json(context, mask):
    result = {"ack": _ack, "error": _error}
    if mask & 2:
        result["hierarchy"] = _hierarchy(context)
    return json.dumps(result, ensure_ascii=True, allow_nan=False)


class WM_OT_native_panel_edit(bpy.types.Operator):
    bl_idname = "wm.native_panel_edit"
    bl_label = "Outliner Edit"
    bl_options = {'INTERNAL', 'UNDO'}
    request: StringProperty(options={'HIDDEN', 'SKIP_SAVE'})

    def execute(self, context):
        global _error
        try:
            request = json.loads(self.request)
            obj = next((obj for obj in context.view_layer.objects if _uid(obj) == request.get('id')), None)
            action = request.get('action')
            if action in {'move', 'parent'}:
                # Dragged rows, or a selection moved with the menu: objects or collections.
                if request.get('kind') == 'collection':
                    collections = [_collection_by_uid(context, uid) for uid in request.get('ids', [])]
                    target = _collection_by_uid(context, request.get('target'))
                    if not collections or None in collections or target is None:
                        raise ValueError("These collections are no longer in the scene.")
                    layers = [_layer_collection(context.view_layer.layer_collection, coll)
                              for coll in collections]
                    if any(layer is None or 'move' not in _collection_actions(context, layer)
                           for layer in layers):
                        raise ValueError("These collections cannot be moved.")
                    _move_collections(context, collections, target)
                else:
                    objects = _objects(context, request)
                    if any('move' not in _object_actions(context, item) for item in objects):
                        raise ValueError("These objects cannot be moved in the current mode.")
                    if action == 'parent':
                        parent = next((item for item in context.view_layer.objects
                                       if _uid(item) == request.get('target')), None)
                        if parent is None:
                            raise ValueError("That object is no longer in the current view layer.")
                        _parent_objects(context, objects, parent)
                    else:
                        target = _collection_by_uid(context, request.get('target'))
                        layer = target and _layer_collection(context.view_layer.layer_collection, target)
                        if layer is None or not _accepts_drop(context, layer):
                            raise ValueError("Objects cannot be moved into that collection.")
                        _move_objects(context, objects, target)
            elif request.get('ids') and action in {'duplicate', 'delete', 'visibility'}:
                # The same action on every selected object, as one undo step.
                objects = _objects(context, request)
                if any(action not in _object_actions(context, item) for item in objects):
                    raise ValueError("This action is no longer available for all of these objects.")
                if action == 'visibility':
                    for item in objects:
                        item.hide_set(bool(request['value']), view_layer=context.view_layer)
                elif action == 'delete':
                    with context.temp_override(selected_objects=objects):
                        result = bpy.ops.object.delete('EXEC_DEFAULT', False, use_global=False)
                    if 'FINISHED' not in result:
                        raise ValueError("These objects could not be deleted.")
                else:
                    for other in context.view_layer.objects:
                        other.select_set(other in objects)
                    context.view_layer.objects.active = objects[0]
                    result = bpy.ops.object.duplicate('EXEC_DEFAULT', False, linked=False)
                    if 'FINISHED' not in result:
                        raise ValueError("These objects could not be duplicated.")
            elif action == 'collection_new':
                name = request.get('value')
                parent = context.view_layer.active_layer_collection
                if context.mode != 'OBJECT' or not parent.collection.is_editable or parent.exclude:
                    raise ValueError("A collection cannot be added here in the current mode.")
                if not isinstance(name, str) or not name.strip():
                    raise ValueError("Enter a collection name.")
                collection = bpy.data.collections.new(name.strip())
                try:
                    parent.collection.children.link(collection)
                except Exception:
                    bpy.data.collections.remove(collection)
                    raise
                context.view_layer.update()
                context.view_layer.active_layer_collection = parent.children[collection.name]
            elif request.get('kind') == 'collection':
                coll = next((c for c in bpy.data.collections if _uid(c) == request.get('id')), None)
                layer = coll and _layer_collection(context.view_layer.layer_collection, coll)
                action = request.get('action')
                if layer is None:
                    raise ValueError("This collection is no longer in the current view layer.")
                if action not in _collection_actions(context, layer):
                    raise ValueError("This action is no longer available for this collection.")
                if action == 'visibility':
                    layer.hide_viewport = bool(request['value'])
                elif action == 'rename':
                    name = request.get('value')
                    if not isinstance(name, str) or not name.strip():
                        raise ValueError("Enter a collection name.")
                    coll.name = name.strip()
                else:
                    _delete_collection(context, coll)
            elif obj is None:
                raise ValueError("This object is no longer in the current view layer.")
            elif request.get('action') == 'select':
                if context.mode != 'OBJECT' or obj.hide_select or not obj.visible_get():
                    raise ValueError("This object cannot be selected in the current mode.")
                selected = bool(request.get('value', True))
                if not request.get('extend', False):
                    for other in context.view_layer.objects:
                        other.select_set(other == obj)
                else:
                    obj.select_set(selected)
                if selected:
                    context.view_layer.objects.active = obj
                elif context.view_layer.objects.active == obj:
                    context.view_layer.objects.active = next(
                        (other for other in context.view_layer.objects if other.select_get()), None)
            elif request.get('action') == 'visibility':
                obj.hide_set(bool(request['value']), view_layer=context.view_layer)
            elif request.get('action') in {'duplicate', 'delete', 'rename'}:
                action = request['action']
                if action not in _object_actions(context, obj):
                    raise ValueError("This action is no longer available for this object.")
                if action == 'rename':
                    name = request.get('value')
                    if not isinstance(name, str) or not name.strip():
                        raise ValueError("Enter an object name.")
                    obj.name = name.strip()
                elif action == 'delete':
                    # Delete only this row, retaining Blender's scene/link and override handling.
                    with context.temp_override(selected_objects=[obj]):
                        result = bpy.ops.object.delete('EXEC_DEFAULT', False, use_global=False)
                    if 'FINISHED' not in result:
                        raise ValueError("This object could not be deleted.")
                else:
                    selected = list(context.selected_objects)
                    active = context.view_layer.objects.active
                    try:
                        # The duplicate operator consumes selected bases, not just RNA objects.
                        for other in context.view_layer.objects:
                            other.select_set(other == obj)
                        context.view_layer.objects.active = obj
                        result = bpy.ops.object.duplicate('EXEC_DEFAULT', False, linked=False)
                        if 'FINISHED' not in result:
                            raise ValueError("This object could not be duplicated.")
                    except Exception:
                        for other in context.view_layer.objects:
                            other.select_set(other in selected)
                        context.view_layer.objects.active = active
                        raise
            else:
                raise ValueError("Unknown hierarchy action.")
            for area in context.screen.areas:
                area.tag_redraw()
        except (ValueError, TypeError, RuntimeError, ReferenceError, KeyError) as error:
            _error = str(error)
            return {'CANCELLED'}
        return {'FINISHED'}


def apply_json(context, payload, blocked=False):
    global _ack, _error
    _error = ""
    try:
        request = json.loads(payload)
        _ack = int(request['sequence'])
        if blocked or context.window.modal_operators:
            raise ValueError("Finish the current operation before changing the hierarchy.")
        bpy.ops.wm.native_panel_edit('EXEC_DEFAULT', True, request=payload)
    except (ValueError, TypeError, RuntimeError, KeyError) as error:
        _error = str(error)


class VIEW3D_PT_native_sculpt_settings(bpy.types.Panel):
    """Key sculpt settings for the native iPadOS top bar."""
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Tool"
    bl_label = "Sculpt"

    @classmethod
    def poll(cls, context):
        # Only the native projection draws this, from the viewport's main region.
        # Blender's own UI draws side-bar panels in the 'UI' region and never polls true.
        region = context.region
        return (context.mode == 'SCULPT' and context.sculpt_object is not None and
                region is not None and region.type == 'WINDOW')

    def draw(self, context):
        from bl_ui.properties_paint_common import UnifiedPaintPanel

        layout = self.layout
        settings = UnifiedPaintPanel.paint_settings(context)
        brush = settings.brush if settings else None
        if brush is not None:
            ups = settings.unified_paint_settings
            size_owner = ups if ups.use_unified_size else brush
            size_prop = "unprojected_size" if size_owner.use_locked_size == 'SCENE' else "size"
            layout.prop(size_owner, size_prop, text="Radius", slider=True)
            strength_owner = ups if ups.use_unified_strength else brush
            layout.prop(strength_owner, "strength", text="Strength", slider=True)
            if brush.sculpt_capabilities.has_direction:
                layout.prop(brush, "direction", text="Direction")

        obj = context.sculpt_object
        dyntopo = obj.use_dynamic_topology_sculpting
        layout.operator(
            "sculpt.dynamic_topology_toggle",
            text="Dyntopo",
            icon='CHECKBOX_HLT' if dyntopo else 'CHECKBOX_DEHLT',
        )
        if dyntopo:
            sculpt = context.tool_settings.sculpt
            if sculpt.detail_type_method in {'CONSTANT', 'MANUAL'}:
                layout.prop(sculpt, "constant_detail_resolution", text="Detail", slider=True)
            elif sculpt.detail_type_method == 'BRUSH':
                layout.prop(sculpt, "detail_percent", text="Detail", slider=True)
            else:
                layout.prop(sculpt, "detail_size", text="Detail", slider=True)
        layout.prop(obj, "use_mesh_mirror_x", text="Mirror X")


classes = (WM_OT_native_panel_edit, VIEW3D_PT_native_sculpt_settings)
