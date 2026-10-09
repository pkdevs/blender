# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Rendering and viewport options for the native iPad interface.

Renders run as Blender's usual render job, without its render window: the iPad shows progress and
the result itself. Animations are written as PNG frames to a temporary folder, from which the iPad
makes a video. Settings changed for this are restored when the render ends.
"""

import json
import os
import re

import bpy

__all__ = (
    "start",
    "cancel",
    "clear",
    "status_json",
    "viewport_json",
    "viewport_toggle",
)

_state = {"state": "idle"}
_restore = None
_sequence = 0

# EEVEE: "Rendering 12 / 64 samples", Cycles: "Sample 12/64".
_SAMPLES = (
    re.compile(r"(\d+)\s*/\s*(\d+)\s*samples", re.IGNORECASE),
    re.compile(r"Sample\s+(\d+)\s*/\s*(\d+)", re.IGNORECASE),
)


def _set_state(**values):
    global _state, _sequence
    _sequence += 1
    _state = dict(values, id=_sequence)


def _save_settings(context):
    scene = context.scene
    render = scene.render
    return {
        "scene": scene.name,
        "display": context.preferences.view.render_display_type,
        "filepath": render.filepath,
        "file_format": render.image_settings.file_format,
        "color_mode": render.image_settings.color_mode,
        "use_file_extension": render.use_file_extension,
        "use_overwrite": render.use_overwrite,
        "use_placeholder": render.use_placeholder,
    }


def _restore_settings():
    global _restore
    saved, _restore = _restore, None
    if saved is None:
        return
    bpy.context.preferences.view.render_display_type = saved["display"]
    scene = bpy.data.scenes.get(saved["scene"])
    if scene is None:
        return
    render = scene.render
    render.filepath = saved["filepath"]
    render.image_settings.file_format = saved["file_format"]
    render.image_settings.color_mode = saved["color_mode"]
    render.use_file_extension = saved["use_file_extension"]
    render.use_overwrite = saved["use_overwrite"]
    render.use_placeholder = saved["use_placeholder"]


def _remove_handlers():
    for handlers, function in (
            (bpy.app.handlers.render_stats, _on_stats),
            (bpy.app.handlers.render_post, _on_frame),
            (bpy.app.handlers.render_complete, _on_complete),
            (bpy.app.handlers.render_cancel, _on_cancel),
    ):
        if function in handlers:
            handlers.remove(function)


def _on_stats(stats, *_args):
    if _state.get("state") != "rendering" or not isinstance(stats, str):
        return
    for pattern in _SAMPLES:
        if match := pattern.search(stats):
            done, total = int(match.group(1)), int(match.group(2))
            if total > 0:
                _set_state(**dict(_state, samples=min(1.0, done / total)))
            return


def _on_frame(scene, *_args):
    if _state.get("state") != "rendering" or not _state.get("animation"):
        return
    path = scene.render.frame_path(frame=scene.frame_current)
    _set_state(**dict(_state, done=_state["done"] + 1, frame=scene.frame_current, last=path, samples=0.0))


def _on_complete(scene, *_args):
    _remove_handlers()
    state = dict(_state)
    try:
        if state.get("animation"):
            folder = state["folder"]
            frames = sorted(
                os.path.join(folder, name) for name in os.listdir(folder) if name.lower().endswith(".png")
            )
            state.update(state="done", frames=frames)
        else:
            path = os.path.join(state["folder"], "Render.png")
            image = bpy.data.images.get("Render Result")
            if image is None:
                raise RuntimeError("The render produced no image.")
            image.save_render(path, scene=scene)
            state.update(state="done", image=path)
    except Exception as ex:
        state.update(state="error", message=str(ex))
    _restore_settings()
    _set_state(**state)


def _on_cancel(*_args):
    _remove_handlers()
    _restore_settings()
    _set_state(state="cancelled")


def start(context, animation):
    """Start rendering the image, or the animation when `animation` is true."""
    global _restore
    if bpy.app.is_job_running('RENDER') or _state.get("state") == "rendering":
        return
    scene = context.scene
    if scene.camera is None:
        _set_state(state="error", message="Add a camera to the scene to render it.")
        return
    folder = os.path.join(bpy.app.tempdir, "Render {:d}".format(_sequence + 1))
    os.makedirs(folder, exist_ok=True)

    _restore = _save_settings(context)
    # Blender's render window is replaced by the iPad's own progress and result.
    context.preferences.view.render_display_type = 'NONE'
    render = scene.render
    if animation:
        render.filepath = os.path.join(folder, "Frame ")
        render.image_settings.file_format = 'PNG'
        render.image_settings.color_mode = 'RGB'
        render.use_file_extension = True
        render.use_overwrite = True
        render.use_placeholder = False

    bpy.app.handlers.render_stats.append(_on_stats)
    bpy.app.handlers.render_post.append(_on_frame)
    bpy.app.handlers.render_complete.append(_on_complete)
    bpy.app.handlers.render_cancel.append(_on_cancel)
    step = max(1, scene.frame_step)
    _set_state(
        state="rendering",
        animation=bool(animation),
        folder=folder,
        frame=scene.frame_start if animation else scene.frame_current,
        total=(len(range(scene.frame_start, scene.frame_end + 1, step)) if animation else 1),
        done=0,
        samples=0.0,
        fps=render.fps / render.fps_base,
        width=int(render.resolution_x * render.resolution_percentage / 100),
        height=int(render.resolution_y * render.resolution_percentage / 100),
    )
    try:
        result = bpy.ops.render.render('INVOKE_DEFAULT', animation=bool(animation))
    except RuntimeError as ex:
        result = {'CANCELLED'}
        message = str(ex)
    else:
        message = "Rendering could not start."
    if 'RUNNING_MODAL' not in result and 'FINISHED' not in result:
        _remove_handlers()
        _restore_settings()
        _set_state(state="error", message=message)


def cancel(_context):
    """Called after the render job is stopped, in case Blender reported no cancellation."""
    if _state.get("state") == "rendering":
        _on_cancel(None)


def clear(_context):
    """The iPad has shown the result: forget it."""
    if _state.get("state") != "rendering":
        _set_state(state="idle")


def status_json(_context):
    return json.dumps(_state, ensure_ascii=True, allow_nan=False)


# -----------------------------------------------------------------------------
# Viewport options for the iPad's "More" menu.

def _view3d(context):
    space = context.space_data
    if space is not None and space.type == 'VIEW_3D':
        return space
    for area in context.screen.areas if context.screen else ():
        if area.type == 'VIEW_3D':
            return area.spaces.active
    return None


def _xray_owner(space):
    shading = space.shading
    return shading, ("show_xray_wireframe" if shading.type == 'WIREFRAME' else "show_xray")


_TOGGLES = {
    "overlays": lambda space: (space.overlay, "show_overlays"),
    "xray": _xray_owner,
    "gizmos": lambda space: (space, "show_gizmo"),
    "statistics": lambda space: (space.overlay, "show_stats"),
    "wireframe": lambda space: (space.overlay, "show_wireframes"),
}


def viewport_json(context):
    space = _view3d(context)
    if space is None:
        return "null"
    result = {}
    for name, owner_and_property in _TOGGLES.items():
        owner, prop = owner_and_property(space)
        result[name] = bool(getattr(owner, prop))
    return json.dumps(result)


def viewport_toggle(context, name):
    space = _view3d(context)
    toggle = _TOGGLES.get(name)
    if space is None or toggle is None:
        return
    owner, prop = toggle(space)
    setattr(owner, prop, not getattr(owner, prop))
    for area in context.screen.areas if context.screen else ():
        if area.type == 'VIEW_3D':
            area.tag_redraw()


classes = ()
