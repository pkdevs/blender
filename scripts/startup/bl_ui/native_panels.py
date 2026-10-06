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
        actions.append('rename')
        if obj.visible_get(view_layer=context.view_layer) and not obj.hide_select:
            actions.append('duplicate')
        if not obj.library and not obj.override_library:
            actions.append('delete')
    return actions


def _hierarchy(context):
    rows = []
    active = context.view_layer.objects.active
    objects = {obj.name: obj for obj in context.view_layer.objects}

    def collection(layer, depth, parent):
        coll = layer.collection
        key = parent + "/c" + _uid(coll)
        rows.append({"key": key, "parent": parent, "depth": depth, "kind": "collection",
                     "id": _uid(coll), "name": coll.name, "icon": "OUTLINER_COLLECTION",
                     "enabled": not layer.exclude, "active": False})
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
    bl_label = "Change Hierarchy"
    bl_options = {'INTERNAL', 'UNDO'}
    request: StringProperty(options={'HIDDEN', 'SKIP_SAVE'})

    def execute(self, context):
        global _error
        try:
            request = json.loads(self.request)
            obj = next((obj for obj in context.view_layer.objects if _uid(obj) == request.get('id')), None)
            if request.get('action') == 'collection_new':
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


classes = (WM_OT_native_panel_edit,)
