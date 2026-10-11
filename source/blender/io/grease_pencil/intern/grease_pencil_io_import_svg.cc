/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_main.hh"
#include "BLI_assert.hh"
#include "BLI_bounds.hh"
#include "BLI_color_types.hh"
#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_color_c.hh"
#include "BLI_math_euler_types.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_rotation_c.hh"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.hh"
#include "BLI_vector.hh"

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_report.hh"

#include "DNA_grease_pencil_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "ED_grease_pencil.hh"

#include "grease_pencil_io_intern.hh"

#include <thorvg.h>

#ifdef WITH_PUGIXML
#  include <pugixml.hpp>
#endif

#include <fmt/core.h>
#include <fmt/format.h>
#include <memory>
#include <string>

namespace blender {

/** \file
 * \ingroup bgrease_pencil
 */

using bke::greasepencil::Drawing;
using bke::greasepencil::Layer;
using bke::greasepencil::TreeNode;

namespace io::grease_pencil {

class SVGImporter : public GreasePencilImporter {
 public:
  using GreasePencilImporter::GreasePencilImporter;

  bool read(StringRefNull filepath);
};

/* -------------------------------------------------------------------- */
/** \name ThorVG Lifetime Helpers
 * \{ */

/* ThorVG keeps a reference count on init/term, so this is safe even if another part of Blender
 * has already initialized the engine. */
struct ThorVGScope {
  bool ok = false;

  ThorVGScope()
  {
    ok = (tvg::Initializer::init(0) == tvg::Result::Success);
  }
  ~ThorVGScope()
  {
    if (ok) {
      tvg::Initializer::term();
    }
  }
};

struct PictureDeleter {
  void operator()(tvg::Picture *picture) const
  {
    /* The destructor of tvg::Paint is protected; `rel` is the supported way to release. */
    tvg::Paint::rel(picture);
  }
};
using PicturePtr = std::unique_ptr<tvg::Picture, PictureDeleter>;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Intermediate Shape Representation
 * \{ */

/* One Bezier control point with its two handles (all in SVG user space, then in SVG pixels after
 * the world transform has been applied). */
struct SvgKnot {
  float2 pos;
  float2 handle_left;
  float2 handle_right;
};

struct SvgSubPath {
  Vector<SvgKnot> knots;
  bool closed = false;
};

struct SvgShape {
  Vector<SvgSubPath, 0> paths;
  ColorGeometry4f fill_color = ColorGeometry4f(0, 0, 0, 1);
  ColorGeometry4f stroke_color = ColorGeometry4f(0, 0, 0, 1);
  bool use_fill = false;
  bool use_stroke = false;
  bool has_gradient = false;
  /* Stroke width in SVG pixels, world scale already applied. */
  float stroke_width = 0.0f;
  /* Name of the closest ancestor group that has an `id` (empty if none). */
  std::string parent_id;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Matrix / Color Helpers
 * \{ */

static float3x3 tvg_matrix_to_blender(const tvg::Matrix &m)
{
  float3x3 mat;
  mat[0][0] = m.e11;
  mat[0][1] = m.e12;
  mat[0][2] = m.e13;
  mat[1][0] = m.e21;
  mat[1][1] = m.e22;
  mat[1][2] = m.e23;
  mat[2][0] = m.e31;
  mat[2][1] = m.e32;
  mat[2][2] = m.e33;
  return mat;
}

static float2 matrix_apply(const float3x3 &m, const float2 &p)
{
  const float3 v = m * float3(p.x, p.y, 1.0f);
  return float2(v.x, v.y);
}

static float matrix_uniform_scale(const float3x3 &m)
{
  return math::sqrt(math::abs(m[0][0] * m[1][1] - m[0][1] * m[1][0]));
}

static float3x3 paint_matrix(const tvg::Paint &paint)
{
  /* `Paint::transform()` is not const-qualified in the ThorVG API, but it only reads here. */
  const tvg::Matrix m = const_cast<tvg::Paint &>(paint).transform();
  return tvg_matrix_to_blender(m);
}

/* Convert an 8-bit sRGB color (as stored by ThorVG) to Blender's linear float color.
 * `opacity` is multiplied into the alpha channel. */
static ColorGeometry4f color_from_rgba8(
    const uint8_t r, const uint8_t g, const uint8_t b, const uint8_t a, const float opacity)
{
  const float4 rgb_f = {
      float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, (float(a) / 255.0f) * opacity};
  ColorGeometry4f color;
  srgb_to_linearrgb_v4(color, rgb_f);
  return color;
}

/* Simple approximation of a gradient by a single color. */
static ColorGeometry4f average_gradient_color(const tvg::Fill &gradient, const float opacity)
{
  const tvg::Fill::ColorStop *stops = nullptr;
  const uint32_t stops_num = gradient.colorStops(&stops);
  if (stops == nullptr || stops_num == 0) {
    return ColorGeometry4f(0, 0, 0, 0);
  }

  float4 avg_color = float4(0, 0, 0, 0);
  for (const uint32_t i : IndexRange(stops_num)) {
    avg_color += float4(color_from_rgba8(stops[i].r, stops[i].g, stops[i].b, stops[i].a, opacity));
  }
  avg_color /= float(stops_num);
  return ColorGeometry4f(avg_color);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ThorVG Scene Traversal
 * \{ */

/* Close to nanosvg's behavior: a "no paint" color is opaque black. */
static const ColorGeometry4f no_paint_color = ColorGeometry4f(0, 0, 0, 1);

/* TODO: Gradients are not yet fully supported (approximated by average color).
 * Fill gradients require unique materials per curve, and stroke gradients
 * can be baked into vertex colors in future passes. */
static void read_shape_style(const tvg::Shape &tvg_shape, const float opacity, SvgShape &shape)
{
  /* Fill. */
  {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    tvg_shape.fill(&r, &g, &b, &a);
    if (const tvg::Fill *gradient = tvg_shape.fill()) {
      shape.use_fill = true;
      shape.has_gradient = true;
      shape.fill_color = average_gradient_color(*gradient, opacity);
    }
    else if (a > 0) {
      shape.use_fill = true;
      shape.fill_color = color_from_rgba8(r, g, b, a, opacity);
    }
    else {
      shape.use_fill = false;
      shape.fill_color = no_paint_color;
    }
  }

  /* Stroke. A zero stroke width means "no stroke" in ThorVG. */
  {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    tvg_shape.strokeFill(&r, &g, &b, &a);
    const tvg::Fill *gradient = tvg_shape.strokeFill();
    shape.use_stroke = tvg_shape.strokeWidth() > 0.0f && (gradient != nullptr || a > 0);
    if (!shape.use_stroke) {
      shape.stroke_color = no_paint_color;
    }
    else if (gradient) {
      shape.has_gradient = true;
      shape.stroke_color = average_gradient_color(*gradient, opacity);
    }
    else {
      shape.stroke_color = color_from_rgba8(r, g, b, a, opacity);
    }
  }
}

/* Convert ThorVG command/point arrays into Bezier sub-paths. */
static void read_shape_paths(const tvg::Shape &tvg_shape, const float3x3 &world, SvgShape &shape)
{
  const tvg::PathCommand *cmds = nullptr;
  const tvg::Point *pts = nullptr;
  uint32_t cmds_num = 0;
  uint32_t pts_num = 0;
  if (tvg_shape.path(&cmds, &cmds_num, &pts, &pts_num) != tvg::Result::Success || cmds_num == 0) {
    return;
  }

  SvgSubPath current;
  bool has_current = false;

  auto to_float2 = [](const tvg::Point &p) { return float2(p.x, p.y); };

  auto flush = [&]() {
    if (has_current && current.knots.size() >= 2) {
      /* Apply world transform to positions and handles. */
      for (SvgKnot &knot : current.knots) {
        knot.pos = matrix_apply(world, knot.pos);
        knot.handle_left = matrix_apply(world, knot.handle_left);
        knot.handle_right = matrix_apply(world, knot.handle_right);
      }

      /* Merge closing endpoint into start point if spatially identical. */
      bool first_removed = true;
      while (current.knots.size() > 1 &&
             math::distance_squared(current.knots.first().pos, current.knots.last().pos) < 1e-8f)
      {
        if (first_removed) {
          current.knots.first().handle_left = current.knots.last().handle_left;
          first_removed = false;
        }
        current.knots.pop_last();
        current.closed = true;
      }

      shape.paths.append(std::move(current));
    }
    current = SvgSubPath();
    has_current = false;
  };

  auto begin_subpath = [&](const float2 &p) {
    current.knots.append({p, p, p});
    has_current = true;
  };

  uint32_t pt = 0;
  for (const uint32_t i : IndexRange(cmds_num)) {
    switch (cmds[i]) {
      case tvg::PathCommand::MoveTo: {
        if (pt + 1 > pts_num) {
          return;
        }
        flush();
        begin_subpath(to_float2(pts[pt]));
        pt += 1;
        break;
      }
      case tvg::PathCommand::LineTo: {
        if (pt + 1 > pts_num) {
          return;
        }
        const float2 p = to_float2(pts[pt]);
        pt += 1;
        if (!has_current) {
          begin_subpath(p);
          break;
        }
        current.knots.append({p, p, p});
        break;
      }
      case tvg::PathCommand::CubicTo: {
        if (pt + 3 > pts_num) {
          return;
        }
        const float2 c1 = to_float2(pts[pt + 0]);
        const float2 c2 = to_float2(pts[pt + 1]);
        const float2 p = to_float2(pts[pt + 2]);
        pt += 3;
        if (!has_current) {
          break;
        }
        current.knots.last().handle_right = c1;
        current.knots.append({p, c2, p});
        break;
      }
      case tvg::PathCommand::Close: {
        if (has_current) {
          current.closed = true;
          flush();
        }
        break;
      }
    }
  }
  flush();
}

static Map<uint32_t, std::string> read_group_names(const char *filepath, tvg::Accessor &accessor)
{
  Map<uint32_t, std::string> names;
#ifdef WITH_PUGIXML
  pugi::xml_document doc;
  if (doc.load_file(filepath)) {
    Vector<pugi::xml_node> stack;
    stack.append(doc.document_element());
    while (!stack.is_empty()) {
      const pugi::xml_node node = stack.pop_last();
      if (StringRef(node.name()) == "g") {
        const char *id = node.attribute("id").as_string("");
        if (id[0] != '\0') {
          names.add_overwrite(accessor.id(id), id);
        }
      }
      for (const pugi::xml_node child : node.children()) {
        stack.append(child);
      }
    }
  }
#else
  UNUSED_VARS(filepath, accessor);
#endif
  return names;
}

struct ShapeCollectContext {
  const Map<uint32_t, std::string> *names = nullptr;
  Vector<SvgShape, 0> shapes;
};

static bool collect_shapes_cb(const tvg::Paint *paint, void *data)
{
  ShapeCollectContext &ctx = *static_cast<ShapeCollectContext *>(data);
  if (paint->type() != tvg::Type::Shape) {
    return true;
  }
  const tvg::Shape &tvg_shape = *static_cast<const tvg::Shape *>(paint);

  float3x3 world = paint_matrix(*paint);
  float opacity = float(paint->opacity()) / 255.0f;
  bool visible = paint->visible();
  std::string parent_id;
  for (const tvg::Paint *parent = paint->parent(); parent; parent = parent->parent()) {
    world = paint_matrix(*parent) * world;
    opacity *= float(parent->opacity()) / 255.0f;
    visible = visible && parent->visible();
    if (parent_id.empty() && parent->type() == tvg::Type::Scene && parent->id != 0) {
      if (const std::string *name = ctx.names->lookup_ptr(parent->id)) {
        parent_id = *name;
      }
    }
  }
  if (!visible || opacity <= 0.0f) {
    return true;
  }

  SvgShape shape;
  read_shape_paths(tvg_shape, world, shape);
  if (shape.paths.is_empty()) {
    return true;
  }
  read_shape_style(tvg_shape, opacity, shape);
  shape.stroke_width = tvg_shape.strokeWidth() * matrix_uniform_scale(world);
  shape.parent_id = std::move(parent_id);

  ctx.shapes.append(std::move(shape));
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grease Pencil Conversion
 * \{ */

static std::string get_layer_id(const SvgShape &shape, const int prefix)
{
  return shape.parent_id.empty() ? fmt::format("Layer_{:03d}", prefix) :
                                   fmt::format("{:s}", shape.parent_id);
}

static IndexRange extend_curves_geometry(bke::CurvesGeometry &curves, const SvgShape &shape)
{
  const int old_curves_num = curves.curves_num();
  const int old_points_num = curves.points_num();
  const Span<int> old_offsets = curves.offsets();

  Vector<int> new_curve_offsets;
  for (const SvgSubPath &path : shape.paths) {
    if (path.knots.is_empty()) {
      continue;
    }
    new_curve_offsets.append(int(path.knots.size()));
  }
  if (new_curve_offsets.is_empty()) {
    return {};
  }
  new_curve_offsets.append(0);
  const OffsetIndices new_points_by_curve = offset_indices::accumulate_counts_to_offsets(
      new_curve_offsets, old_points_num);

  const IndexRange new_curves_range = {old_curves_num, new_points_by_curve.size()};
  const int curves_num = new_curves_range.one_after_last();
  const int points_num = new_points_by_curve.total_size() + old_points_num;

  Array<int> new_offsets(curves_num + 1);
  if (old_curves_num > 0) {
    new_offsets.as_mutable_span().slice(0, old_curves_num).copy_from(old_offsets.drop_back(1));
  }
  new_offsets.as_mutable_span()
      .slice(old_curves_num, new_curve_offsets.size())
      .copy_from(new_curve_offsets);

  curves.resize(points_num, curves_num);
  curves.offsets_for_write().copy_from(new_offsets);

  curves.tag_topology_changed();

  return new_curves_range;
}

static void shape_attributes_to_curves(bke::CurvesGeometry &curves,
                                       const SvgShape &shape,
                                       const IndexRange curves_range,
                                       const float4x4 &transform,
                                       const int shape_index,
                                       const int material_index)
{
  const float path_width_scale = 0.5f * math::average(math::to_scale(transform));
  const OffsetIndices points_by_curve = curves.points_by_curve();

  curves.curve_types_for_write().slice(curves_range).fill(CURVE_TYPE_BEZIER);
  curves.update_curve_types();

  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
  bke::SpanAttributeWriter<int> materials = attributes.lookup_or_add_for_write_span<int>(
      "material_index", bke::AttrDomain::Curve);
  MutableSpan<bool> cyclic = curves.cyclic_for_write();

  bke::SpanAttributeWriter fill_colors = attributes.lookup_or_add_for_write_span<ColorGeometry4f>(
      "fill_color", bke::AttrDomain::Curve);
  bke::SpanAttributeWriter<float> fill_opacities = attributes.lookup_or_add_for_write_span<float>(
      "fill_opacity", bke::AttrDomain::Curve);

  MutableSpan<float3> positions = curves.positions_for_write();
  MutableSpan<float3> handle_positions_left = curves.handle_positions_left_for_write();
  MutableSpan<float3> handle_positions_right = curves.handle_positions_right_for_write();
  MutableSpan<int8_t> handle_types_left = curves.handle_types_left_for_write();
  MutableSpan<int8_t> handle_types_right = curves.handle_types_right_for_write();
  bke::SpanAttributeWriter<float> radii = attributes.lookup_or_add_for_write_span<float>(
      "radius", bke::AttrDomain::Point);
  bke::SpanAttributeWriter<ColorGeometry4f> vertex_colors =
      attributes.lookup_or_add_for_write_span<ColorGeometry4f>("vertex_color",
                                                               bke::AttrDomain::Point);
  bke::SpanAttributeWriter<float> point_opacities = attributes.lookup_or_add_for_write_span<float>(
      "opacity", bke::AttrDomain::Point);

  materials.span.slice(curves_range).fill(material_index);
  const ColorGeometry4f shape_color = shape.fill_color;
  if (fill_colors) {
    fill_colors.span.slice(curves_range).fill(shape_color);
  }
  if (fill_opacities) {
    fill_opacities.span.slice(curves_range).fill(shape_color.a);
  }

  const bool use_stroke = shape.use_stroke;
  const bool use_fill = shape.use_fill;

  if (!use_stroke) {
    attributes.add<bool>("hide_stroke", bke::AttrDomain::Curve, bke::AttributeInitDefaultValue());
  }
  if (use_fill) {
    attributes.add<int>("fill_id", bke::AttrDomain::Curve, bke::AttributeInitDefaultValue());
  }

  bke::SpanAttributeWriter<bool> hide_stroke = attributes.lookup_for_write_span<bool>(
      "hide_stroke");
  bke::SpanAttributeWriter<int> fill_ids = attributes.lookup_for_write_span<int>("fill_id");
  if (hide_stroke) {
    hide_stroke.span.slice(curves_range).fill(!use_stroke);
    hide_stroke.finish();
  }
  if (fill_ids) {
    fill_ids.span.slice(curves_range).fill(use_fill ? shape_index + 1 : 0);
    fill_ids.finish();
  }

  const ColorGeometry4f point_color = shape.stroke_color;
  const float point_radius = shape.stroke_width * path_width_scale;

  int curve_index = curves_range.start();
  for (const SvgSubPath &path : shape.paths) {
    if (path.knots.is_empty()) {
      continue;
    }
    const IndexRange points = points_by_curve[curve_index];
    BLI_assert(points.size() == int64_t(path.knots.size()));

    cyclic[curve_index] = path.closed;

    for (const int i : points.index_range()) {
      const int point_index = points[i];
      const SvgKnot &knot = path.knots[i];

      positions[point_index] = math::transform_point(transform, float3(knot.pos, 0.0f));
      handle_positions_left[point_index] = math::transform_point(transform,
                                                                 float3(knot.handle_left, 0.0f));
      handle_positions_right[point_index] = math::transform_point(transform,
                                                                  float3(knot.handle_right, 0.0f));
      handle_types_left[point_index] = BEZIER_HANDLE_FREE;
      handle_types_right[point_index] = BEZIER_HANDLE_FREE;

      radii.span[point_index] = point_radius;

      if (vertex_colors) {
        vertex_colors.span[point_index] = point_color;
      }
      if (point_opacities) {
        point_opacities.span[point_index] = point_color.a;
      }
    }

    ++curve_index;
  }

  materials.finish();
  fill_colors.finish();
  fill_opacities.finish();
  radii.finish();
  vertex_colors.finish();
  point_opacities.finish();
  curves.tag_positions_changed();
  curves.tag_radii_changed();
}

static void shift_to_bounds_center(GreasePencil &grease_pencil)
{
  const std::optional<Bounds<float3>> bounds = [&]() {
    std::optional<Bounds<float3>> bounds;
    for (GreasePencilDrawingBase *drawing_base : grease_pencil.drawings()) {
      if (drawing_base->type != GP_DRAWING) {
        continue;
      }
      Drawing &drawing = reinterpret_cast<GreasePencilDrawing *>(drawing_base)->wrap();
      bounds = bounds::merge(bounds, drawing.strokes().bounds_min_max());
    }
    return bounds;
  }();
  if (!bounds) {
    return;
  }
  const float3 offset = -bounds->center();

  for (GreasePencilDrawingBase *drawing_base : grease_pencil.drawings()) {
    if (drawing_base->type != GP_DRAWING) {
      continue;
    }
    Drawing &drawing = reinterpret_cast<GreasePencilDrawing *>(drawing_base)->wrap();
    drawing.strokes_for_write().translate(offset);
    drawing.tag_positions_changed();
  }
}

/** \} */

bool SVGImporter::read(StringRefNull filepath)
{
  constexpr float svg_px_to_mm = 25.4f / 96.0f;

  char abs_filepath[FILE_MAX];
  STRNCPY(abs_filepath, filepath.c_str());
  BLI_path_abs(abs_filepath, BKE_main_blendfile_path_from_global());

  ThorVGScope thorvg;
  if (!thorvg.ok) {
    BKE_report(context_.reports, RPT_ERROR, "Could not initialize the SVG library");
    return false;
  }

  PicturePtr picture(tvg::Picture::gen());
  if (picture->load(abs_filepath) != tvg::Result::Success) {
    BKE_report(context_.reports, RPT_ERROR, "Could not open SVG");
    return false;
  }

  std::unique_ptr<tvg::Accessor> accessor(tvg::Accessor::gen());
  const Map<uint32_t, std::string> group_names = read_group_names(abs_filepath, *accessor);

  ShapeCollectContext shapes_ctx;
  shapes_ctx.names = &group_names;
  accessor->set(picture.get(), collect_shapes_cb, &shapes_ctx);

  char filename[FILE_MAX];
  BLI_path_split_file_part(abs_filepath, filename, ARRAY_SIZE(filename));
  object_ = create_object(filename);
  if (object_ == nullptr) {
    BKE_report(context_.reports, RPT_ERROR, "Unable to create new object");
    return false;
  }
  GreasePencil &grease_pencil = *id_cast<GreasePencil *>(object_->data);

  std::optional<int> mat_index_stroke;
  std::optional<int> mat_index_fill;
  std::optional<int> mat_index_both;

  const float scene_unit_scale = (context_.scene->unit.system != USER_UNIT_NONE &&
                                  params_.use_scene_unit) ?
                                     context_.scene->unit.scale_length :
                                     1.0f;
  const float svg_scale = 0.001f * svg_px_to_mm * scene_unit_scale * params_.scale;
  const float4x4 transform = math::scale(
      math::from_rotation<float4x4>(math::EulerXYZ(DEG2RAD(-90), 0, 0)), float3(svg_scale));

  bool has_color_gradient = false;

  std::string prv_id = "*";
  int prefix = 0;
  int shape_index = 0;
  for (const SvgShape &shape : shapes_ctx.shapes) {
    std::string layer_id = get_layer_id(shape, prefix);
    if (prv_id != layer_id) {
      prefix++;
      layer_id = get_layer_id(shape, prefix);
      prv_id = layer_id;
    }

    Layer &layer = [&]() -> Layer & {
      TreeNode *layer_node = grease_pencil.find_node_by_name(layer_id);
      if (layer_node && layer_node->is_layer()) {
        return layer_node->as_layer();
      }

      Layer &layer = grease_pencil.add_layer(layer_id);
      layer.as_node().flag |= GP_LAYER_TREE_NODE_USE_LIGHTS;
      return layer;
    }();

    Drawing *drawing = grease_pencil.get_drawing_at(layer, params_.frame_number);
    if (drawing == nullptr) {
      drawing = grease_pencil.insert_frame(layer, params_.frame_number);
      if (!drawing) {
        continue;
      }
    }

    const bool is_fill = shape.use_fill;
    const bool is_stroke = shape.use_stroke || !is_fill;
    int material_index = 0;
    if (is_stroke && is_fill) {
      if (!mat_index_both) {
        mat_index_both = create_material("Both");
      }
      material_index = *mat_index_both;
    }
    else if (is_stroke) {
      if (!mat_index_stroke) {
        mat_index_stroke = create_material("Stroke");
      }
      material_index = *mat_index_stroke;
    }
    else if (is_fill) {
      if (!mat_index_fill) {
        mat_index_fill = create_material("Fill");
      }
      material_index = *mat_index_fill;
    }

    if (shape.has_gradient) {
      has_color_gradient = true;
    }

    bke::CurvesGeometry &curves = drawing->strokes_for_write();
    const IndexRange new_curves_range = extend_curves_geometry(curves, shape);
    if (new_curves_range.is_empty()) {
      continue;
    }

    shape_attributes_to_curves(
        curves, shape, new_curves_range, transform, shape_index, material_index);
    drawing->strokes_for_write() = std::move(curves);
    drawing->tag_topology_changed();

    shape_index++;
  }

  if (params_.recenter_bounds) {
    shift_to_bounds_center(grease_pencil);
  }

  if (has_color_gradient) {
    BKE_report(context_.reports,
               RPT_WARNING,
               "SVG has gradients, Grease Pencil color will be approximated");
  }

  return true;
}

bool import_svg(const IOContext &context, const ImportParams &params, StringRefNull filepath)
{
  SVGImporter importer(context, params);
  return importer.read(filepath);
}

}  // namespace io::grease_pencil

}  // namespace blender
