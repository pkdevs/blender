# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import api
import enum
import json
import pathlib


RESULT_KEY = "GP_SCULPT_RESULT: "
ERROR_KEY = "GP_SCULPT_ERROR: "

NUM_LOOPS = 1
STEPS_PER_LOOP = 64
GRAB_STROKES = 4
GRAB_STEPS_PER_STROKE = 16
EVENT_INTERVAL = 0.005
RELEASE_SETTLE_TIME = 0.1
MAX_RUN_TIME = 120.0


class GreasePencilBrushType(enum.Enum):
    SMOOTH = "SMOOTH"
    THICKNESS = "THICKNESS"
    STRENGTH = "STRENGTH"
    GRAB = "GRAB"
    PULL = "PUSH"
    TWIST = "TWIST"
    PINCH = "PINCH"
    RANDOMIZE = "RANDOMIZE"
    CLONE = "CLONE"


def set_view3d_context_override(context_override):
    for area in context_override["screen"].areas:
        if area.type != 'VIEW_3D':
            continue

        for region in area.regions:
            if region.type == 'WINDOW':
                context_override["area"] = area
                context_override["region"] = region
                return

    raise RuntimeError("No 3D View window region found")


def generate_stroke(context):
    import math

    area = context["area"]
    center_x = area.width / 2
    center_y = area.height / 2
    radius_x = area.width * 0.4
    radius_y = area.height * 0.15
    num_steps = NUM_LOOPS * STEPS_PER_LOOP

    return [
        (
            int(area.x + center_x + radius_x * math.sin(angle)),
            int(area.y + center_y + radius_y * math.sin(2 * angle)),
        )
        for i in range(num_steps)
        for angle in [2 * math.pi * NUM_LOOPS * i / (num_steps - 1)]
    ]


def generate_grab_strokes(context):
    area = context["area"]
    strokes = []

    for index in range(GRAB_STROKES):
        start_x = area.x + area.width * (0.12 + index * 0.24)
        start_y = area.y + area.height * (0.48 + (index % 2) * 0.04)
        delta_x = area.width * 0.08
        delta_y = area.height * (0.025 if index % 2 == 0 else -0.025)
        stroke = []
        for step in range(GRAB_STEPS_PER_STROKE):
            factor = step / (GRAB_STEPS_PER_STROKE - 1)
            stroke.append((
                int(start_x + delta_x * factor),
                int(start_y + delta_y * factor),
            ))
        strokes.append(stroke)

    return strokes


def _run(args):
    import bpy
    import time

    settings = bpy.context.scene.tool_settings.gpencil_sculpt_paint
    brush = settings.brush
    if brush is None:
        raise RuntimeError("Input file has no active Grease Pencil sculpt brush")

    brush_type = args["brush_type"]
    valid_brush_types = {
        item.identifier
        for item in brush.bl_rna.properties["gpencil_sculpt_brush_type"].enum_items
    }
    if brush_type not in valid_brush_types:
        raise RuntimeError(
            f"Brush type {brush_type!r} is unavailable; "
            f"available types: {', '.join(sorted(valid_brush_types))}"
        )
    brush.gpencil_sculpt_brush_type = brush_type
    if brush.gpencil_sculpt_brush_type != brush_type:
        raise RuntimeError(
            f"Failed to select {brush_type!r}; active mode is "
            f"{brush.gpencil_sculpt_brush_type!r}"
        )

    obj = bpy.context.active_object
    if obj is None or obj.type != 'GREASEPENCIL':
        raise RuntimeError("Input file has no active Grease Pencil object")
    if obj.mode != 'SCULPT_GREASE_PENCIL':
        raise RuntimeError("Input Grease Pencil object is not in Sculpt Mode")

    is_clone = brush_type == "CLONE"
    if is_clone:
        result = bpy.ops.object.mode_set(mode='EDIT')
        if 'FINISHED' not in result:
            raise RuntimeError("Could not enter Grease Pencil Edit Mode for Clone")
        result = bpy.ops.grease_pencil.select_all(action='SELECT')
        if 'FINISHED' not in result:
            raise RuntimeError("Could not select Grease Pencil strokes for Clone")
        result = bpy.ops.grease_pencil.copy()
        if 'FINISHED' not in result:
            raise RuntimeError("Could not copy Grease Pencil strokes for Clone")
        result = bpy.ops.object.mode_set(mode='SCULPT_GREASE_PENCIL')
        if 'FINISHED' not in result:
            raise RuntimeError("Could not return to Grease Pencil Sculpt Mode")

    context_override = bpy.context.copy()
    set_view3d_context_override(context_override)
    window = bpy.context.window
    if window is None:
        raise RuntimeError("No Blender window available for stroke simulation")

    original_data = obj.data.copy()
    events = []

    def prepare_events():
        if is_clone:
            x, y = generate_stroke(context_override)[0]
            return [
                ("MOUSEMOVE", "NOTHING", x, y),
                ("LEFTMOUSE", "PRESS", x, y),
                ("LEFTMOUSE", "RELEASE", x, y),
            ]

        paths = (
            generate_grab_strokes(context_override)
            if brush_type == "GRAB"
            else [generate_stroke(context_override)]
        )
        events_for_strokes = []
        for points in paths:
            start_x, start_y = points[0]
            end_x, end_y = points[-1]
            events_for_strokes.append(("MOUSEMOVE", "NOTHING", start_x, start_y))
            events_for_strokes.append(("LEFTMOUSE", "PRESS", start_x, start_y))
            events_for_strokes.extend(
                ("MOUSEMOVE", "NOTHING", x, y) for x, y in points[1:]
            )
            events_for_strokes.append(("LEFTMOUSE", "RELEASE", end_x, end_y))
        return events_for_strokes

    state = {
        "phase": "warmup",
        "event_index": 0,
        "start_time": None,
        "finish_after": None,
        "run_started": time.perf_counter(),
    }
    events.extend(prepare_events())

    def finish_error(error):
        print(f"{ERROR_KEY}{type(error).__name__}: {error}")
        bpy.app.use_event_simulate = False
        bpy.ops.wm.quit_blender()

    def event_step():
        try:
            if time.perf_counter() - state["run_started"] > MAX_RUN_TIME:
                raise RuntimeError(
                    f"Grease Pencil stroke simulation exceeded {MAX_RUN_TIME:g} seconds"
                )

            index = state["event_index"]
            if index < len(events):
                event_type, value, x, y = events[index]
                if state["phase"] == "measure" and index == 1:
                    state["start_time"] = time.perf_counter()
                window.event_simulate(
                    type=event_type,
                    value=value,
                    x=x,
                    y=y,
                )
                state["event_index"] += 1
                return EVENT_INTERVAL

            if state["finish_after"] is None:
                state["finish_after"] = time.perf_counter() + RELEASE_SETTLE_TIME
                return EVENT_INTERVAL

            if time.perf_counter() < state["finish_after"]:
                return EVENT_INTERVAL

            if state["phase"] == "warmup":
                obj.data = original_data.copy()
                state["phase"] = "measure"
                state["event_index"] = 0
                state["finish_after"] = None
                events[:] = prepare_events()
                return EVENT_INTERVAL

            elapsed = time.perf_counter() - state["start_time"]
            print(f"{RESULT_KEY}{json.dumps({'time': elapsed})}")
            bpy.app.use_event_simulate = False
            if original_data.users == 0:
                bpy.data.grease_pencils.remove(original_data)
            bpy.ops.wm.quit_blender()
            return None
        except Exception as error:
            finish_error(error)
            return None

    bpy.app.timers.register(event_step, first_interval=0.0, persistent=True)
    return {}


class GreasePencilSculptBrushTest(api.Test):
    def __init__(self, filepath: pathlib.Path, brush_type: GreasePencilBrushType):
        self.filepath = filepath
        self.brush_type = brush_type

    def name(self):
        return f"{self.filepath.stem}_{self.brush_type.name.lower()}"

    def category(self):
        return "gp_sculpt"

    def use_device(self):
        return True

    def supported_device_types(self):
        return ["VULKAN", "OPENGL", "METAL"]

    def run(self, env, device_id, gpu_backend):
        args = {"brush_type": self.brush_type.value}
        blender_args = api.test.Test.blender_gpu_arguments(device_id, gpu_backend)
        blender_args.extend(["--enable-event-simulate", self.filepath])

        _, log = env.run_in_blender(_run, args, blender_args, foreground=True)

        for line in log:
            if line.startswith(RESULT_KEY):
                return json.loads(line[len(RESULT_KEY):])
            if line.startswith(ERROR_KEY):
                raise RuntimeError(line[len(ERROR_KEY):])

        raise RuntimeError("No Grease Pencil sculpt benchmark result found")


def generate(env):
    filepaths = env.find_blend_files("grease_pencil/scuplt_brushes")
    return [
        GreasePencilSculptBrushTest(filepath, brush_type)
        for filepath in filepaths
        for brush_type in GreasePencilBrushType
    ]
