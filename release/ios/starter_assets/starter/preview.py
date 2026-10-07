# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Renders every asset's thumbnail in one shared studio, so the asset pane looks consistent."""

import math
import os
import tempfile

import bpy
from mathutils import Vector

SIZE = 256
SCENE = "Starter Preview"

# Studio lights relative to the camera, for a subject framed 3 m away: energy (W), offset, size.
LIGHTS = {
    "Key": (135.0, (-2.6, 2.4, -0.4), 2.5),
    "Fill": (40.0, (2.8, 0.6, 0.6), 3.5),
    "Rim": (105.0, (0.6, 3.2, 3.0), 2.0),
}

# Camera direction per view: (azimuth, elevation) in degrees.
VIEWS = {"front": (35.0, 22.0), "top": (35.0, 62.0), "side": (90.0, 8.0)}


def _studio():
    scene = bpy.data.scenes.get(SCENE)
    if scene:
        return scene
    scene = bpy.data.scenes.new(SCENE)
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = scene.render.resolution_y = SIZE
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.eevee.taa_render_samples = 24
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.look = "AgX - Medium High Contrast"

    # The camera sees a dark backdrop; reflections see a soft studio gradient, so metals,
    # glass and glossy materials have something to reflect.
    world = bpy.data.worlds.new(SCENE)
    world.use_nodes = True
    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputWorld")
    backdrop = nodes.new("ShaderNodeBackground")
    backdrop.inputs["Color"].default_value = (0.03, 0.032, 0.036, 1.0)
    studio = nodes.new("ShaderNodeBackground")
    studio.inputs["Strength"].default_value = 1.0
    coordinate = nodes.new("ShaderNodeTexCoord")
    separate = nodes.new("ShaderNodeSeparateXYZ")
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.35
    ramp.color_ramp.elements[0].color = (0.04, 0.04, 0.045, 1.0)
    ramp.color_ramp.elements[1].position = 0.85
    ramp.color_ramp.elements[1].color = (0.9, 0.9, 0.92, 1.0)
    map_range = nodes.new("ShaderNodeMapRange")
    map_range.inputs["From Min"].default_value = -1.0
    light_path = nodes.new("ShaderNodeLightPath")
    mix = nodes.new("ShaderNodeMixShader")
    links.new(coordinate.outputs["Generated"], separate.inputs[0])
    links.new(separate.outputs["Z"], map_range.inputs["Value"])
    links.new(map_range.outputs["Result"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], studio.inputs["Color"])
    links.new(light_path.outputs["Is Camera Ray"], mix.inputs["Fac"])
    links.new(studio.outputs[0], mix.inputs[1])
    links.new(backdrop.outputs[0], mix.inputs[2])
    links.new(mix.outputs[0], output.inputs["Surface"])
    scene.world = world

    camera_data = bpy.data.cameras.new(SCENE)
    camera_data.lens = 60.0
    camera = bpy.data.objects.new(SCENE + " Camera", camera_data)
    scene.collection.objects.link(camera)
    scene.camera = camera

    # Key, fill and rim, all soft, parented to the camera so lighting follows every view.
    for name, (energy, offset, size) in LIGHTS.items():
        light_data = bpy.data.lights.new(f"{SCENE} {name}", "AREA")
        light_data.energy = energy
        light_data.size = size
        light = bpy.data.objects.new(f"{SCENE} {name}", light_data)
        light.parent = camera
        light.location = offset
        constraint = light.constraints.new("TRACK_TO")
        constraint.track_axis = "TRACK_NEGATIVE_Z"
        constraint.up_axis = "UP_Y"
        target = bpy.data.objects.get(SCENE + " Target") or bpy.data.objects.new(SCENE + " Target", None)
        if target.name not in scene.collection.objects:
            scene.collection.objects.link(target)
        constraint.target = target
        scene.collection.objects.link(light)
    return scene


def _base_mesh(kind):
    """Shared preview meshes that materials and modifiers are shown on."""
    name = f"{SCENE} Base {kind}"
    obj = bpy.data.objects.get(name)
    if obj:
        return obj
    if kind == "sphere":
        bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=0.5, location=(0, 0, 0.5))
    elif kind == "cube":
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0, 0, 0.5))
    elif kind == "monkey":
        bpy.ops.mesh.primitive_monkey_add(size=1.0, location=(0, 0, 0.45))
    elif kind == "plane":
        bpy.ops.mesh.primitive_grid_add(x_subdivisions=32, y_subdivisions=32, size=2.0)
    elif kind == "torus":
        bpy.ops.mesh.primitive_torus_add(major_radius=0.45, minor_radius=0.18, location=(0, 0, 0.18))
    else:
        raise ValueError(f"Unknown preview base {kind!r}")
    obj = bpy.context.active_object
    obj.name = name
    obj.data.name = name
    if kind in {"sphere", "monkey", "torus"}:
        for polygon in obj.data.polygons:
            polygon.use_smooth = True
    for collection in list(obj.users_collection):
        collection.objects.unlink(obj)
    return obj


def _frame(scene, objects, view):
    """Aim the camera at the evaluated bounds of `objects`."""
    with bpy.context.temp_override(scene=scene, view_layer=scene.view_layers[0]):
        depsgraph = bpy.context.evaluated_depsgraph_get()
    depsgraph.update()
    low = Vector((math.inf,) * 3)
    high = Vector((-math.inf,) * 3)
    for instance in depsgraph.object_instances:
        obj = instance.object
        if obj.original not in objects and (instance.parent is None or
                                            instance.parent.original not in objects):
            continue
        if obj.type not in {"MESH", "CURVE", "CURVES", "POINTCLOUD", "VOLUME", "FONT"}:
            continue
        for corner in obj.bound_box:
            point = instance.matrix_world @ Vector(corner)
            low = Vector(map(min, low, point))
            high = Vector(map(max, high, point))
    if low.x == math.inf:
        low, high = Vector((-0.5, -0.5, 0)), Vector((0.5, 0.5, 1))
    center = (low + high) * 0.5
    radius = max((high - low).length * 0.5, 0.005)
    azimuth, elevation = (math.radians(a) for a in VIEWS[view])
    direction = Vector((math.sin(azimuth) * math.cos(elevation),
                        -math.cos(azimuth) * math.cos(elevation),
                        math.sin(elevation)))
    camera = scene.camera
    fov = camera.data.angle
    distance = radius / math.sin(fov * 0.5) * 1.08
    camera.location = center + direction * distance
    camera.rotation_euler = (-direction).to_track_quat("-Z", "Y").to_euler()
    camera.data.clip_start = distance * 0.01
    camera.data.clip_end = distance * 10.0
    target = bpy.data.objects[SCENE + " Target"]
    target.location = center
    # The whole light rig scales with the framed size, so small and large assets read the same.
    factor = distance / 3.0
    for obj in scene.collection.objects:
        if obj.type == "LIGHT":
            energy, offset, _ = LIGHTS[obj.name.rsplit(" ", 1)[-1]]
            obj.location = Vector(offset) * factor
            obj.scale = (factor,) * 3
            obj.data.energy = energy * factor ** 2


def _render_to_preview(scene, id_data):
    # Unique per process: builds may run side by side.
    path = os.path.join(tempfile.gettempdir(), f"starter_preview_{os.getpid()}.png")
    scene.render.filepath = path
    with bpy.context.temp_override(scene=scene):
        bpy.ops.render.render(write_still=True, scene=scene.name)
    image = bpy.data.images.load(path, check_existing=False)
    try:
        id_data.preview_ensure()
        preview = id_data.preview
        preview.image_size = (SIZE, SIZE)
        preview.image_pixels_float = list(image.pixels)
    finally:
        bpy.data.images.remove(image)


def render(spec, id_data):
    """Render the thumbnail for one asset and store it as the asset's preview."""
    scene = _studio()
    shown = []
    created = []
    if spec.kind == "object":
        scene.collection.objects.link(id_data)
        shown.append(id_data)
    elif spec.kind == "collection":
        instance = bpy.data.objects.new(f"{SCENE} Instance", None)
        instance.instance_type = "COLLECTION"
        instance.instance_collection = id_data
        scene.collection.objects.link(instance)
        shown.append(instance)
        created.append(instance)
    else:
        base = _base_mesh(spec.preview_base)
        obj = bpy.data.objects.new(f"{SCENE} Subject", base.data.copy())
        obj.location = base.location
        scene.collection.objects.link(obj)
        if spec.kind == "material":
            obj.data.materials.clear()
            obj.data.materials.append(id_data)
        else:
            modifier = obj.modifiers.new(id_data.name, "NODES")
            modifier.node_group = id_data
        shown.append(obj)
        created.append(obj)
    # A collection that brings its own lights is shown lit by them alone.
    own_lights = spec.kind == "collection" and any(
        obj.type == "LIGHT" for obj in id_data.all_objects)
    studio_lights = [obj for obj in scene.collection.objects if obj.type == "LIGHT"]
    for light in studio_lights:
        light.hide_render = own_lights
    try:
        _frame(scene, shown, spec.preview_view)
        _render_to_preview(scene, id_data)
    finally:
        for light in studio_lights:
            light.hide_render = False
        if spec.kind == "object":
            scene.collection.objects.unlink(id_data)
        for obj in created:
            mesh = obj.data if obj.type == "MESH" else None
            bpy.data.objects.remove(obj)
            if mesh and mesh.users == 0:
                bpy.data.meshes.remove(mesh)


def cleanup():
    """Remove the studio so it's never written into the library."""
    scene = bpy.data.scenes.get(SCENE)
    if not scene:
        return
    for obj in list(scene.collection.objects):
        bpy.data.objects.remove(obj)
    for obj in [o for o in bpy.data.objects if o.name.startswith(SCENE)]:
        bpy.data.objects.remove(obj)
    world = scene.world
    bpy.data.scenes.remove(scene)
    if world:
        bpy.data.worlds.remove(world)
