"""
Penny Noise Preview Renderer
=============================
Renders a preview image of each of the 19 Penny noise types.
Run inside Blender:
    blender --background --python render_penny_previews.py
Or open in Blender's Text Editor and press Run Script.

Output: doc/penny_noise/images/<noise_name>.png
"""

import bpy
import os
import math

# ---- Configuration ----
RENDER_SIZE = 1024
SAMPLES = 64
SCALE = 100.0
OCTAVES = 7.0
ROUGHNESS = 0.5

OUTPUT_DIR = os.path.join(os.getcwd(), "doc", "penny_noise", "images")

NOISE_TYPES = [
    (0,  "FBM",          "fBm",                    "fbm"),
    (1,  "TURBULENCE",   "Turbulence",             "turbulence"),
    (2,  "STUPL",        "Stupl",                   "stupl"),
    (3,  "LUKA",         "Luka",                    "luka"),
    (4,  "BLISTERED",    "Blistered Turbulence",    "blistered_turbulence"),
    (5,  "BOX",          "Box",                     "box"),
    (6,  "MOD",          "Mod",                     "mod"),
    (7,  "NUTOUS",       "Nutous",                  "nutous"),
    (8,  "CRANAL",       "Cranal",                  "cranal"),
    (9,  "BUYA",         "Buya",                    "buya"),
    (10, "CELL",         "Cell",                    "cell"),
    (11, "DISTURB",      "Displaced Turbulence",    "displaced_turbulence"),
    (12, "HAMA",         "Hama",                    "hama"),
    (13, "NAKI",         "Naki",                    "naki"),
    (14, "POXO",         "Poxo",                    "poxo"),
    (15, "PEZO",         "Pezo",                    "pezo"),
    (16, "DENTS",        "Dents",                   "dents"),
    (17, "SEMA",         "Sema",                    "sema"),
    (18, "VL_NOISE",     "VL",                      "vl"),
]


def setup_scene():
    """Create a minimal scene using data API only (no bpy.ops, no context)."""

    # Clear everything
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    for mesh in list(bpy.data.meshes):
        bpy.data.meshes.remove(mesh)
    for mat in list(bpy.data.materials):
        bpy.data.materials.remove(mat)
    for cam in list(bpy.data.cameras):
        bpy.data.cameras.remove(cam)
    for world in list(bpy.data.worlds):
        bpy.data.worlds.remove(world)

    scene = bpy.context.scene

    # ---- Render settings ----
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.samples = SAMPLES
    scene.cycles.use_denoising = False
    scene.render.resolution_x = RENDER_SIZE
    scene.render.resolution_y = RENDER_SIZE
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGB'
    scene.render.image_settings.compression = 15

    # ---- World: black background ----
    world = bpy.data.worlds.new("PennyWorld")
    scene.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0, 0, 0, 1)
    bg.inputs["Strength"].default_value = 0.0

    # ---- Plane: 2x2 meters at origin ----
    import bmesh
    mesh = bpy.data.meshes.new("PennyPlane")
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=1.0)
    bm.to_mesh(mesh)
    bm.free()

    plane = bpy.data.objects.new("PennyPreviewPlane", mesh)
    scene.collection.objects.link(plane)

    # ---- Camera: orthographic top-down ----
    cam_data = bpy.data.cameras.new("PennyCamera")
    cam_data.type = 'ORTHO'
    cam_data.ortho_scale = 2.0

    camera = bpy.data.objects.new("PennyCamera", cam_data)
    camera.location = (0, 0, 2)
    camera.rotation_euler = (0, 0, 0)
    scene.collection.objects.link(camera)
    scene.camera = camera

    # ---- Material: Penny Noise -> Emission -> Output ----
    mat = bpy.data.materials.new("PennyPreviewMat")
    mat.use_nodes = True
    tree = mat.node_tree
    tree.nodes.clear()

    # Penny Noise node
    penny = tree.nodes.new(type='ShaderNodeTexPenny')
    penny.location = (-400, 0)

    # Set dimensions
    try:
        penny.penny_dimensions = '3D'
    except:
        try:
            penny.dimensions = '3D'
        except:
            pass

    # Set socket defaults
    for sock in penny.inputs:
        if sock.name == "Scale":
            sock.default_value = SCALE
        elif sock.name == "Octaves":
            sock.default_value = OCTAVES
        elif sock.name == "Roughness":
            sock.default_value = ROUGHNESS

    # Emission shader (unlit)
    emission = tree.nodes.new(type='ShaderNodeEmission')
    emission.location = (0, 0)
    emission.inputs["Strength"].default_value = 1.0

    # Material output
    output = tree.nodes.new(type='ShaderNodeOutputMaterial')
    output.location = (200, 0)

    # Connect: Penny Fac -> Emission Color -> Output Surface (grayscale)
    tree.links.new(penny.outputs["Fac"], emission.inputs["Color"])
    tree.links.new(emission.outputs["Emission"], output.inputs["Surface"])

    # Assign material
    plane.data.materials.append(mat)

    return scene, penny


def render_all_types(scene, penny_node):
    """Cycle through all 19 noise types and render each."""

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    print(f"\n{'='*60}")
    print(f"Penny Noise Preview Renderer")
    print(f"Output: {OUTPUT_DIR}")
    print(f"Resolution: {RENDER_SIZE}x{RENDER_SIZE}, Samples: {SAMPLES}")
    print(f"{'='*60}\n")

    for idx, internal, display, filename in NOISE_TYPES:
        print(f"  [{idx+1:2d}/19] Rendering {display} ({internal})...")

        # Set noise type by string identifier
        try:
            penny_node.noise_type = internal
        except TypeError:
            # Fall back to integer index
            penny_node.noise_type = idx

        # Set output path
        filepath = os.path.join(OUTPUT_DIR, f"{filename}.png")
        scene.render.filepath = filepath

        # Render
        bpy.ops.render.render(write_still=True)
        print(f"         -> {filepath}")

    print(f"\n{'='*60}")
    print(f"Done. {len(NOISE_TYPES)} previews saved to {OUTPUT_DIR}")
    print(f"{'='*60}\n")


if __name__ == "__main__":
    scene, penny_node = setup_scene()
    render_all_types(scene, penny_node)
