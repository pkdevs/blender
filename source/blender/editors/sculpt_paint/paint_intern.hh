/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BLI_index_mask_fwd.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rand.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_object_enums.h"
#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"
#include "DNA_vec_types.h"
#include "DNA_windowmanager_enums.h"

#include "ED_view3d.hh"

#include <optional>

namespace blender {

enum class PaintMode : int8_t;

struct ARegion;
struct bContext;
struct Brush;
struct Depsgraph;
struct Image;
struct ImagePool;
struct ImageUser;
struct ImBuf;
struct Main;
struct MTex;
struct Object;
struct Paint;
struct PointerRNA;
struct RegionView3D;
struct ReportList;
struct Scene;
struct ScrArea;
struct SculptSession;
struct SpaceImage;
struct ToolSettings;
struct VertProjHandle;
struct ViewContext;
struct VPaint;
struct wmEvent;
struct wmKeyConfig;
struct wmKeyMap;
struct wmOperator;
struct wmOperatorType;
struct wmPaintCursor;

namespace bke::pbvh {
class Node;
}

namespace ed::sculpt_paint {
class PaintModeData;
struct PaintStroke;
struct PaintSample;
struct StrokeCache;
}  // namespace ed::sculpt_paint

namespace ocio {
class Display;
}
using ColorManagedDisplay = ocio::Display;

/* paint_stroke.cc */

enum class BrushStrokeMode : int8_t {
  Normal = 0,
  Invert = 1,
};

/* Indicates a brush that the stroke will switch to for the duration of the stroke */
enum class BrushSwitchMode : int8_t {
  None = 0,
  Smooth = 1,
  Erase = 2,
  Mask = 3,
};

namespace ed::sculpt_paint {

/* stroke operator */
#define PAINT_STROKE_MODAL_CANCEL 1

struct PaintSample {
  float2 mouse = float2(0.0f, 0.0f);
  float pressure = 0.0f;
};

/** Pen touch-down and lift-off report near-zero pressure; roll points and
 * the dab spacing clamp to this. */
inline constexpr float MIN_ROLL_PRESSURE = 0.05f;

/** Ring buffer size for recorded roll points (history + virtual extension). */
inline constexpr int ROLL_MAX_STROKE_POINTS = 512;

/**
 * Polyline-based arc-length parameterized spline for roll texture mapping.
 * Uses raw stroke knots as the polyline — smoothing is handled by the
 * subdivided grid + Laplacian smoothing in compute_roll_center().
 */
struct RollSpline {
  Vector<float2> poly_2d;
  Vector<float3> poly_3d;
  /** Cumulative 3D arc length at the end of each segment. */
  Vector<float> lengths_3d;
  /** Smooth tangents at each polyline vertex (central-difference). */
  Vector<float3> tangents_3d;
  /** Per-vertex pen pressure (0..1). Used to compute variable strip width. */
  Vector<float> pressures;
  /** Surface normal per vertex, frozen when the point was recorded. */
  Vector<float3> normals;

  void clear();
  bool is_empty() const;
  void update_lengths();

  /** Length of segment `seg_idx` (difference of consecutive cumulative lengths). */
  float segment_length_3d(int seg_idx) const
  {
    return lengths_3d[seg_idx] - (seg_idx > 0 ? lengths_3d[seg_idx - 1] : 0.0f);
  }

  float2 tangent_2d_at_index(int poly_idx) const;

  /** Arc length of the closest point on the 3D polyline to `query`. */
  float closest_point_arc_length(const float3 &query) const;
};

struct PaintStrokePoint {
  float2 mouse_in;
  float2 mouse_out;
  float3 location;
  /** Object-space surface normal at `location`, frozen at recording time. */
  float3 surface_normal = float3(0, 0, 1);
  float pressure = 0.0f;
  float x_tilt = 0.0f;
  float y_tilt = 0.0f;
  bool pen_flip = false;
  float size = 0.0f;
};

/**
 * Common structure for various paint operators (e.g. Sculpt, Grease Pencil, Curves Sculpt)
 *
 * Callback functions defined and stored on this struct (e.g. `StrokeGetLocation`) allow each of
 * these modes to customize specific behavior while still sharing other common handing.
 *
 * See #paint_stroke_modal for the majority of the paint operator logic.
 */
struct PaintStroke : NonCopyable, NonMovable {
 public:
  struct StrokeStep {
    float size;
    float3 location;
    float2 mouse;
    float2 mouse_event;
    float pressure;
    float2 tilt;
  };
  /* TODO: Temporary, used to assist removing usage of bContext in PaintStroke callbacks.
   * See #149378 */
  bContext *evil_C = nullptr;

  /* Cached values */
  ViewContext vc = {};
  Depsgraph *depsgraph = nullptr;
  Object *object = nullptr;
  Scene *scene = nullptr;
  Paint *paint = nullptr;
  Brush *brush = nullptr;
  UnifiedPaintSettings *ups = nullptr;

  /* TODO: These are only public so that cursor drawing code can use them. Find a better place. */
  float2 last_mouse_position = float2(0.0f, 0.0f);
  bool constrain_line = false;
  float2 constrained_pos = float2(0.0f, 0.0f);

 protected:
  std::unique_ptr<PaintModeData> mode_data_ = nullptr;

 private:
  void *stroke_cursor_ = nullptr;

  wmTimer *timer_ = nullptr;
  std::optional<RandomNumberGenerator> rng_ = std::nullopt;

  /* Paint stroke can use up to PAINT_MAX_INPUT_SAMPLES prior inputs
   * to smooth the stroke */
  PaintSample samples_[PAINT_MAX_INPUT_SAMPLES];
  int num_samples_ = 0;
  int cur_sample_ = 0;
  int tot_samples_ = 0;

  float3 last_world_space_position_ = float3(0.0f, 0.0f, 0.0f);
  float3 last_scene_spacing_delta_ = float3(0.0f, 0.0f, 0.0f);

  bool stroke_over_mesh_ = false;
  /* space distance covered so far */
  float stroke_distance_ = 0.0f;
  float stroke_distance_world_ = 0.0f;

  /* Roll texture mapping */
  bool need_roll_mapping_ = false;
  bool roll_virtual_prepended_ = false; /* true after virtual backward segments are prepended */
  int n_virtual_poly_points_ = 0;       /* polyline points in virtual backward extension */
  float roll_virtual_length_ = 0.0f;    /* arc length of virtual extension (subtracted from V) */
  float roll_initial_radius_ = 0.0f;    /* cache.initial_radius, captured on first dab */
  float3 roll_proj_normal_ = float3(0); /* projection normal, frozen on first dab */
  float stroke_distance_normalized_ = 0.0f; /* pressure-normalized V offset for consumed knots */
  float roll_virtual_length_normalized_ = 0.0f; /* normalized arc length of virtual extension */
  int initial_backward_ext_count_ = 0; /* backward_ext knot count at creation, for budget */
  void *roll_cursor_ = nullptr;        /* always-on preview of unflushed spline portion */
  void *debug_cursor_ = nullptr;
  int stroke_sample_index_ = 0;
  int last_painted_roll_idx_ = -1; /* ring buffer index of the last deferred dab placed */
  float spacing_raw_ = 0.0f;
  PaintStrokePoint points_[ROLL_MAX_STROKE_POINTS];
  int num_points_ = 0;
  int cur_point_ = 0;
  RollSpline roll_spline_;
  Vector<float2> backward_ext_2d_; /* virtual backward extension knots (screen space) */
  Vector<float3> backward_ext_3d_; /* virtual backward extension knots (world space) */

  /* Set whether any stroke step has yet occurred
   * e.g. in sculpt mode, stroke doesn't start until cursor
   * passes over the mesh */
  bool stroke_started_ = false;
  /* Set when enough motion was found for rake rotation */
  bool rake_started_ = false;
  /* event that started stroke, for modal() return */
  int event_type_ = 0;
  /* check if stroke variables have been initialized */
  bool stroke_init_ = false;
  /** Check if input variables have been initialized (e.g. cursor position & pressure). */
  bool input_init_ = false;
  float2 initial_mouse_ = float2(0.0f, 0.0f);
  float cached_size_pressure_ = 0.0f;
  /* last pressure will store last pressure value for use in interpolation for space strokes */
  float last_pressure_ = 0.0f;
  /* last smoothed mouse position (for stabilize stroke finalization) */
  float2 last_smoothed_mouse_ = float2(0.0f, 0.0f);
  BrushStrokeMode stroke_mode_ = BrushStrokeMode::Normal;
  BrushSwitchMode brush_switch_mode_ = BrushSwitchMode::None;

  float last_tablet_event_pressure_ = 0.0f;

  float zoom_2d_ = 0.0f;
  bool pen_flip_ = false;

  /* Tilt, as read from the event. */
  float2 tilt_ = float2(0.0f, 0.0f);

  bool original_ = false; /* Ray-cast original mesh at start of stroke. */

 public:
  PaintStroke() = delete;
  virtual ~PaintStroke() = default;

  /**
   * The main modal callback shared by any custom operator that implements a form of painting.
   *
   * At a high level, this function performs the following steps for interactive stroke types:
   * 1. Initialization of necessary common `PaintStroke` values.
   * 2. Custom paint initialization via `test_start`>
   * 3. Create an `OperatorStrokeElement` for a given mouse position by calling `get_location`
   *    to potentially turn screen space coordinates into object space coordinates.
   * 4. Call `update_step` to perform custom paint operation on the most recent
   *    `OperatorStrokeElement` data.
   * 5. Tag extra redraws if necessary via `redraw`.
   * 6. Return to step 3 while stroke is ongoing.
   * 7. Call `done` when finished to perform any cleanup or finalization.
   */
  wmOperatorStatus modal(bContext *C, wmOperator *op, const wmEvent *event);
  wmOperatorStatus exec(bContext *C, wmOperator *op);

  /** Cancel a stroke and return to the initial state.
   *
   * \note Typically handled as part of modal operator actions. Consumers of this API may need
   * to call this if returning OPERATOR_CANCELLED during the `invoke` operator callback.
   */
  void cancel(bContext *C);
  /** Finish a stroke, performing any necessary cleanup actions.
   *
   * \note Typically handled as part of modal operator actions. Consumers of this API may need
   * to call this if returning OPERATOR_FINISHED during the `invoke` operator callback.
   */
  void finish(bContext *C);

  /* TODO: The following accessors should all be parameters passed into various callbacks */
  bool stroke_flipped() const
  {
    return pen_flip_;
  }

  bool stroke_inverted() const
  {
    return stroke_mode_ == BrushStrokeMode::Invert;
  }

  float stroke_distance() const
  {
    return stroke_distance_;
  }

  /**
   * Roll UV for an object-space position, sampled from the per-dab LUT.
   * \param r_out: [0] across-strip U (±1 at the borders, sign flipped for
   * texture X; FLT_MAX when unmappable), [1] along-stroke V including the
   * continuity offsets, [2] unused.
   * \param r_tan: Along-stroke tangent at the sample.
   */
  void spline_uv(const StrokeCache &cache,
                 const float co[3],
                 float r_out[3],
                 float r_tan[3]) const;

  /** Returns true when roll texture mapping is active for this stroke. */
  bool need_roll_mapping() const
  {
    return need_roll_mapping_;
  }

  /** Build the strip (borders, collapse, grid, LUT) for the current dab.
   * Once per dab, before the per-vertex loop. */
  void compute_roll_center(StrokeCache &cache);

  /** Debug: draw the roll spline overlay in the viewport. */
  void draw_debug_roll(bContext *C) const;

  /** Draw the unflushed portion of the roll spline as an always-on preview. */
  void draw_roll_preview(bContext *C) const;

 protected:
  PaintStroke(bContext *C, wmOperator *op, const wmEvent *event);

  /**
   * Callback function to retrieve the object space coordinates based on screen space coordinates.
   * \returns whether a value was actually found & the value in location is usable
   */
  virtual std::optional<float3> get_location(float2 mouse, bool force_original) = 0;

  /**
   * Callback function to determine whether a stroke has started, and performing initialization.
   *
   * In many cases, this is a check to whether the stroke is over the active mesh.
   */
  virtual bool test_start(wmOperator *op, float2 mouse) = 0;

  /**
   * Callback function for performing a paint stroke for a new step.
   */
  virtual void update_step(wmOperator *op, const StrokeStep &stroke_step) = 0;

  /**
   * Callback function for performing necessary redraw functions based on the stroke.
   */
  virtual void redraw(bool final) = 0;

  /**
   * Callback function for dynamically determining if a stroke can be cancelled.
   */
  virtual bool test_cancel() = 0;

  /**
   * Callback function for cleaning up and finalizing data after a stroke has finished.
   *
   * \param is_cancel: Some paint modes support cancelling a stroke and returning to the initial
   * state. This parameter indicates this case so that appropriate cleanup actions can be taken.
   * \param stroke_started: Whether the stroke started. Subclasses can use this to determine if
   * undo steps should be created. See \test_start.
   */
  virtual void done(bool is_cancel, bool stroke_started) = 0;

 private:
  void done(bContext *C, bool is_cancel);
  bool update(bContext *C,
              const Brush &brush,
              PaintMode mode,
              const float mouse_init[2],
              float mouse[2],
              float pressure,
              float r_location[3],
              bool *r_location_is_set);

  /** Recorded points per brush radius of stroke (from the brush spacing). */
  int roll_points_per_radius() const;
  /** Stroke history kept for the roll strip, in recorded points. */
  int roll_max_points() const;
  /** Pressure Scale option: the texture shrinks with the strip width. */
  bool roll_texture_pressure_scaled() const;
  /** Dabs lag this many points behind the pen (about three radii). */
  int roll_half_points() const
  {
    return roll_points_per_radius() * 3 + 2;
  }
  void add_roll_point(const float2 &mouse_in,
                      const float2 &mouse_out,
                      const float3 &loc,
                      const float3 &surface_normal,
                      float size,
                      float pressure,
                      bool pen_flip,
                      float x_tilt,
                      float y_tilt);
  /** Object-space surface normal under `mouse`, for freezing into a roll point. */
  float3 roll_sample_surface_normal(bContext *C, const float2 &mouse) const;
  void prepend_virtual_roll_points();
  void make_roll_spline(bContext *C);
  void finish_roll_stroke(bContext *C, wmOperator *op, const float2 &mouse_up, float pressure);
  void init_roll_cursors();

  void add_step(bContext *C, wmOperator *op, float2 mval, float pressure);

  void add_sample(int input_samples, float x, float y, float pressure);
  void calc_average_sample(PaintSample *average);

  void lines_spacing(bContext *C,
                     wmOperator *op,
                     float spacing,
                     float *length_residue,
                     float2 old_pos,
                     float2 new_pos);
  int space_stroke(bContext *C, wmOperator *op, float2 final_mouse, float final_pressure);

  void line_end(bContext *C, wmOperator *op, float2 mouse);
  bool curve_end(bContext *C, wmOperator *op);
};

float2 paint_stroke_jitter_pos(Paint *paint,
                               PaintMode mode,
                               const Brush &brush,
                               float pressure,
                               BrushStrokeMode stroke_mode,
                               float zoom_2d,
                               const float2 &mval);

/**
 * Returns true if the stroke dots should be spaced.
 */
bool paint_space_stroke_enabled(const Brush &br, PaintMode mode);
/**
 * Return true if the brush size can change during paint (normally used for pressure).
 */
bool paint_supports_dynamic_size(const Brush &br, PaintMode mode);
/**
 * Return true if the brush size can change during paint (normally used for pressure).
 */
bool paint_supports_dynamic_tex_coords(const Brush &br, PaintMode mode);
bool paint_supports_smooth_stroke(const Brush &brush,
                                  PaintMode mode,
                                  BrushSwitchMode brush_switch_mode);
bool paint_supports_texture(PaintMode mode);

class PaintModeData {
 public:
  virtual ~PaintModeData() = default;
};

/** Returns true if the active tool uses brushes. */
bool paint_brush_tool_poll(bContext *C);
bool paint_brush_tool_poll(const ScrArea *area,
                           const ARegion *region,
                           const Paint *paint,
                           const Object *ob);
/** Returns true if the brush cursor should be activated. */
bool paint_brush_cursor_poll(bContext *C);

void BRUSH_OT_asset_activate(wmOperatorType *ot);
void BRUSH_OT_asset_save_as(wmOperatorType *ot);
void BRUSH_OT_asset_edit_metadata(wmOperatorType *ot);
void BRUSH_OT_asset_load_preview(wmOperatorType *ot);
void BRUSH_OT_asset_delete(wmOperatorType *ot);
void BRUSH_OT_asset_save(wmOperatorType *ot);
void BRUSH_OT_asset_revert(wmOperatorType *ot);

}  // namespace ed::sculpt_paint

/** Initialize viewport pivot from evaluated bounding box center of `ob`. */
void paint_init_pivot(Object *ob, Scene *scene, Paint *paint);

/**
 * Delete overlay cursor textures to preserve memory and invalidate all overlay flags.
 */
void paint_cursor_delete_textures();

/* `paint_vertex.cc` */

bool weight_paint_poll(bContext *C);
bool weight_paint_poll_ignore_tool(bContext *C);
bool weight_paint_mode_poll(bContext *C);
bool weight_paint_mode_region_view3d_poll(bContext *C);
bool vertex_paint_poll(bContext *C);
bool vertex_paint_poll_ignore_tool(bContext *C);
bool vertex_paint_mode_poll(bContext *C);

void PAINT_OT_weight_paint_toggle(wmOperatorType *ot);
void PAINT_OT_weight_paint(wmOperatorType *ot);
void PAINT_OT_weight_set(wmOperatorType *ot);

void PAINT_OT_weight_gradient(wmOperatorType *ot);

void PAINT_OT_vertex_paint_toggle(wmOperatorType *ot);
void PAINT_OT_vertex_paint(wmOperatorType *ot);

/* `paint_vertex_weight_utils.cc` */

/**
 * \param weight: Typically the current weight: #MDeformWeight.weight
 *
 * \return The final weight, note that this is _not_ clamped from [0-1].
 * Clamping must be done on the final #MDeformWeight.weight
 */
float ED_wpaint_blend_tool(int tool, float weight, float paintval, float alpha);
/* Utility for tools to ensure vertex groups exist before they begin. */
enum eWPaintFlag {
  WPAINT_ENSURE_MIRROR = (1 << 0),
};
struct WPaintVGroupIndex {
  int active;
  int mirror;
};
/**
 * Ensure we have data on wpaint start, add if needed.
 */
bool ED_wpaint_ensure_data(bContext *C,
                           ReportList *reports,
                           eWPaintFlag flag,
                           WPaintVGroupIndex *vgroup_index);
bool ED_wpaint_ensure_data(bContext *C,
                           Main *bmain,
                           Object *object,
                           ReportList *reports,
                           eWPaintFlag flag,
                           WPaintVGroupIndex *vgroup_index);
/** Return -1 when invalid. */
int ED_wpaint_mirror_vgroup_ensure(Object *ob, int vgroup_active);

/* `paint_vertex_color_ops.cc` */

void PAINT_OT_vertex_color_set(wmOperatorType *ot);
void PAINT_OT_vertex_color_from_weight(wmOperatorType *ot);
void PAINT_OT_vertex_color_smooth(wmOperatorType *ot);
void PAINT_OT_vertex_color_brightness_contrast(wmOperatorType *ot);
void PAINT_OT_vertex_color_hsv(wmOperatorType *ot);
void PAINT_OT_vertex_color_invert(wmOperatorType *ot);
void PAINT_OT_vertex_color_levels(wmOperatorType *ot);

/* `paint_vertex_weight_ops.cc` */

void PAINT_OT_weight_from_bones(wmOperatorType *ot);
void PAINT_OT_weight_sample(wmOperatorType *ot);
void PAINT_OT_weight_sample_group(wmOperatorType *ot);

/* `paint_image.cc` */

struct ImagePaintPartialRedraw {
  rcti dirty_region;
};

bool image_texture_paint_poll(bContext *C);
bool image_paint_poll_ignore_tool(bContext *C);
void imapaint_image_update(ImBuf *ibuf);
ImagePaintPartialRedraw *get_imapaintpartial();
void set_imapaintpartial(ImagePaintPartialRedraw *ippr);
void imapaint_region_tiles(
    ImBuf *ibuf, int x, int y, int w, int h, int *tx, int *ty, int *tw, int *th);
bool get_imapaint_zoom(bContext *C, float *zoomx, float *zoomy);
void *paint_2d_new_stroke(bContext *, wmOperator *, BrushStrokeMode mode);
void paint_2d_redraw(const bContext *C, void *ps, bool final);
void paint_2d_stroke_done(void *ps, wmPaintCursor *cursor);
void paint_2d_stroke(void *ps,
                     const float prev_mval[2],
                     const float mval[2],
                     bool eraser,
                     float pressure,
                     float distance,
                     float base_size);
/**
 * This function expects sRGB space color values.
 */
void paint_2d_bucket_fill(const bContext *C,
                          const float color[3],
                          Brush *br,
                          const float mouse_init[2],
                          const float mouse_final[2],
                          void *ps);
void paint_2d_gradient_fill(
    const bContext *C, Brush *br, const float mouse_init[2], const float mouse_final[2], void *ps);
void *paint_proj_new_stroke(bContext *C,
                            Object *ob,
                            const float mouse[2],
                            BrushStrokeMode mode,
                            BrushSwitchMode brush_switch_mode);
void paint_proj_stroke(const bContext *C,
                       void *ps_handle_p,
                       const float prev_pos[2],
                       const float pos[2],
                       bool eraser,
                       float pressure,
                       float distance,
                       float size);
void paint_proj_redraw(const bContext *C, void *ps_handle_p, bool final);
void paint_proj_stroke_done(void *ps_handle_p, wmPaintCursor *cursor);

void paint_brush_color_get(const Paint *paint,
                           Brush *br,
                           std::optional<float3> &initial_hsv_jitter,
                           bool invert,
                           float distance,
                           float pressure,
                           float r_color[3]);
bool paint_use_opacity_masking(const Paint *paint, const Brush *brush);
void paint_brush_init_tex(Brush *brush);
void paint_brush_exit_tex(Brush *brush);

void PAINT_OT_grab_clone(wmOperatorType *ot);
namespace ed::sculpt_paint {
void PAINT_OT_sample_color(wmOperatorType *ot);
}
void PAINT_OT_brush_colors_flip(wmOperatorType *ot);
void PAINT_OT_texture_paint_toggle(wmOperatorType *ot);
void PAINT_OT_project_image(wmOperatorType *ot);
void PAINT_OT_image_from_view(wmOperatorType *ot);
void PAINT_OT_add_texture_paint_slot(wmOperatorType *ot);
void PAINT_OT_image_paint(wmOperatorType *ot);
void PAINT_OT_add_simple_uvs(wmOperatorType *ot);

/* paint_image_2d_curve_mask.cc */

/**
 * \brief Caching structure for curve mask.
 *
 * When 2d painting images the curve mask is used as an input.
 */
struct CurveMaskCache {
  /**
   * \brief Last #CurveMapping.changed_timestamp being read.
   *
   * When different the input cache needs to be recalculated.
   */
  int last_curve_timestamp;

  /**
   * \brief sampled version of the brush curve-mapping.
   */
  float *sampled_curve;

  /**
   * \brief Size in bytes of the curve_mask field.
   *
   * Used to determine if the curve_mask needs to be re-allocated.
   */
  size_t curve_mask_size;

  /**
   * \brief Curve mask that can be passed as curve_mask parameter when.
   */
  ushort *curve_mask;
};

void paint_curve_mask_cache_free_data(CurveMaskCache *curve_mask_cache);
void paint_curve_mask_cache_update(CurveMaskCache *curve_mask_cache,
                                   const Brush *brush,
                                   int diameter,
                                   float radius,
                                   const float cursor_position[2]);

/* `sculpt_uv.cc` */

void SCULPT_OT_uv_sculpt_grab(wmOperatorType *ot);
void SCULPT_OT_uv_sculpt_relax(wmOperatorType *ot);
void SCULPT_OT_uv_sculpt_pinch(wmOperatorType *ot);

/* paint_utils.cc */
float paint_calc_object_space_radius(const ViewContext &vc,
                                     const float3 &center,
                                     float pixel_radius);

/**
 * Returns true when a color was sampled and false when a value was sampled.
 */
bool paint_get_tex_pixel(const MTex *mtex,
                         float u,
                         float v,
                         ImagePool *pool,
                         int thread,
                         float *r_intensity,
                         float r_rgba[4]);

void paint_stroke_operator_properties(wmOperatorType *ot);

void PAINT_OT_face_select_linked(wmOperatorType *ot);
void PAINT_OT_face_select_linked_pick(wmOperatorType *ot);
void PAINT_OT_face_select_all(wmOperatorType *ot);
void PAINT_OT_face_select_more(wmOperatorType *ot);
void PAINT_OT_face_select_less(wmOperatorType *ot);
void PAINT_OT_face_select_hide(wmOperatorType *ot);
void PAINT_OT_face_select_loop(wmOperatorType *ot);

void PAINT_OT_face_vert_reveal(wmOperatorType *ot);

void PAINT_OT_vert_select_all(wmOperatorType *ot);
void PAINT_OT_vert_select_ungrouped(wmOperatorType *ot);
void PAINT_OT_vert_select_hide(wmOperatorType *ot);
void PAINT_OT_vert_select_linked(wmOperatorType *ot);
void PAINT_OT_vert_select_linked_pick(wmOperatorType *ot);
void PAINT_OT_vert_select_more(wmOperatorType *ot);
void PAINT_OT_vert_select_less(wmOperatorType *ot);
void PAINT_OT_vert_select_loop(wmOperatorType *ot);

bool facemask_paint_poll(bContext *C);
bool vert_paint_poll(bContext *C);
bool mask_paint_poll(bContext *C);
bool paint_curve_poll(bContext *C);

namespace ed::sculpt_paint {

/**
 * Determines whether a given symmetry pass is valid.
 *
 * Uses the #ePaintSymmetryFlags enum.
 *
 * symm is a bit combination of XYZ.
 * 1 is X; 2 is Y; 3 is XY; 4 is Z; 5 is XZ; 6 is YZ; 7 is XYZ
 */
inline bool is_symmetry_iteration_valid(const char i, const char symm)
{
  return i == 0 || (symm & i && (symm != 5 || i != 3) && (symm != 6 || !ELEM(i, 3, 5)));
}

inline float3 symmetry_flip(const float3 &src, const ePaintSymmetryFlags symm)
{
  float3 dst;
  if (symm & PAINT_SYMM_X) {
    dst.x = -src.x;
  }
  else {
    dst.x = src.x;
  }
  if (symm & PAINT_SYMM_Y) {
    dst.y = -src.y;
  }
  else {
    dst.y = src.y;
  }
  if (symm & PAINT_SYMM_Z) {
    dst.z = -src.z;
  }
  else {
    dst.z = src.z;
  }
  return dst;
}

}  // namespace ed::sculpt_paint

/* `paint_curve.cc` */
#define PAINT_CURVE_NUM_SEGMENTS 40

void PAINTCURVE_OT_new(wmOperatorType *ot);
void PAINTCURVE_OT_add_point(wmOperatorType *ot);
void PAINTCURVE_OT_delete_point(wmOperatorType *ot);
void PAINTCURVE_OT_select(wmOperatorType *ot);
void PAINTCURVE_OT_slide(wmOperatorType *ot);
void PAINTCURVE_OT_draw(wmOperatorType *ot);
void PAINTCURVE_OT_cursor(wmOperatorType *ot);

/* image painting blur kernel */
struct BlurKernel {
  float *wdata;     /* actual kernel */
  int side;         /* kernel side */
  int side_squared; /* data side */
  int pixel_len;    /* pixels around center that kernel is wide */
};

/**
 * Paint blur kernels. Projective painting enforces use of a 2x2 kernel due to lagging.
 * Can be extended to other blur kernels later,
 */
BlurKernel *paint_new_blur_kernel(Brush *br, bool proj);
void paint_delete_blur_kernel(BlurKernel *);

/* palette.cc */

void PALETTE_OT_new(wmOperatorType *ot);
void PALETTE_OT_color_add(wmOperatorType *ot);
void PALETTE_OT_color_delete(wmOperatorType *ot);

void PALETTE_OT_extract_from_image(wmOperatorType *ot);
void PALETTE_OT_sort(wmOperatorType *ot);
void PALETTE_OT_color_move(wmOperatorType *ot);
void PALETTE_OT_join(wmOperatorType *ot);

}  // namespace blender
