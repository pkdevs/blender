/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"
#include "BLI_straight_skeleton.hh"
#include "BLI_vector.hh"

/* Debug output mode for tests:
 *   DebugOutputMode::NONE   (0) - No debug output (standard quiet test run, assert failures only).
 *   DebugOutputMode::STEPS  (1) - Step-by-step event loop progress.
 *   DebugOutputMode::OUTPUT (2) - Final straight skeleton output: vertices, edges, faces, and SVG
 * diagrams.
 *
 * Can also be overridden at runtime via the STRAIGHT_SKELETON_DEBUG environment variable:
 *   STRAIGHT_SKELETON_DEBUG=none / 0
 *   STRAIGHT_SKELETON_DEBUG=steps / 1
 *   STRAIGHT_SKELETON_DEBUG=output / 2
 */
enum class DebugOutputMode {
  NONE = 0,
  STEPS = 1,
  OUTPUT = 2,
};

static constexpr DebugOutputMode debug_output_mode = DebugOutputMode::NONE;

/* SVG drawing mode:
 * Controlled by default_enable_svg_drawing or overridden via STRAIGHT_SKELETON_SVG env var:
 *   STRAIGHT_SKELETON_SVG=1 / on / true
 * Also enabled if STRAIGHT_SKELETON_DEBUG=output / 2.
 */
static constexpr bool default_enable_svg_drawing = false;

static bool is_svg_drawing_enabled()
{
  if (const char *env = std::getenv("STRAIGHT_SKELETON_SVG")) {
    const std::string_view s(env);
    if (s == "1" || s == "true" || s == "on") {
      return true;
    }
    if (s == "0" || s == "false" || s == "off") {
      return false;
    }
  }
  return default_enable_svg_drawing;
}

namespace blender::straightskeleton::test {

/* -------------------------------------------------------------------- */
/** \name SVG Debug Drawings
 * \{ */

inline bool &get_debug_new_file()
{
  static bool debug_new_file = true;
  return debug_new_file;
}

inline void write_debug_svg(const char *test_name,
                            const StraightSkeletonInput &input,
                            const StraightSkeletonOutput &output,
                            const char *html_filepath = "/tmp/straight_skeleton.html")
{
  const OffsetIndices<int> face_offsets = output.face_offsets_data.is_empty() ?
                                              output.face_offsets :
                                              OffsetIndices<int>(
                                                  output.face_offsets_data.as_span());

  /* Build the inline SVG content into a string first, then append it to the HTML file. */

  /* Compute bounding box of input vertices. */
  double in_xmin = 1e30, in_xmax = -1e30, in_ymin = 1e30, in_ymax = -1e30;
  for (const double2 &v : input.vert) {
    in_xmin = std::min(in_xmin, v.x);
    in_xmax = std::max(in_xmax, v.x);
    in_ymin = std::min(in_ymin, v.y);
    in_ymax = std::max(in_ymax, v.y);
  }
  if (in_xmin > in_xmax) {
    in_xmin = 0.0;
    in_xmax = 1.0;
    in_ymin = 0.0;
    in_ymax = 1.0;
  }
  double in_dx = in_xmax - in_xmin;
  double in_dy = in_ymax - in_ymin;
  if (in_dx < 1e-4) {
    in_dx = 1.0;
  }
  if (in_dy < 1e-4) {
    in_dy = 1.0;
  }

  /* Expand bounding box to include nearby output vertices, filtering out distant ray
   * intersections. */
  double xmin = in_xmin - 0.15 * in_dx;
  double xmax = in_xmax + 0.15 * in_dx;
  double ymin = in_ymin - 0.15 * in_dy;
  double ymax = in_ymax + 0.15 * in_dy;
  for (const double2 &v : output.vert) {
    if (v.x >= in_xmin - 1.0 * in_dx && v.x <= in_xmax + 1.0 * in_dx &&
        v.y >= in_ymin - 1.0 * in_dy && v.y <= in_ymax + 1.0 * in_dy)
    {
      xmin = std::min(xmin, v.x);
      xmax = std::max(xmax, v.x);
      ymin = std::min(ymin, v.y);
      ymax = std::max(ymax, v.y);
    }
  }

  double dx = xmax - xmin;
  double dy = ymax - ymin;
  const double pad = std::max(dx, dy) * 0.1;
  xmin -= pad;
  xmax += pad;
  ymin -= pad;
  ymax += pad;
  dx = xmax - xmin;
  dy = ymax - ymin;

  const double base_size = 500.0;
  const double scale = std::min(base_size / dx, base_size / dy);
  const double width = dx * scale + 100.0;
  const double height = dy * scale + 120.0;

  auto tx = [&](double x) { return 50.0 + (x - xmin) * scale; };
  auto ty = [&](double y) { return height - 50.0 - (y - ymin) * scale; };

  std::ostringstream svg;

  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << width << " " << height
      << "\" width=\"" << width << "\" height=\"" << height
      << "\" style=\"background-color:#ffffff; overflow:hidden; font-family: -apple-system, "
         "BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;\">\n";

  /* Definitions & styles. */
  svg << "<defs>\n"
      << "  <filter id=\"shadow\" x=\"-10%\" y=\"-10%\" width=\"120%\" height=\"120%\">\n"
      << "    <feDropShadow dx=\"1\" dy=\"1\" stdDeviation=\"1.5\" flood-opacity=\"0.2\"/>\n"
      << "  </filter>\n"
      << "</defs>\n";

  /* Title. */
  svg << "<text x=\"30\" y=\"40\" font-size=\"20\" font-weight=\"bold\" fill=\"#2c3e50\">"
      << (test_name ? test_name : "Straight Skeleton") << "</text>\n";

  /* Pastel face color palette. */
  static const char *face_colors[] = {
      "#d4e6f1",
      "#d5f5e3",
      "#fdebd0",
      "#e8daef",
      "#fcf3cf",
      "#fadbd8",
      "#d0ece7",
      "#ebf5fb",
      "#eafaf1",
      "#fef9e7",
  };
  const int num_colors = int(sizeof(face_colors) / sizeof(face_colors[0]));
  const bool draw_labels = []() {
    if (const char *env = std::getenv("STRAIGHT_SKELETON_LABELS")) {
      return (std::string_view(env) == "1" || std::string_view(env) == "true");
    }
    return false;
  }();

  /* Draw faces. */
  const Span<int> fvi = output.face_vert_indices;
  for (const int fi : face_offsets.index_range()) {
    const IndexRange r = face_offsets[fi];
    const int n = int(r.size());
    if (n < 3) {
      continue;
    }
    const char *col = face_colors[fi % num_colors];
    svg << "<polygon points=\"";
    double cx = 0.0, cy = 0.0;
    for (const int idx : r) {
      const double2 p = output.vert[fvi[idx]];
      svg << tx(p.x) << "," << ty(p.y) << " ";
      cx += p.x;
      cy += p.y;
    }
    cx /= double(n);
    cy /= double(n);
    svg << "\" fill=\"" << col << "\" stroke=\"#7f8c8d\" stroke-width=\"1\" opacity=\"0.85\"/>\n";

    /* Face label at centroid if vert count is reasonable. */
    if (draw_labels) {
      svg << "<text x=\"" << tx(cx) << "\" y=\"" << ty(cy)
          << "\" font-size=\"14\" font-weight=\"bold\" fill=\"#34495e\" text-anchor=\"middle\" "
             "dominant-baseline=\"middle\" filter=\"url(#shadow)\">f"
          << fi << "</text>\n";
    }
  }

  /* Identify input boundary edges to distinguish from straight skeleton internal edges. */
  Set<std::pair<int, int>> input_boundary_edges;
  const int n_outer = int(input.outer_polygon.size());
  for (int k = 0; k < n_outer; k++) {
    const int u = input.outer_polygon[k];
    const int v = input.outer_polygon[(k + 1) % n_outer];
    input_boundary_edges.add({std::min(u, v), std::max(u, v)});
  }
  for (const int h : IndexRange(input.hole_offsets.size())) {
    const IndexRange r = input.hole_offsets[h];
    const int n_h = int(r.size());
    for (int k = 0; k < n_h; k++) {
      const int u = input.hole_vert_indices[r[k]];
      const int v = input.hole_vert_indices[r[(k + 1) % n_h]];
      input_boundary_edges.add({std::min(u, v), std::max(u, v)});
    }
  }
  const int n_chain = int(input.edge_chain.size());
  for (int k = 0; k < n_chain - 1; k++) {
    const int u = input.edge_chain[k];
    const int v = input.edge_chain[k + 1];
    input_boundary_edges.add({std::min(u, v), std::max(u, v)});
  }

  /* Draw straight skeleton edges (non-boundary internal arcs in crimson red). */
  for (const int2 &e : output.edge) {
    const std::pair<int, int> pair_e = {std::min(e[0], e[1]), std::max(e[0], e[1])};
    if (input_boundary_edges.contains(pair_e)) {
      continue;
    }
    const double2 p1 = output.vert[e[0]];
    const double2 p2 = output.vert[e[1]];
    svg << "<line x1=\"" << tx(p1.x) << "\" y1=\"" << ty(p1.y) << "\" x2=\"" << tx(p2.x)
        << "\" y2=\"" << ty(p2.y) << "\" stroke=\"#e74c3c\" stroke-width=\""
        << (draw_labels ? "2.5" : "1.5") << "\" stroke-linecap=\"round\"/>\n";
  }

  /* Draw input boundary edges in bold dark blue. */
  for (const auto &e : input_boundary_edges) {
    if (e.first < output.vert.size() && e.second < output.vert.size()) {
      const double2 p1 = output.vert[e.first];
      const double2 p2 = output.vert[e.second];
      svg << "<line x1=\"" << tx(p1.x) << "\" y1=\"" << ty(p1.y) << "\" x2=\"" << tx(p2.x)
          << "\" y2=\"" << ty(p2.y) << "\" stroke=\"#2980b9\" stroke-width=\""
          << (draw_labels ? "3.5" : "2.0") << "\" stroke-linecap=\"round\"/>\n";
    }
  }

  /* Draw vertices and vertex labels if reasonable count. */
  if (draw_labels) {
    for (const int vi : output.vert.index_range()) {
      const double2 p = output.vert[vi];
      const bool is_input_vert = (vi < input.vert.size());
      const char *circle_color = is_input_vert ? "#2980b9" : "#e74c3c";
      const double r = is_input_vert ? 5.0 : 4.0;

      svg << "<circle cx=\"" << tx(p.x) << "\" cy=\"" << ty(p.y) << "\" r=\"" << r << "\" fill=\""
          << circle_color
          << "\" stroke=\"#ffffff\" stroke-width=\"1.5\" filter=\"url(#shadow)\"/>\n";

      /* Offset label away from vertex. */
      svg << "<text x=\"" << (tx(p.x) + 7.0) << "\" y=\"" << (ty(p.y) - 6.0)
          << "\" font-size=\"11\" font-weight=\"bold\" fill=\""
          << (is_input_vert ? "#1a5276" : "#922b21") << "\">v" << vi << "</text>\n";
    }
  }

  /* Legend in top-right corner. */
  const double leg_x = width - 200.0;
  svg << "<g transform=\"translate(" << leg_x << ", 15)\">\n"
      << "  <rect width=\"180\" height=\"75\" fill=\"#f8f9fa\" stroke=\"#dee2e6\" rx=\"6\"/>\n"
      << "  <line x1=\"15\" y1=\"20\" x2=\"45\" y2=\"20\" stroke=\"#2980b9\" "
         "stroke-width=\"3.5\"/>\n"
      << "  <text x=\"55\" y=\"24\" font-size=\"12\" fill=\"#2c3e50\">Input Boundary</text>\n"
      << "  <line x1=\"15\" y1=\"40\" x2=\"45\" y2=\"40\" stroke=\"#e74c3c\" "
         "stroke-width=\"2.5\"/>\n"
      << "  <text x=\"55\" y=\"44\" font-size=\"12\" fill=\"#2c3e50\">Skeleton Edge</text>\n"
      << "  <rect x=\"15\" y=\"52\" width=\"30\" height=\"12\" fill=\"#d4e6f1\" "
         "stroke=\"#7f8c8d\" "
         "stroke-width=\"1\"/>\n"
      << "  <text x=\"55\" y=\"63\" font-size=\"12\" fill=\"#2c3e50\">Output Face</text>\n"
      << "</g>\n";

  svg << "</svg>\n";

  /* Also write standalone SVG file to /tmp/<test_name>.svg if test_name is non-empty. */
  if (test_name && test_name[0] != '\0') {
    std::string safe_name;
    for (const char *c = test_name; *c != '\0'; c++) {
      if (*c == ' ' || *c == '/' || *c == '=' || *c == '(' || *c == ')') {
        safe_name += '_';
      }
      else {
        safe_name += *c;
      }
    }
    std::string svg_path = std::string("/tmp/") + safe_name + ".svg";
    std::ofstream sf(svg_path);
    if (sf.is_open()) {
      sf << svg.str();
      sf.close();
      std::cout << "Standalone SVG written to: " << svg_path << "\n";
    }
  }

  /* Append (or create) the HTML debug file. */
  const char *target_html = std::getenv("STRAIGHT_SKELETON_HTML_FILE");
  if (!target_html || target_html[0] == '\0') {
    target_html = html_filepath;
  }

  bool &debug_new_file = get_debug_new_file();
  if (const char *app_env = std::getenv("STRAIGHT_SKELETON_APPEND_HTML")) {
    if (std::string_view(app_env) == "1" || std::string_view(app_env) == "true") {
      debug_new_file = false;
    }
  }
  const std::ios::openmode mode = debug_new_file ? (std::ios::out | std::ios::trunc) :
                                                   (std::ios::out | std::ios::app);

  std::ofstream f(target_html, mode);
  if (!f.is_open()) {
    std::cout << "Could not open debug HTML file: " << target_html << "\n";
    return;
  }

  if (debug_new_file) {
    /* Write the HTML document header on first open. */
    f << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
      << "<meta charset=\"utf-8\">\n"
      << "<title>Straight Skeleton Drawings</title>\n"
      << "<style>\n"
      << "  body { font-family: sans-serif; background: #1a1a2e; color: #e0e0e0;"
         " margin: 0; padding: 20px; }\n"
      << "  h1 { color: #a0c4ff; border-bottom: 2px solid #a0c4ff; padding-bottom: 8px; }\n"
      << "  .card { display: inline-block; background: #16213e; border: 1px solid #3a3a5c;"
         " border-radius: 8px; padding: 12px; margin: 12px; vertical-align: top; }\n"
      << "  .card svg { display: block; background: #fff; border-radius: 4px; }\n"
      << "</style>\n"
      << "</head>\n<body>\n"
      << "<h1>Straight Skeleton Drawings</h1>\n";
    debug_new_file = false;
  }

  /* Wrap the SVG in a labelled card div. */
  f << "<div class=\"card\">\n" << svg.str() << "</div>\n";
  f.close();

  std::cout << "SVG appended to: " << target_html << "\n";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Input helpers
 * \{ */

static StraightSkeletonInput make_test_input()
{
  StraightSkeletonInput input;
  input.full_verification = true;
  return input;
}

/**
 * Owns the backing storage for a #StraightSkeletonInput.
 *
 * The spec string has the form:
 *   #verts #faces
 *   <double> <double>          [#verts lines -- 2D x y]
 *   <int> <int> ... <int>      [#faces lines: line 0 is outer polygon (CCW), lines 1..#faces-1 are
 * holes (CW)]
 */
struct InputStorage {
  Array<double2> vert;
  Vector<int> outer_polygon;
  Vector<int> hole_offsets_buf;
  Vector<int> hole_vert_indices;
};

static StraightSkeletonInput fill_input_from_string(const char *spec, InputStorage &r_storage)
{
  std::istringstream ss(spec);
  std::string line;
  getline(ss, line);
  std::istringstream hdrss(line);
  int verts_num, faces_num;
  hdrss >> verts_num >> faces_num;

  r_storage.vert.reinitialize(verts_num);
  r_storage.outer_polygon.clear();
  r_storage.hole_offsets_buf.clear();
  r_storage.hole_vert_indices.clear();

  int i = 0;
  while (i < verts_num && getline(ss, line)) {
    std::istringstream iss(line);
    double x, y;
    iss >> x >> y;
    r_storage.vert[i] = double2(x, y);
    i++;
  }

  i = 0;
  while (i < faces_num && getline(ss, line)) {
    std::istringstream fss(line);
    int v;
    if (i == 0) {
      while (fss >> v) {
        r_storage.outer_polygon.append(v);
      }
    }
    else {
      const int hole_idx = i - 1;
      if (r_storage.hole_offsets_buf.is_empty()) {
        r_storage.hole_offsets_buf.resize(faces_num); /* faces_num - 1 holes + 1 */
        r_storage.hole_offsets_buf[0] = 0;
      }
      while (fss >> v) {
        r_storage.hole_vert_indices.append(v);
      }
      r_storage.hole_offsets_buf[hole_idx + 1] = int(r_storage.hole_vert_indices.size());
    }
    i++;
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = r_storage.vert.as_span();
  input.outer_polygon = r_storage.outer_polygon.as_span();
  if (!r_storage.hole_offsets_buf.is_empty()) {
    input.hole_offsets = OffsetIndices<int>(r_storage.hole_offsets_buf.as_span());
    input.hole_vert_indices = r_storage.hole_vert_indices.as_span();
  }
  return input;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug Output Controls
 * \{ */

static DebugOutputMode get_debug_output_mode()
{
  const char *env = std::getenv("STRAIGHT_SKELETON_DEBUG");
  if (env != nullptr) {
    const std::string s(env);
    if (s == "steps" || s == "1") {
      return DebugOutputMode::STEPS;
    }
    if (s == "output" || s == "2") {
      return DebugOutputMode::OUTPUT;
    }
    if (s == "none" || s == "0") {
      return DebugOutputMode::NONE;
    }
  }
  return debug_output_mode;
}

static double total_faces_area(const StraightSkeletonOutput &output)
{
  double total = 0.0;
  const Span<int> fvi = output.face_vert_indices;
  for (const int fi : output.face_offsets.index_range()) {
    const IndexRange r = output.face_offsets[fi];
    const int n = int(r.size());
    double area = 0.0;
    for (const int i : IndexRange(n)) {
      const double2 p1 = output.vert[fvi[r[i]]];
      const double2 p2 = output.vert[fvi[r[(i + 1) % n]]];
      area += math::cross(p1, p2);
    }
    total += 0.5 * area;
  }
  return total;
}

static void print_test_output(const char *test_name,
                              const StraightSkeletonInput &input,
                              const StraightSkeletonOutput &output)
{
  EXPECT_FALSE(output.hit_max_events)
      << "Straight skeleton hit max_events in test: " << (test_name ? test_name : "unknown");

  if (input.full_verification) {
    EXPECT_TRUE(output.is_valid) << "verify_straight_skeleton failed for test: "
                                 << (test_name ? test_name : "unknown") << ": "
                                 << output.error_message;
  }

  if (is_svg_drawing_enabled() || get_debug_output_mode() == DebugOutputMode::OUTPUT) {
    write_debug_svg(test_name, input, output);
  }

  if (get_debug_output_mode() != DebugOutputMode::OUTPUT) {
    return;
  }

  std::cout << "\n============================================================\n";
  std::cout << "Straight Skeleton Output: " << (test_name ? test_name : "Test") << "\n";
  std::cout << "============================================================\n";

  std::cout << "Vertices (" << output.vert.size() << "):\n";
  for (const int vi : output.vert.index_range()) {
    std::cout << "  v" << vi << ": pos=(" << std::fixed << std::setprecision(4)
              << output.vert[vi].x << ", " << output.vert[vi].y << ")";
    if (vi < output.vert_orig.size()) {
      std::cout << " | vert_orig=" << output.vert_orig[vi];
    }
    std::cout << "\n";
  }

  std::cout << "\nEdges (" << output.edge.size() << "):\n";
  for (const int ei : output.edge.index_range()) {
    std::cout << "  e" << ei << ": (" << output.edge[ei][0] << " -> " << output.edge[ei][1] << ")";
    if (ei < output.edge_orig.size()) {
      std::cout << " | edge_orig=" << output.edge_orig[ei];
    }
    std::cout << "\n";
  }

  std::cout << "\nFaces (" << output.face_offsets.size() << "):\n";
  const Span<int> fvi = output.face_vert_indices;
  double total_area = 0.0;
  for (const int fi : output.face_offsets.index_range()) {
    const IndexRange r = output.face_offsets[fi];
    const int n = int(r.size());
    double area = 0.0;
    for (const int i : IndexRange(n)) {
      const double2 p1 = output.vert[fvi[r[i]]];
      const double2 p2 = output.vert[fvi[r[(i + 1) % n]]];
      area += math::cross(p1, p2);
    }
    area *= 0.5;
    total_area += area;

    std::cout << "  f" << fi << ": verts=[";
    for (int i = 0; i < n; i++) {
      std::cout << (i > 0 ? ", " : "") << fvi[r[i]];
    }
    std::cout << "] | area=" << std::fixed << std::setprecision(4) << area;
    if (fi < output.face_orig.size()) {
      std::cout << " | face_orig=" << output.face_orig[fi];
    }
    std::cout << "\n";
  }
  std::cout << "Total Face Area: " << std::fixed << std::setprecision(6) << total_area << "\n";
  std::cout << "============================================================\n\n";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tests
 * \{ */

/**
 * Equilateral triangle.
 *
 * The straight skeleton of a triangle is the single point where all three
 * angle bisectors meet (the incenter).  The output should be three triangles,
 * each sharing the incenter as one vertex and one side of the input as their base.
 *
 * Input:
 *   v0 = (0, 0)   v1 = (1, 0)   v2 = (0.5, sqrt(3)/2 ~= 0.866)
 *
 * Expected:
 *   4 output vertices: the 3 originals plus the incenter at (0.5, 0.5/sqrt(3) ~= 0.2887).
 *   3 output faces, each a triangle.
 */
TEST(straight_skeleton, Triangle)
{
  const char *spec = R"(3 1
0.0 0.0
1.0 0.0
0.5 0.8660254037844387
0 1 2
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "Triangle";

  StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_FALSE(output.hit_max_events);

  /* Triangle skeleton: 3 original verts + 1 incenter = 4 verts, 3 output faces. */
  EXPECT_EQ(output.vert.size(), 4);
  EXPECT_EQ(output.face_offsets.size(), 3);

  /* Every output face should be a triangle (3 verts). */
  for (const int fi : output.face_offsets.index_range()) {
    EXPECT_EQ(output.face_offsets[fi].size(), 3);
  }

  /* vert_orig: the first 3 output verts should map back to input verts 0,1,2;
   * the incenter should map to -1. */
  EXPECT_EQ(output.vert_orig.size(), int64_t(output.vert.size()));
  int new_verts_count = 0;
  for (const int vi : IndexRange(output.vert.size())) {
    if (output.vert_orig[vi] == -1) {
      new_verts_count++;
      /* Incenter of an equilateral triangle with side 1 is at (0.5, 0.5/sqrt(3)). */
      EXPECT_NEAR(output.vert[vi].x, 0.5, 1e-6);
      EXPECT_NEAR(output.vert[vi].y, 0.5 / 1.7320508075688772, 1e-6);
    }
  }
  EXPECT_EQ(new_verts_count, 1);

  /* face_orig: every output face should derive from input face 0. */
  EXPECT_EQ(output.face_orig.size(), int64_t(output.face_offsets.size()));
  for (const int fi : IndexRange(output.face_orig.size())) {
    EXPECT_EQ(output.face_orig[fi], 0);
  }

  EXPECT_NEAR(total_faces_area(output), std::sqrt(3.0) / 4.0, 1e-5);
}

/**
 * Non-convex polygon from Palfrader, Held, Huber (2012) demonstrating both
 * edge events and split events in the straight skeleton algorithm.
 *
 * The polygon has 23 vertices with multiple concavities: a notch on the
 * upper-left, a notch at the top center, a notch on the right side, a
 * pointed protrusion at the lower-right, a V-notch at the bottom center, a
 * pointed protrusion at the lower-left, and a notch on the left side.
 */
TEST(straight_skeleton, PaperExample)
{
  const char *spec = R"(23 1
1.3 7.9
0.4 6.0
2.0 4.6
0.4 3.6
0.4 1.8
1.6 0.0
5.0 1.4
6.7 1.1
6.7 2.4
9.2 1.3
11.7 2.4
11.7 1.1
15.3 1.9
13.7 3.5
14.7 4.5
14.7 7.9
15.9 9.1
14.6 12.8
10.2 12.8
8.5 11.7
7.3 12.8
1.8 12.8
0.0 11.0
0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "PaperExample";
  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets.size(), 23);
  EXPECT_NEAR(total_faces_area(output), 159.835, 1e-3);
}

TEST(straight_skeleton, NotchedPolygonFull)
{
  const char *spec = R"(7 1
0.0 0.0
7.0 2.0
14.0 7.0
0.0 9.0
0.2 5.5
1.5 4.5
0.2 3.5
0 1 2 3 4 5 6
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "NotchedPolygonFull";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.vert.size(), 12);
  EXPECT_EQ(output.edge.size(), 18);
  EXPECT_EQ(output.face_offsets.size(), 7);
  EXPECT_NEAR(total_faces_area(output), 71.1, 1e-4);
  for (const int fi : output.face_offsets.index_range()) {
    EXPECT_EQ(output.face_orig[fi], 0);
  }
}

TEST(straight_skeleton, Square)
{
  const char *spec = R"(4 1
0.0 0.0
2.0 0.0
2.0 2.0
0.0 2.0
0 1 2 3
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "Square";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.vert.size(), 5);
  EXPECT_EQ(output.edge.size(), 8);
  EXPECT_EQ(output.face_offsets.size(), 4);
  EXPECT_NEAR(total_faces_area(output), 4.0, 1e-6);
  for (const int fi : output.face_offsets.index_range()) {
    EXPECT_EQ(output.face_offsets[fi].size(), 3);
    EXPECT_EQ(output.face_orig[fi], 0);
  }
}

TEST(straight_skeleton, LShape)
{
  const char *spec = R"(6 1
0.0 0.0
4.0 0.0
4.0 2.0
2.0 2.0
2.0 4.0
0.0 4.0
0 1 2 3 4 5
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "LShape";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.vert.size(), 9);
  EXPECT_EQ(output.edge.size(), 14);
  EXPECT_EQ(output.face_offsets.size(), 6);
  EXPECT_NEAR(total_faces_area(output), 12.0, 1e-6);
  for (const int fi : output.face_offsets.index_range()) {
    EXPECT_EQ(output.face_orig[fi], 0);
  }
}

TEST(straight_skeleton, RectangleEdgeCollapse)
{
  const char *spec = R"(4 1
0.0 0.0
4.0 0.0
4.0 2.0
0.0 2.0
0 1 2 3
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "RectangleEdgeCollapse";

  /* Full straight skeleton output: 4 original corners + 2 ridge vertices = 6 vertices. */
  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.vert.size(), 6);
  EXPECT_EQ(output.edge.size(), 9);
  int new_verts_count = 0;
  for (const int vi : IndexRange(output.vert.size())) {
    if (output.vert_orig[vi] == -1) {
      new_verts_count++;
      /* Ridge nodes should be at y = 1.0, x = 1.0 or x = 3.0. */
      EXPECT_NEAR(output.vert[vi].y, 1.0, 1e-5);
      EXPECT_TRUE(std::abs(output.vert[vi].x - 1.0) < 1e-5 ||
                  std::abs(output.vert[vi].x - 3.0) < 1e-5);
    }
  }
  EXPECT_EQ(new_verts_count, 2);

  EXPECT_EQ(output.face_offsets.size(), 4);
  EXPECT_NEAR(total_faces_area(output), 8.0, 1e-6);
  for (const int fi : output.face_offsets.index_range()) {
    EXPECT_EQ(output.face_orig[fi], 0);
  }
}

/**
 * Elongated rectangle (8 x 2).
 *
 * Tests the infinite speed vertex propagation over an extended distance of 6 units
 * along the ridge y = 1.0 between x = 1.0 and x = 7.0 at t = 1.0.
 */
TEST(straight_skeleton, ElongatedRectangleInfiniteSpeed)
{
  const char *spec = R"(4 1
0.0 0.0
8.0 0.0
8.0 2.0
0.0 2.0
0 1 2 3
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "ElongatedRectangleInfiniteSpeed";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.vert.size(), 6);
  EXPECT_EQ(output.edge.size(), 9);

  int new_verts_count = 0;
  for (const int vi : IndexRange(output.vert.size())) {
    if (output.vert_orig[vi] == -1) {
      new_verts_count++;
      EXPECT_NEAR(output.vert[vi].y, 1.0, 1e-5);
      EXPECT_TRUE(std::abs(output.vert[vi].x - 1.0) < 1e-5 ||
                  std::abs(output.vert[vi].x - 7.0) < 1e-5);
    }
  }
  EXPECT_EQ(new_verts_count, 2);

  EXPECT_EQ(output.face_offsets.size(), 4);
  EXPECT_NEAR(total_faces_area(output), 16.0, 1e-6);
  for (const int fi : output.face_offsets.index_range()) {
    EXPECT_EQ(output.face_orig[fi], 0);
  }
}

TEST(straight_skeleton, MultiSplitHourglass)
{
  const char *spec = R"(6 1
0.0 0.0
4.0 0.0
3.0 1.0
4.0 2.0
0.0 2.0
1.0 1.0
0 1 2 3 4 5
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "MultiSplitHourglass";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.vert.size(), 9);
  EXPECT_EQ(output.edge.size(), 14);
  EXPECT_EQ(output.face_offsets.size(), 6);
  EXPECT_NEAR(total_faces_area(output), 6.0, 1e-6);
  for (const int fi : output.face_offsets.index_range()) {
    EXPECT_EQ(output.face_orig[fi], 0);
  }
}

/**
 * "Flipper" test adapted from the straightskel_2022 branch.
 *
 * A polygon with curved reflex corner and 4 notches along the top edge of varying depths.
 * Tests multiple flip events as reflex vertices move across internal triangulation spokes.
 */
TEST(straight_skeleton, Flipper)
{
  const char *spec = R"(20 1
0.0 0.0
1.5 0.0
1.375 0.025
1.25 0.06
1.125 0.11
1.0 0.2
1.0 1.0
0.79 1.0
0.75 0.95
0.71 1.0
0.585 1.0
0.55 0.9
0.515 1.0
0.38 1.0
0.35 0.85
0.32 1.0
0.175 1.0
0.15 0.8
0.125 1.0
0.0 1.0
0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "Flipper";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.vert.size(), 38);
  EXPECT_EQ(output.edge.size(), 57);
  for (const double2 &v : output.vert) {
    EXPECT_GE(v.y, -1e-6);
  }
  EXPECT_EQ(output.face_offsets.size(), 20);
  EXPECT_NEAR(total_faces_area(output), 1.021875, 1e-5);
  for (const int fi : output.face_offsets.index_range()) {
    EXPECT_EQ(output.face_orig[fi], 0);
  }
}

/* -------------------------------------------------------------------- */
/** \name Multi-segment tests
 * \{ */

/**
 * Full straight skeleton of the L-shaped polygon subdivided into 5 equal-width bands.
 * offset = infinity so effective_offset = max_collapse_time.
 * Verifies that all 5 bands are produced, all face_band values are in [0, 4],
 * and the total face area equals the polygon area (12.0).
 */
TEST(straight_skeleton, LShape_FiveSegments)
{
  const char *spec = R"(6 1
0.0 0.0
4.0 0.0
4.0 2.0
2.0 2.0
2.0 4.0
0.0 4.0
0 1 2 3 4 5
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "LShape_FiveSegments";
  input.segments = 5;
  /* input.offset defaults to infinity: effective_offset = max_collapse_time. */

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* All face_band values must be in [0, 4]. */
  bool bands_seen[5] = {false, false, false, false, false};
  for (const int fi : output.face_offsets.index_range()) {
    const int b = output.face_band[fi];
    EXPECT_GE(b, 0);
    EXPECT_LT(b, 5);
    if (b >= 0 && b < 5) {
      bands_seen[b] = true;
    }
  }
  for (const int b : IndexRange(5)) {
    EXPECT_TRUE(bands_seen[b]) << "Band " << b << " has no faces.";
  }

  /* Total area must equal the L-shape area. */
  EXPECT_NEAR(total_faces_area(output), 12.0, 1e-5);
}

/**
 * Full straight skeleton of the notched polygon subdivided into 5 equal-width bands.
 * offset = infinity so effective_offset = max_collapse_time.
 * Verifies that all 5 bands are produced and the total area equals the polygon area (71.1).
 */
TEST(straight_skeleton, NotchedPolygonFull_FiveSegments)
{
  const char *spec = R"(7 1
0.0 0.0
7.0 2.0
14.0 7.0
0.0 9.0
0.2 5.5
1.5 4.5
0.2 3.5
0 1 2 3 4 5 6
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "NotchedPolygonFull_FiveSegments";
  input.segments = 5;
  /* input.offset defaults to infinity: effective_offset = max_collapse_time. */

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* All face_band values must be in [0, 4]. */
  bool bands_seen[5] = {false, false, false, false, false};
  for (const int fi : output.face_offsets.index_range()) {
    const int b = output.face_band[fi];
    EXPECT_GE(b, 0);
    EXPECT_LT(b, 5);
    if (b >= 0 && b < 5) {
      bands_seen[b] = true;
    }
  }
  for (const int b : IndexRange(5)) {
    EXPECT_TRUE(bands_seen[b]) << "Band " << b << " has no faces.";
  }

  /* Total area must equal the notched polygon area. */
  EXPECT_NEAR(total_faces_area(output), 71.1, 1e-3);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Offset tests
 * \{ */

/**
 * Partial offset of a 10x10 square at distance 1.
 * Produces 4 side-strip trapezoids and 1 inner square (5 faces total).
 */
TEST(straight_skeleton, OffsetSquarePartial)
{
  Array<double2> verts = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.outer_polygon.extend(face_verts.as_span());

  StraightSkeletonInput input = make_test_input();
  input.vert = storage.vert.as_span();
  input.outer_polygon = storage.outer_polygon.as_span();
  input.offset = 1.0;
  input.segments = 1;
  input.label = "OffsetSquarePartial";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* 4 side-strip trapezoids + 1 inner square. */
  EXPECT_EQ(output.face_offsets.size(), 5);

  /* Output contains only 4 input corners + 4 cut vertices (the apex at t = 5 is omitted). */
  EXPECT_EQ(output.vert.size(), 8);
  for (const int vi : output.vert.index_range()) {
    EXPECT_LE(output.vert_time[vi], 1.0 + 1e-6);
  }

  bool found_cut_vert = false;
  for (const int vi : output.vert.index_range()) {
    if (output.vert_orig[vi] == -1 && std::abs(output.vert_time[vi] - 1.0) < 1e-6) {
      found_cut_vert = true;
    }
  }
  EXPECT_TRUE(found_cut_vert);

  /* Total area must equal the square area. */
  EXPECT_NEAR(total_faces_area(output), 100.0, 1e-6);

  /* All faces are in band 0. */
  for (const int fi : output.face_offsets.index_range()) {
    EXPECT_EQ(output.face_band[fi], 0);
  }
}

/**
 * Offset of a 10x10 square exactly to its collapse point (t = 5).
 * The inner polygon degenerates to the single apex vertex.
 * Produces 4 triangular side-strip faces (each vertex is the apex).
 */
TEST(straight_skeleton, OffsetSquareCollapse)
{
  Array<double2> verts = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.outer_polygon.extend(face_verts.as_span());

  StraightSkeletonInput input = make_test_input();
  input.vert = storage.vert.as_span();
  input.outer_polygon = storage.outer_polygon.as_span();
  input.offset = 5.0;
  input.segments = 1;
  input.label = "OffsetSquareCollapse";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* 4 side-strip triangles (apex at center). */
  EXPECT_EQ(output.face_offsets.size(), 4);

  /* The apex vertex is at (5, 5) with vert_time == 5. */
  bool found_apex = false;
  for (const int vi : output.vert.index_range()) {
    if (output.vert_orig[vi] == -1) {
      EXPECT_NEAR(output.vert[vi].x, 5.0, 1e-6);
      EXPECT_NEAR(output.vert[vi].y, 5.0, 1e-6);
      EXPECT_NEAR(output.vert_time[vi], 5.0, 1e-6);
      found_apex = true;
    }
  }
  EXPECT_TRUE(found_apex);

  EXPECT_NEAR(total_faces_area(output), 100.0, 1e-6);
}

/**
 * Offset of a 10x10 square with an excess offset (10 > max_collapse_time = 5).
 * Should be silently clamped to t = 5, giving the same result as OffsetSquareCollapse.
 */
TEST(straight_skeleton, OffsetSquareExcess)
{
  Array<double2> verts = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.outer_polygon.extend(face_verts.as_span());

  StraightSkeletonInput input = make_test_input();
  input.vert = storage.vert.as_span();
  input.outer_polygon = storage.outer_polygon.as_span();
  input.offset = 10.0; /* Exceeds max_collapse_time = 5, clamped. */
  input.segments = 1;
  input.label = "OffsetSquareExcess";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Same as OffsetSquareCollapse: 4 triangular strips to the apex. */
  EXPECT_EQ(output.face_offsets.size(), 4);

  bool found_apex = false;
  for (const int vi : output.vert.index_range()) {
    if (output.vert_orig[vi] == -1) {
      EXPECT_NEAR(output.vert[vi].x, 5.0, 1e-6);
      EXPECT_NEAR(output.vert[vi].y, 5.0, 1e-6);
      EXPECT_NEAR(output.vert_time[vi], 5.0, 1e-6);
      found_apex = true;
    }
  }
  EXPECT_TRUE(found_apex);

  EXPECT_NEAR(total_faces_area(output), 100.0, 1e-6);
}

/**
 * Offset of a 20x10 rectangle at distance 2 (less than min half-width 5).
 * Produces 4 side-strip trapezoids + 1 inner rectangle (5 faces total).
 * All new vertices are at vert_time == 2.
 */
TEST(straight_skeleton, OffsetRectangle)
{
  Array<double2> verts = {{0.0, 0.0}, {20.0, 0.0}, {20.0, 10.0}, {0.0, 10.0}};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.outer_polygon.extend(face_verts.as_span());

  StraightSkeletonInput input = make_test_input();
  input.vert = storage.vert.as_span();
  input.outer_polygon = storage.outer_polygon.as_span();
  input.offset = 2.0;
  input.segments = 1;
  input.label = "OffsetRectangle";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_EQ(output.face_offsets.size(), 5);

  /* Output contains only 4 input corners + 4 cut vertices (ridge nodes at t = 5 are omitted). */
  EXPECT_EQ(output.vert.size(), 8);
  for (const int vi : output.vert.index_range()) {
    EXPECT_LE(output.vert_time[vi], 2.0 + 1e-6);
  }

  bool found_cut_vert = false;
  for (const int vi : output.vert.index_range()) {
    if (output.vert_orig[vi] == -1 && std::abs(output.vert_time[vi] - 2.0) < 1e-6) {
      found_cut_vert = true;
    }
  }
  EXPECT_TRUE(found_cut_vert);

  EXPECT_NEAR(total_faces_area(output), 200.0, 1e-6);
}

/**
 * Offset of an L-shaped polygon at distance 0.5.
 * Produces 6 side-strip faces + 1 inner L-shape polygon (7 faces total).
 */
TEST(straight_skeleton, OffsetLShape)
{
  Array<double2> verts = {{0.0, 0.0}, {4.0, 0.0}, {4.0, 2.0}, {2.0, 2.0}, {2.0, 4.0}, {0.0, 4.0}};
  Array<int> face_verts = {0, 1, 2, 3, 4, 5};

  InputStorage storage;
  storage.vert = verts;
  storage.outer_polygon.extend(face_verts.as_span());

  StraightSkeletonInput input = make_test_input();
  input.vert = storage.vert.as_span();
  input.outer_polygon = storage.outer_polygon.as_span();
  input.offset = 0.5;
  input.segments = 1;
  input.label = "OffsetLShape";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_EQ(output.face_offsets.size(), 7); /* 6 side-strips + 1 inner L-shape. */

  /* Output contains only 6 input corners + 6 cut vertices (skeleton nodes at t = 1 are omitted).
   */
  EXPECT_EQ(output.vert.size(), 12);
  for (const int vi : output.vert.index_range()) {
    EXPECT_LE(output.vert_time[vi], 0.5 + 1e-6);
  }

  EXPECT_NEAR(total_faces_area(output), 12.0, 1e-6);
}

/**
 * Two-segment offset of a 10x10 square with offset = 2.
 * effective_offset = 2 (< max_collapse = 5).
 * Band 0: t in [0, 1]  -- 4 side-strip trapezoids + 1 inner square.
 * Band 1: t in [1, 2]  -- 4 side-strip trapezoids + 1 inner square.
 * Total: 10 faces.  Total area = 100 (the whole square).
 */
TEST(straight_skeleton, OffsetSquare_TwoBands)
{
  Array<double2> verts = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.outer_polygon.extend(face_verts.as_span());

  StraightSkeletonInput input = make_test_input();
  input.vert = storage.vert.as_span();
  input.outer_polygon = storage.outer_polygon.as_span();
  input.offset = 2.0;
  input.segments = 2;
  input.label = "OffsetSquare_TwoBands";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Band 0 (t=0..1): 4 side-strip trapezoids.
   * Band 1 (t=1..2): 4 side-strip trapezoids + 1 inner square.
   * Total = 9 faces. */
  EXPECT_EQ(output.face_offsets.size(), 9);

  /* Output contains only 4 input corners + 4 cut vertices (t=1) + 4 cut vertices (t=2).
   * Apex at t = 5 is omitted. */
  EXPECT_EQ(output.vert.size(), 12);
  for (const int vi : output.vert.index_range()) {
    EXPECT_LE(output.vert_time[vi], 2.0 + 1e-6);
  }

  /* Check band assignments. */
  int band0_count = 0;
  int band1_count = 0;
  for (const int fi : output.face_offsets.index_range()) {
    const int b = output.face_band[fi];
    EXPECT_TRUE(b == 0 || b == 1);
    if (b == 0) {
      band0_count++;
    }
    else {
      band1_count++;
    }
  }
  EXPECT_EQ(band0_count, 4);
  EXPECT_EQ(band1_count, 5);

  /* Band-boundary vertices at t = 1 (midpoint). */
  bool found_mid = false;
  for (const int vi : output.vert.index_range()) {
    if (output.vert_orig[vi] == -1 && std::abs(output.vert_time[vi] - 1.0) < 1e-6) {
      found_mid = true;
      break;
    }
  }
  EXPECT_TRUE(found_mid);

  /* Total area should cover the whole square. */
  EXPECT_NEAR(total_faces_area(output), 100.0, 1e-6);
}

/**
 * Full-skeleton with segments = 2 on a 10x10 square.
 * offset = infinity, segments = 2.
 * effective_offset = max_collapse_time = 5.
 * Band 0: t in [0, 2.5]  -- 4 side-strip faces + 1 inner square at 2.5.
 * Band 1: t in [2.5, 5]  -- 4 narrower side-strip faces + apex.
 * Total: 10 faces.  Total area = 100.
 */
TEST(straight_skeleton, FullSkeletonWithSegments)
{
  Array<double2> verts = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.outer_polygon.extend(face_verts.as_span());

  StraightSkeletonInput input = make_test_input();
  input.vert = storage.vert.as_span();
  input.outer_polygon = storage.outer_polygon.as_span();
  /* Default input.offset = infinity; segments = 2 divides the full skeleton into 2 bands. */
  input.segments = 2;
  input.label = "FullSkeletonWithSegments";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Band 0 (t=0..2.5): 4 side-strip trapezoids.
   * Band 1 (t=2.5..5): 4 side-strip triangles converging to the apex.
   *   The apex inner polygon degenerates to a point (area ~= 0) and is silently omitted.
   * Total = 8 faces. */
  EXPECT_EQ(output.face_offsets.size(), 8);

  /* Band boundary vertices at t = 2.5 must exist. */
  bool found_mid = false;
  for (const int vi : output.vert.index_range()) {
    if (output.vert_orig[vi] == -1 && std::abs(output.vert_time[vi] - 2.5) < 1e-6) {
      found_mid = true;
      break;
    }
  }
  EXPECT_TRUE(found_mid);

  /* The skeleton apex at t = 5 is part of the vertex pool (from the full skeleton graph). */
  bool found_apex = false;
  for (const int vi : output.vert.index_range()) {
    if (output.vert_orig[vi] == -1 && std::abs(output.vert_time[vi] - 5.0) < 1e-6) {
      found_apex = true;
    }
  }
  EXPECT_TRUE(found_apex);

  EXPECT_NEAR(total_faces_area(output), 100.0, 1e-6);
}

TEST(straight_skeleton, OneFaceBevel)
{
  const char *spec = R"(19 1
0.0000 0.0000
-0.0676 -0.0732
-0.2208 -0.2057
-0.3188 -0.2705
-0.3188 -0.2719
-0.0994 -0.1863
-0.0994 -1.7736
-0.0994 -1.8523
-0.1339 -1.9393
-0.2236 -1.9834
-0.3078 -1.9986
-0.3078 -2.0000
0.3602 -2.0000
0.3602 -1.9986
0.2719 -1.9821
0.1767 -1.9379
0.1408 -1.8523
0.1408 -1.7736
0.1518 0.0000
0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "OneFaceBevel";
  input.offset = 0.2;
  input.segments = 1;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* 19 input boundary edges produce 19 bevel faces. */
  EXPECT_EQ(output.face_offsets.size(), 19);
  for (const int fi : output.face_offsets.index_range()) {
    const IndexRange face_r = output.face_offsets[fi];
    Set<int> unique_verts;
    for (const int i : face_r) {
      EXPECT_TRUE(unique_verts.add(output.face_vert_indices[i]));
    }
  }
  EXPECT_NEAR(total_faces_area(output), 0.509493, 1e-4);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tests with Holes
 * \{ */

/**
 * 10x10 outer square (CCW) with a 4x4 inner square hole (CW) centered inside it.
 *
 * Outer: [0, 10] x [0, 10], corners (0,0), (10,0), (10,10), (0,10).
 * Hole:  [3, 7] x [3, 7], corners (3,7), (7,7), (7,3), (3,3) in CW order.
 *
 * The distance between outer and hole edges is 3 on all sides.
 * Wavefronts advance inward from the outer square and outward from the hole,
 * meeting at t = 1.5 along 4 ridges connecting collision points at:
 * (1.5, 1.5), (8.5, 1.5), (8.5, 8.5), (1.5, 8.5).
 *
 * Produces 8 trapezoidal faces:
 * - 4 outer faces (face_orig = 0)
 * - 4 hole faces (face_orig = 1)
 * Total face area = 100 - 16 = 84.
 */
TEST(straight_skeleton, SquareWithHole)
{
  const char *spec = R"(8 2
0.0 0.0
10.0 0.0
10.0 10.0
0.0 10.0
3.0 7.0
7.0 7.0
7.0 3.0
3.0 3.0
0 1 2 3
4 5 6 7
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "SquareWithHole";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* 8 input vertices + 4 skeleton ridge vertices = 12 vertices. */
  EXPECT_EQ(output.vert.size(), 12);
  /* 4 outer + 4 hole + 8 spoke + 4 ridge = 20 edges. */
  EXPECT_EQ(output.edge.size(), 20);
  /* 4 outer trapezoids + 4 hole trapezoids = 8 faces. */
  EXPECT_EQ(output.face_offsets.size(), 8);

  /* Total area = 10x10 outer - 4x4 hole = 84. */
  EXPECT_NEAR(total_faces_area(output), 84.0, 1e-5);

  /* Input boundary vertices (v0..v7) have vert_orig == vi and vert_time == 0. */
  for (const int vi : IndexRange(8)) {
    EXPECT_EQ(output.vert_orig[vi], vi);
    EXPECT_NEAR(output.vert_time[vi], 0.0, 1e-6);
  }

  /* 4 skeleton ridge vertices (v8..v11) are at collapse time t = 1.5 and vert_orig == -1. */
  int new_verts_count = 0;
  for (const int vi : IndexRange(8, 4)) {
    EXPECT_EQ(output.vert_orig[vi], -1);
    EXPECT_NEAR(output.vert_time[vi], 1.5, 1e-5);
    new_verts_count++;
    /* Each ridge vertex is either at x = 1.5 or x = 8.5, and y = 1.5 or y = 8.5. */
    const double2 p = output.vert[vi];
    EXPECT_TRUE(std::abs(p.x - 1.5) < 1e-5 || std::abs(p.x - 8.5) < 1e-5);
    EXPECT_TRUE(std::abs(p.y - 1.5) < 1e-5 || std::abs(p.y - 8.5) < 1e-5);
  }
  EXPECT_EQ(new_verts_count, 4);

  /* 4 outer faces have face_orig == 0 (area 12.75 each);
   * 4 hole faces have face_orig == 1 (area 8.25 each). */
  int outer_faces_count = 0;
  int hole_faces_count = 0;
  for (const int fi : output.face_offsets.index_range()) {
    const int orig = output.face_orig[fi];
    const IndexRange r = output.face_offsets[fi];
    EXPECT_EQ(r.size(), 4); /* All are trapezoids. */
    if (orig == 0) {
      outer_faces_count++;
    }
    else if (orig == 1) {
      hole_faces_count++;
    }
  }
  EXPECT_EQ(outer_faces_count, 4);
  EXPECT_EQ(hole_faces_count, 4);
}

/**
 * Fat cross outer polygon with 5 square holes inside it (1 in the center, 1 in each arm).
 *
 * Outer polygon: 12 vertices forming a plus / fat cross shape spanning [0, 30] x [0, 30].
 * Outer fat cross with 4 reflex vertices pushed outward to (22,8), (22,22), (8,22), (8,8)
 * plus 5 square holes (4x4 each): 1 in center, 4 in the arms.
 * Total area = 580 (outer area) - 5 * 16 (holes) = 500.
 */
TEST(straight_skeleton, FatCrossPushedOutWithHoles)
{
  const char *spec = R"(32 6
10.0 0.0
20.0 0.0
22.0 8.0
30.0 10.0
30.0 20.0
22.0 22.0
20.0 30.0
10.0 30.0
8.0 22.0
0.0 20.0
0.0 10.0
8.0 8.0
13.0 17.0
17.0 17.0
17.0 13.0
13.0 13.0
13.0 27.0
17.0 27.0
17.0 23.0
13.0 23.0
13.0 7.0
17.0 7.0
17.0 3.0
13.0 3.0
3.0 17.0
7.0 17.0
7.0 13.0
3.0 13.0
23.0 17.0
27.0 17.0
27.0 13.0
23.0 13.0
0 1 2 3 4 5 6 7 8 9 10 11
12 13 14 15
16 17 18 19
20 21 22 23
24 25 26 27
28 29 30 31
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "FatCrossPushedOutWithHoles";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Total area = 580 (outer area) - 5 * 16 (holes) = 500. */
  EXPECT_NEAR(total_faces_area(output), 500.0, 1e-4);

  /* Input boundary vertices (v0..v31) have vert_orig == vi and vert_time == 0. */
  for (const int vi : IndexRange(32)) {
    EXPECT_EQ(output.vert_orig[vi], vi);
    EXPECT_NEAR(output.vert_time[vi], 0.0, 1e-6);
  }

  /* Skeleton event vertices have vert_orig == -1 and vert_time > 0. */
  for (const int vi : IndexRange(32, output.vert.size() - 32)) {
    EXPECT_EQ(output.vert_orig[vi], -1);
    EXPECT_GT(output.vert_time[vi], 0.0);
  }

  /* Every face has at least 3 unique vertices. */
  for (const int fi : output.face_offsets.index_range()) {
    const IndexRange face_r = output.face_offsets[fi];
    EXPECT_GE(face_r.size(), 3);
    Set<int> unique_verts;
    for (const int i : face_r) {
      EXPECT_TRUE(unique_verts.add(output.face_vert_indices[i]));
    }
  }
}

/**
 * Fat cross outer polygon with 5 square holes inside it, where all outer cross
 * edges are strictly parallel to the x and y axes.
 *
 * Outer polygon: 12 vertices forming an axis-aligned fat cross spanning [0, 30] x [0, 30],
 * composed of five 10x10 squares. Outer area = 500.
 * 5 square holes (4x4 each): 1 in center, 4 in arms. Hole area = 5 * 16 = 80.
 * Expected total area = 500 - 80 = 420.
 */
TEST(straight_skeleton, FatCrossAxisAlignedWithHoles)
{
  const char *spec = R"(32 6
10.0 0.0
20.0 0.0
20.0 10.0
30.0 10.0
30.0 20.0
20.0 20.0
20.0 30.0
10.0 30.0
10.0 20.0
0.0 20.0
0.0 10.0
10.0 10.0
13.0 17.0
17.0 17.0
17.0 13.0
13.0 13.0
13.0 27.0
17.0 27.0
17.0 23.0
13.0 23.0
13.0 7.0
17.0 7.0
17.0 3.0
13.0 3.0
3.0 17.0
7.0 17.0
7.0 13.0
3.0 13.0
23.0 17.0
27.0 17.0
27.0 13.0
23.0 13.0
0 1 2 3 4 5 6 7 8 9 10 11
12 13 14 15
16 17 18 19
20 21 22 23
24 25 26 27
28 29 30 31
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "FatCrossAxisAlignedWithHoles";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Total area = 500 (outer area) - 5 * 16 (holes) = 420. */
  EXPECT_NEAR(total_faces_area(output), 420.0, 1e-4);

  /* Input boundary vertices (v0..v31) have vert_orig == vi and vert_time == 0. */
  for (const int vi : IndexRange(32)) {
    EXPECT_EQ(output.vert_orig[vi], vi);
    EXPECT_NEAR(output.vert_time[vi], 0.0, 1e-6);
  }

  /* Skeleton event vertices have vert_orig == -1 and vert_time > 0. */
  for (const int vi : IndexRange(32, output.vert.size() - 32)) {
    EXPECT_EQ(output.vert_orig[vi], -1);
    EXPECT_GT(output.vert_time[vi], 0.0);
  }

  /* Every face has at least 3 unique vertices. */
  for (const int fi : output.face_offsets.index_range()) {
    const IndexRange face_r = output.face_offsets[fi];
    EXPECT_GE(face_r.size(), 3);
    Set<int> unique_verts;
    for (const int i : face_r) {
      EXPECT_TRUE(unique_verts.add(output.face_vert_indices[i]));
    }
  }
}

/**
 * Opposing notches designed to trigger concurrent flip events (Section 3.2, Fig 3(b)).
 * Four reflex vertices (v2, v4, v9, v12) move towards the horizontal midline y = 0
 * and become collinear simultaneously at t* = 1 / sqrt(5) ~= 0.447214.
 *
 * Outer polygon: 15 vertices on a 10x6 box [-5, 5] x [-3, 3] with 4 reflex notches:
 * - 2 top notches pointing down: tips v12=(-2, 1) and v9=(2, 1) moving down at Vy = sqrt(5).
 * - 2 bottom notches pointing up: tips v2=(-1, -1) and v4=(1, -1) moving up at Vy = sqrt(5).
 * At t* = 1/sqrt(5) ~= 0.447214, all four tips reach y = 0 with x: -2 < -1 < 1 < 2.
 * Total area = 60 - 4 * 2 = 52.
 */
TEST(straight_skeleton, OpposingNotchesConcurrentFlips)
{
  const char *spec = R"(15 1
-5.0 -4.0
-2.0 -4.0
-1.0 -1.0
 0.0 -4.0
 1.0 -1.0
 2.0 -4.0
 5.0 -4.0
 5.0  4.0
 3.0  4.0
 2.0  1.0
 1.0  4.0
-1.0  4.0
-2.0  1.0
-3.0  4.0
-5.0  4.0
0 1 2 3 4 5 6 7 8 9 10 11 12 13 14
)";

  for (int shift = 0; shift < 15; shift++) {
    InputStorage storage;
    StraightSkeletonInput input = fill_input_from_string(spec, storage);
    Array<double2> orig_verts = storage.vert;
    for (int i = 0; i < 15; i++) {
      storage.vert[i] = orig_verts[(i + shift) % 15];
    }
    const StraightSkeletonOutput output = straight_skeleton_calc(input);
    if (shift == 0) {
      print_test_output("OpposingNotchesConcurrentFlips", input, output);
    }
    EXPECT_FALSE(output.hit_max_events)
        << "Infinite flip loop detected: hit_max_events is true at shift " << shift;
    EXPECT_NEAR(total_faces_area(output), 68.0, 1e-4);
    EXPECT_EQ(output.face_offsets.size(), 15);
  }
}

/**
 * 6-notch polygon designed to exhibit the 6-vertex collinear cycle of Section 3.2 (Figure 4).
 * Six reflex vertices (v2, v4, v6, v8 on bottom; v13, v16 on top) move towards y = 0
 * and all reach the line y = 0 simultaneously at t* = 1 / sqrt(5) ~= 0.447214:
 *   v16: (-5, 0)
 *   v2:  (-3, 0)
 *   v4:  (-1, 0)
 *   v6:  ( 1, 0)
 *   v8:  ( 3, 0)
 *   v13: ( 5, 0)
 * Box [-8, 8] x [-3, 3] has area 96; minus 6 notches of area 2 each = 84.
 */
TEST(straight_skeleton, SixNotchesConcurrentFlips)
{
  const char *spec = R"(19 1
-8.0 -4.0
-4.0 -4.0
-3.0 -1.0
-2.0 -4.0
-1.0 -1.0
 0.0 -4.0
 1.0 -1.0
 2.0 -4.0
 3.0 -1.0
 4.0 -4.0
 8.0 -4.0
 8.0  4.0
 6.0  4.0
 5.0  1.0
 4.0  4.0
-4.0  4.0
-5.0  1.0
-6.0  4.0
-8.0  4.0
0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18
)";

  for (int shift = 0; shift < 19; shift++) {
    InputStorage storage;
    StraightSkeletonInput input = fill_input_from_string(spec, storage);
    Array<double2> orig_verts = storage.vert;
    for (int i = 0; i < 19; i++) {
      storage.vert[i] = orig_verts[(i + shift) % 19];
    }
    const StraightSkeletonOutput output = straight_skeleton_calc(input);
    if (shift == 0) {
      print_test_output("SixNotchesConcurrentFlips", input, output);
    }
    EXPECT_FALSE(output.hit_max_events)
        << "Infinite flip loop detected: hit_max_events is true at shift " << shift;
  }
}

/**
 * Regular 64-gon approximating a circle.
 * In a regular polygon, all vertices converge to the center concurrently without splits,
 * yielding exactly n triangular faces and n skeleton rays from perimeter vertices to center.
 */
TEST(straight_skeleton, Circle64)
{
  const int n = 64;
  const double radius = 100.0;
  Array<double2> verts(n);
  Vector<int> outer_polygon(n);
  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    verts[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    outer_polygon[i] = i;
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.outer_polygon = outer_polygon.as_span();
  input.label = "Circle64";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets.size(), n);

  /* All faces must be triangles connecting consecutive boundary vertices to the center node. */
  for (const int fi : output.face_offsets.index_range()) {
    EXPECT_EQ(output.face_offsets[fi].size(), 3);
  }

  /* Exactly 65 vertices in the output: 64 perimeter vertices + 1 center node. */
  EXPECT_EQ(output.vert.size(), n + 1);

  /* Total area should match regular 64-gon: 0.5 * n * r^2 * sin(2*pi/n). */
  const double expected_area = 0.5 * double(n) * radius * radius *
                               std::sin(2.0 * M_PI / double(n));
  EXPECT_NEAR(total_faces_area(output), expected_area, 1e-4);
}

TEST(straight_skeleton, CircleWithHoles6)
{
  const int n = 6;
  const int hole_verts_num = 8;
  const double radius = 100.0;
  const int total_verts_num = n + n * hole_verts_num;

  Array<double2> verts(total_verts_num);
  Vector<int> outer_polygon;
  outer_polygon.reserve(n);

  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    verts[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    outer_polygon.append(i);
  }

  const double r_mid = radius * 0.5;
  const double d_adj = 2.0 * r_mid * std::sin(M_PI / double(n));
  const double r_hole = std::min(0.35 * r_mid, 0.35 * d_adj);

  Vector<int> hole_offsets_buf;
  hole_offsets_buf.reserve(n + 1);
  hole_offsets_buf.append(0);
  Vector<int> hole_vert_indices;
  hole_vert_indices.reserve(n * hole_verts_num);

  int v_idx = n;
  for (const int j : IndexRange(n)) {
    const double phi = (2.0 * M_PI * (double(j) + 0.5)) / double(n);
    const double2 center(r_mid * std::cos(phi), r_mid * std::sin(phi));

    for (const int k : IndexRange(hole_verts_num)) {
      const double alpha = (-2.0 * M_PI * double(k)) / double(hole_verts_num);
      verts[v_idx] = center + double2(r_hole * std::cos(alpha), r_hole * std::sin(alpha));
      hole_vert_indices.append(v_idx);
      v_idx++;
    }
    hole_offsets_buf.append(int(hole_vert_indices.size()));
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.outer_polygon = outer_polygon.as_span();
  input.hole_offsets = OffsetIndices<int>(hole_offsets_buf.as_span());
  input.hole_vert_indices = hole_vert_indices.as_span();
  input.label = "CircleWithHoles6";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets.size(), 54);
}

TEST(straight_skeleton, CircleWithHoles24)
{
  const int n = 24;
  const int hole_verts_num = 8;
  const double radius = 100.0;
  const int total_verts_num = n + n * hole_verts_num;

  Array<double2> verts(total_verts_num);
  Vector<int> outer_polygon;
  outer_polygon.reserve(n);

  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    verts[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    outer_polygon.append(i);
  }

  const double r_mid = radius * 0.5;
  const double d_adj = 2.0 * r_mid * std::sin(M_PI / double(n));
  const double r_hole = std::min(0.35 * r_mid, 0.35 * d_adj);

  Vector<int> hole_offsets_buf;
  hole_offsets_buf.reserve(n + 1);
  hole_offsets_buf.append(0);
  Vector<int> hole_vert_indices;
  hole_vert_indices.reserve(n * hole_verts_num);

  int v_idx = n;
  for (const int j : IndexRange(n)) {
    const double phi = (2.0 * M_PI * (double(j) + 0.5)) / double(n);
    const double2 center(r_mid * std::cos(phi), r_mid * std::sin(phi));

    for (const int k : IndexRange(hole_verts_num)) {
      const double alpha = (-2.0 * M_PI * double(k)) / double(hole_verts_num);
      verts[v_idx] = center + double2(r_hole * std::cos(alpha), r_hole * std::sin(alpha));
      hole_vert_indices.append(v_idx);
      v_idx++;
    }
    hole_offsets_buf.append(int(hole_vert_indices.size()));
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.outer_polygon = outer_polygon.as_span();
  input.hole_offsets = OffsetIndices<int>(hole_offsets_buf.as_span());
  input.hole_vert_indices = hole_vert_indices.as_span();
  input.label = "CircleWithHoles24";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets.size(), 216);
  EXPECT_NEAR(total_faces_area(output), 29641.554, 1e-2);
}

TEST(straight_skeleton, CircleWithHoles48)
{
  const int n = 48;
  const int hole_verts_num = 8;
  const double radius = 100.0;
  const int total_verts_num = n + n * hole_verts_num;

  Array<double2> verts(total_verts_num);
  Vector<int> outer_polygon;
  outer_polygon.reserve(n);

  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    verts[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    outer_polygon.append(i);
  }

  const double r_mid = radius * 0.5;
  const double d_adj = 2.0 * r_mid * std::sin(M_PI / double(n));
  const double r_hole = std::min(0.35 * r_mid, 0.35 * d_adj);

  Vector<int> hole_offsets_buf;
  hole_offsets_buf.reserve(n + 1);
  hole_offsets_buf.append(0);
  Vector<int> hole_vert_indices;
  hole_vert_indices.reserve(n * hole_verts_num);

  int v_idx = n;
  for (const int j : IndexRange(n)) {
    const double phi = (2.0 * M_PI * (double(j) + 0.5)) / double(n);
    const double2 center(r_mid * std::cos(phi), r_mid * std::sin(phi));

    for (const int k : IndexRange(hole_verts_num)) {
      const double alpha = (-2.0 * M_PI * double(k)) / double(hole_verts_num);
      verts[v_idx] = center + double2(r_hole * std::cos(alpha), r_hole * std::sin(alpha));
      hole_vert_indices.append(v_idx);
      v_idx++;
    }
    hole_offsets_buf.append(int(hole_vert_indices.size()));
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.outer_polygon = outer_polygon.as_span();
  input.hole_offsets = OffsetIndices<int>(hole_offsets_buf.as_span());
  input.hole_vert_indices = hole_vert_indices.as_span();
  input.label = "CircleWithHoles48";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets.size(), 432);
}

TEST(straight_skeleton, CircleWithHoles96)
{
  const int n = 96;
  const int hole_verts_num = 8;
  const double radius = 100.0;
  const int total_verts_num = n + n * hole_verts_num;

  Array<double2> verts(total_verts_num);
  Vector<int> outer_polygon;
  outer_polygon.reserve(n);

  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    verts[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    outer_polygon.append(i);
  }

  const double r_mid = radius * 0.5;
  const double d_adj = 2.0 * r_mid * std::sin(M_PI / double(n));
  const double r_hole = std::min(0.35 * r_mid, 0.35 * d_adj);

  Vector<int> hole_offsets_buf;
  hole_offsets_buf.reserve(n + 1);
  hole_offsets_buf.append(0);
  Vector<int> hole_vert_indices;
  hole_vert_indices.reserve(n * hole_verts_num);

  int v_idx = n;
  for (const int j : IndexRange(n)) {
    const double phi = (2.0 * M_PI * (double(j) + 0.5)) / double(n);
    const double2 center(r_mid * std::cos(phi), r_mid * std::sin(phi));

    for (const int k : IndexRange(hole_verts_num)) {
      const double alpha = (-2.0 * M_PI * double(k)) / double(hole_verts_num);
      verts[v_idx] = center + double2(r_hole * std::cos(alpha), r_hole * std::sin(alpha));
      hole_vert_indices.append(v_idx);
      v_idx++;
    }
    hole_offsets_buf.append(int(hole_vert_indices.size()));
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.outer_polygon = outer_polygon.as_span();
  input.hole_offsets = OffsetIndices<int>(hole_offsets_buf.as_span());
  input.hole_vert_indices = hole_vert_indices.as_span();
  input.label = "CircleWithHoles96";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets.size(), 864);
}

TEST(straight_skeleton, SquareWithHole_FiveSegments)
{
  const char *spec = R"(8 2
0.0 0.0
10.0 0.0
10.0 10.0
0.0 10.0
3.0 7.0
7.0 7.0
7.0 3.0
3.0 3.0
0 1 2 3
4 5 6 7
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "SquareWithHole_FiveSegments";
  input.segments = 5;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_TRUE(output.is_valid);
  EXPECT_TRUE(output.error_message.empty());
  EXPECT_NEAR(total_faces_area(output), 84.0, 1e-4);
}

TEST(straight_skeleton, FourWayStarSplit)
{
  const char *spec = R"(12 1
-1.0 4.0
-1.0 1.0
-4.0 1.0
-4.0 -1.0
-1.0 -1.0
-1.0 -4.0
1.0 -4.0
1.0 -1.0
4.0 -1.0
4.0 1.0
1.0 1.0
1.0 4.0
0 1 2 3 4 5 6 7 8 9 10 11
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "FourWayStarSplit";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.face_offsets.size(), 12);
  EXPECT_NEAR(total_faces_area(output), 28.0, 1e-4);
}

TEST(straight_skeleton, DumbbellCollinear)
{
  const char *spec = R"(12 1
2.0 1.0
-2.0 1.0
-2.0 4.0
-6.0 4.0
-6.0 -4.0
-2.0 -4.0
-2.0 -1.0
2.0 -1.0
2.0 -4.0
6.0 -4.0
6.0 4.0
2.0 4.0
0 1 2 3 4 5 6 7 8 9 10 11
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "DumbbellCollinear";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.face_offsets.size(), 12);
  EXPECT_NEAR(total_faces_area(output), 72.0, 1e-4);
}

TEST(straight_skeleton, DoubleCombSplit)
{
  /* 10-vert comb polygon with 2 reflex notches simultaneously splitting top edge:
   * (-6, 0) -> (-4, 0) -> (-3, 2) [reflex] -> (-2, 0) -> (2, 0) -> (3, 2) [reflex]
   * -> (4, 0) -> (6, 0) -> (6, 4) -> (-6, 4)
   * Area: 12 x 4 = 48 minus two triangles of base 2, height 2 (area 2 each) = 44.0.
   */
  const char *spec10 = R"(10 1
-6.0 0.0
-4.0 0.0
-3.0 2.0
-2.0 0.0
2.0 0.0
3.0 2.0
4.0 0.0
6.0 0.0
6.0 4.0
-6.0 4.0
0 1 2 3 4 5 6 7 8 9
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec10, storage);
  input.label = "DoubleCombSplit";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.face_offsets.size(), 10);
  EXPECT_NEAR(total_faces_area(output), 44.0, 1e-4);
}

TEST(straight_skeleton, AsymmetricHourglass)
{
  const char *spec = R"(6 1
-5.0 -3.0
0.0 -1.0
3.0 -2.0
3.0 2.0
0.0 1.0
-5.0 3.0
0 1 2 3 4 5
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "AsymmetricHourglass";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.face_offsets.size(), 6);
  EXPECT_NEAR(total_faces_area(output), 29.0, 1e-4);
}

TEST(straight_skeleton, ThreeWayStarSplit)
{
  const char *spec = R"(9 1
1.0000 4.0000
-1.0000 4.0000
-0.69282 0.40000
-3.96410 -1.13397
-2.96410 -2.86603
0.0000 -0.80000
2.96410 -2.86603
3.96410 -1.13397
0.69282 0.40000
0 1 2 3 4 5 6 7 8
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "ThreeWayStarSplit";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.face_offsets.size(), 9);
  EXPECT_NEAR(total_faces_area(output), 19.1138, 1e-3);
}

TEST(straight_skeleton, TShapeSplit)
{
  /* 8-vertex T-shaped polygon.
   * Two reflex vertices at (1, 2) and (-1, 2) move towards (0, 3) at speed sqrt(2).
   * At t = 1.0, they collide head-on at (0, 3), dividing the single closed wavefront
   * contour into TWO separate contours (the upper stem and the lower bar).
   */
  const char *spec = R"(8 1
-3.0 0.0
 3.0 0.0
 3.0 2.0
 1.0 2.0
 1.0 6.0
-1.0 6.0
-1.0 2.0
-3.0 2.0
0 1 2 3 4 5 6 7
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "TShapeSplit";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.face_offsets.size(), 8);
  EXPECT_NEAR(total_faces_area(output), 20.0, 1e-4);
}

TEST(straight_skeleton, DoubleHourglass)
{
  /* 16-vertex polygon with 3 lobes and 2 necks.
   * Neck 1 at x = -3, Neck 2 at x = 3.
   * At t = 1/sqrt(2) ~= 0.7071, two simultaneous neck vertex collisions occur,
   * dividing the single closed wavefront contour into THREE separate contours
   * (left, center, right lobes).
   */
  const char *spec = R"(16 1
-8.0 -2.0
-4.0 -2.0
-3.0 -1.0
-2.0 -2.0
2.0 -2.0
3.0 -1.0
4.0 -2.0
8.0 -2.0
8.0 2.0
4.0 2.0
3.0 1.0
2.0 2.0
-2.0 2.0
-3.0 1.0
-4.0 2.0
-8.0 2.0
0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "DoubleHourglass";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.face_offsets.size(), 16);
  EXPECT_NEAR(total_faces_area(output), 60.0, 1e-4);
}

TEST(straight_skeleton, TwoPeaksHourglass_EightSegments)
{
  /* 8-vertex polygon with two symmetric diamond lobes connected by a narrow hourglass neck.
   * Neck pinches at t ~= 0.35, dividing the wavefront into TWO separate contours.
   * Each diamond lobe continues to advance until all 4 edges converge simultaneously
   * to a single peak vertex at (-4, 0) and (4, 0).
   * 8 segments are used to show the separate evolution of the wavefronts towards both peaks.
   */
  const char *spec = R"(8 1
-8.0  0.0
-4.0 -3.0
 0.0 -0.6
 4.0 -3.0
 8.0  0.0
 4.0  3.0
 0.0  0.6
-4.0  3.0
0 1 2 3 4 5 6 7
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "TwoPeaksHourglass_EightSegments";
  input.segments = 8;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_NEAR(total_faces_area(output), 52.8, 1e-3);
}

TEST(straight_skeleton, ThreePeaksStar_EightSegments)
{
  /* 12-vertex 3-fold symmetric polygon with 3 pointed lobes and 3 reflex valleys.
   * At t ~= 0.73, the 3 reflex vertices collide at (0, 0), dividing the closed wavefront
   * into THREE separate contours. Each arm continues to advance until converging to its own
   * peak vertex. 8 segments are used to illustrate the separate wavefronts advancing.
   */
  const char *spec = R"(12 1
 1.7500  3.0311
 0.0000  6.0000
-1.7500  3.0311
-0.6928  0.4000
-3.5000 -0.0000
-5.1962 -3.0000
-1.7500 -3.0311
-0.0000 -0.8000
 1.7500 -3.0311
 5.1962 -3.0000
 3.5000 -0.0000
 0.6928  0.4000
0 1 2 3 4 5 6 7 8 9 10 11
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "ThreePeaksStar_EightSegments";
  input.segments = 8;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_NEAR(total_faces_area(output), 35.7, 1e-2);
}

TEST(straight_skeleton, ShiftedOverlap_EightSegments)
{
  const char *spec = R"(10 1
 1.0  1.0
-3.0  1.0
-5.0  3.0
-8.0  0.0
-5.0 -3.0
-1.0 -1.0
 3.0 -1.0
 5.0 -3.0
 8.0  0.0
 5.0  3.0
0 1 2 3 4 5 6 7 8 9
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "ShiftedOverlap_EightSegments";
  input.segments = 8;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_NEAR(total_faces_area(output), 50.0, 1e-3);
}

TEST(straight_skeleton, DiamondStarSplit)
{
  const char *spec = R"(8 1
 3.0  0.0
 1.0  1.0
 0.0  3.0
-1.0  1.0
-3.0  0.0
-1.0 -1.0
 0.0 -3.0
 1.0 -1.0
0 1 2 3 4 5 6 7
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "DiamondStarSplit";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_NEAR(total_faces_area(output), 12.0, 1e-3);
}

/* -------------------------------------------------------------------- */
/** \name Edge Chain Tests
 * \{ */

TEST(straight_skeleton, EdgeChain_SingleSegment)
{
  const Array<double2> verts = {double2(0.0, 0.0), double2(10.0, 0.0)};
  const Array<int> chain = {0, 1};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.edge_chain = chain.as_span();
  input.offset = 2.0;
  input.segments = 1;
  input.label = "EdgeChain_SingleSegment";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_EQ(output.face_offsets.size(), 4);
  /* (10 + 2*2) * (2*2) = 14 * 4 = 56.0. */
  EXPECT_NEAR(total_faces_area(output), 56.0, 1e-5);
  for (const int b : output.face_band) {
    EXPECT_EQ(b, 0);
  }
}

TEST(straight_skeleton, EdgeChain_SingleSegment_TwoBands)
{
  const Array<double2> verts = {double2(0.0, 0.0), double2(10.0, 0.0)};
  const Array<int> chain = {0, 1};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.edge_chain = chain.as_span();
  input.offset = 2.0;
  input.segments = 2;
  input.label = "EdgeChain_SingleSegment_TwoBands";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_EQ(output.face_offsets.size(), 8);
  EXPECT_NEAR(total_faces_area(output), 56.0, 1e-5);

  int band0_count = 0;
  int band1_count = 0;
  for (const int b : output.face_band) {
    if (b == 0) {
      band0_count++;
    }
    else if (b == 1) {
      band1_count++;
    }
  }
  EXPECT_EQ(band0_count, 4);
  EXPECT_EQ(band1_count, 4);
}

TEST(straight_skeleton, EdgeChain_LShape)
{
  const Array<double2> verts = {double2(0.0, 10.0), double2(0.0, 0.0), double2(10.0, 0.0)};
  const Array<int> chain = {0, 1, 2};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.edge_chain = chain.as_span();
  input.offset = 2.0;
  input.segments = 1;
  input.label = "EdgeChain_LShape";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_EQ(output.face_offsets.size(), 6);
  EXPECT_NEAR(total_faces_area(output), 96.0, 1e-4);
}

TEST(straight_skeleton, EdgeChain_NegativeOffset_NoOp)
{
  const Array<double2> verts = {double2(0.0, 0.0), double2(10.0, 0.0)};
  const Array<int> chain = {0, 1};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.edge_chain = chain.as_span();
  input.offset = -2.0;
  input.label = "EdgeChain_NegativeOffset_NoOp";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  EXPECT_EQ(output.face_offsets.size(), 0);
  EXPECT_EQ(output.vert.size(), 0);
}

TEST(straight_skeleton, EdgeChain_InfiniteOffset_NoOp)
{
  const Array<double2> verts = {double2(0.0, 0.0), double2(10.0, 0.0)};
  const Array<int> chain = {0, 1};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.edge_chain = chain.as_span();
  input.offset = std::numeric_limits<double>::infinity();
  input.label = "EdgeChain_InfiniteOffset_NoOp";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  EXPECT_EQ(output.face_offsets.size(), 0);
  EXPECT_EQ(output.vert.size(), 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Negative Offset (Polygon Outset) Tests
 * \{ */

TEST(straight_skeleton, NegativeOffset_Square)
{
  const Array<double2> verts = {
      double2(0.0, 0.0), double2(10.0, 0.0), double2(10.0, 10.0), double2(0.0, 10.0)};
  const Array<int> outer = {0, 1, 2, 3};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.outer_polygon = outer.as_span();
  input.offset = -2.0;
  input.segments = 1;
  input.label = "NegativeOffset_Square";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_EQ(output.face_offsets.size(), 4);
  /* Outset expands from [0, 10]^2 to [-2, 12]^2. Frame area = 14*14 - 100 = 96.0. */
  EXPECT_NEAR(total_faces_area(output), 96.0, 1e-4);
  for (const int b : output.face_band) {
    EXPECT_EQ(b, 0);
  }
}

TEST(straight_skeleton, NegativeOffset_Square_ThreeBands)
{
  const Array<double2> verts = {
      double2(0.0, 0.0), double2(10.0, 0.0), double2(10.0, 10.0), double2(0.0, 10.0)};
  const Array<int> outer = {0, 1, 2, 3};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.outer_polygon = outer.as_span();
  input.offset = -3.0;
  input.segments = 3;
  input.label = "NegativeOffset_Square_ThreeBands";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_EQ(output.face_offsets.size(), 12);
  /* Outset expands to [-3, 13]^2. Frame area = 16*16 - 100 = 156.0. */
  EXPECT_NEAR(total_faces_area(output), 156.0, 1e-4);

  Array<int> band_counts(3, 0);
  for (const int b : output.face_band) {
    if (b >= 0 && b < 3) {
      band_counts[b]++;
    }
  }
  EXPECT_EQ(band_counts[0], 4);
  EXPECT_EQ(band_counts[1], 4);
  EXPECT_EQ(band_counts[2], 4);
}

TEST(straight_skeleton, NegativeOffset_SquareWithHole)
{
  const Array<double2> verts = {
      /* Outer square (CCW). */
      double2(0.0, 0.0),
      double2(10.0, 0.0),
      double2(10.0, 10.0),
      double2(0.0, 10.0),
      /* Hole (CW). */
      double2(4.0, 4.0),
      double2(4.0, 6.0),
      double2(6.0, 6.0),
      double2(6.0, 4.0),
  };
  const Array<int> outer = {0, 1, 2, 3};
  const Array<int> hole_indices = {4, 5, 6, 7};
  const Array<int> hole_offsets_data = {0, 4};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.outer_polygon = outer.as_span();
  input.hole_vert_indices = hole_indices.as_span();
  input.hole_offsets = OffsetIndices<int>(hole_offsets_data.as_span());
  input.offset = -1.0;
  input.segments = 1;
  input.label = "NegativeOffset_SquareWithHole";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  /* Outer frame: [-1, 11]^2 \ [0, 10]^2 = 144 - 100 = 44.0.
   * Hole frame: [4, 6]^2 \ [5, 5]^2 = 4 - 0 = 4.0.
   * Total face area = 48.0. */
  EXPECT_NEAR(total_faces_area(output), 48.0, 1e-4);
}

TEST(straight_skeleton, NegativeOffset_InvalidInfinite)
{
  const Array<double2> verts = {
      double2(0.0, 0.0), double2(10.0, 0.0), double2(10.0, 10.0), double2(0.0, 10.0)};
  const Array<int> outer = {0, 1, 2, 3};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.outer_polygon = outer.as_span();
  input.offset = -std::numeric_limits<double>::infinity();
  input.label = "NegativeOffset_InvalidInfinite";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  EXPECT_EQ(output.face_offsets.size(), 0);
  EXPECT_EQ(output.vert.size(), 0);
}

TEST(straight_skeleton, EdgeChain_ThreeSegments_Collinear)
{
  const Array<double2> verts = {double2(0.0, 0.0), double2(5.0, 0.0), double2(10.0, 0.0)};
  const Array<int> chain = {0, 1, 2};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.edge_chain = chain.as_span();
  input.offset = 2.0;
  input.segments = 1;
  input.label = "EdgeChain_ThreeSegments_Collinear";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_EQ(output.face_offsets.size(), 6);
  EXPECT_NEAR(total_faces_area(output), 56.0, 1e-4);
}

TEST(straight_skeleton, EdgeChain_LShape_ThreeBands)
{
  const Array<double2> verts = {double2(0.0, 10.0), double2(0.0, 0.0), double2(10.0, 0.0)};
  const Array<int> chain = {0, 1, 2};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.edge_chain = chain.as_span();
  input.offset = 3.0;
  input.segments = 3;
  input.label = "EdgeChain_LShape_ThreeBands";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_EQ(output.face_offsets.size(), 18);
  EXPECT_NEAR(total_faces_area(output), 156.0, 1e-4);

  Array<int> band_counts(3, 0);
  for (const int b : output.face_band) {
    if (b >= 0 && b < 3) {
      band_counts[b]++;
    }
  }
  EXPECT_EQ(band_counts[0], 6);
  EXPECT_EQ(band_counts[1], 6);
  EXPECT_EQ(band_counts[2], 6);
}

TEST(straight_skeleton, NegativeOffset_LShape)
{
  const Array<double2> verts = {
      double2(0.0, 0.0),
      double2(10.0, 0.0),
      double2(10.0, 5.0),
      double2(5.0, 5.0),
      double2(5.0, 10.0),
      double2(0.0, 10.0),
  };
  const Array<int> outer = {0, 1, 2, 3, 4, 5};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.outer_polygon = outer.as_span();
  input.offset = -2.0;
  input.segments = 1;
  input.label = "NegativeOffset_LShape";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_EQ(output.face_offsets.size(), 6);
  /* Outset expands from domain area 75 to area 171. Frame area = 171 - 75 = 96.0. */
  EXPECT_NEAR(total_faces_area(output), 96.0, 1e-4);
}

TEST(straight_skeleton, EdgeChain_RectilinearSpiralSplit)
{
  const Array<double2> verts = {
      double2(0.0, 0.0),
      double2(6.0, 0.0),
      double2(6.0, 6.0),
      double2(2.0, 6.0),
      double2(2.0, 2.0),
      double2(4.0, 2.0),
  };
  const Array<int> chain = {0, 1, 2, 3, 4, 5};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.edge_chain = chain.as_span();
  input.offset = 1.5;
  input.segments = 1;
  input.label = "EdgeChain_RectilinearSpiralSplit";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_GT(output.events_processed_num, 0);
  EXPECT_EQ(output.face_offsets.size(), 12);
  EXPECT_NEAR(total_faces_area(output), 68.0, 1e-4);
  for (const int b : output.face_band) {
    EXPECT_EQ(b, 0);
  }
}

TEST(straight_skeleton, EdgeChain_RectilinearSpiral_TwoBands)
{
  const Array<double2> verts = {
      double2(0.0, 0.0),
      double2(6.0, 0.0),
      double2(6.0, 6.0),
      double2(2.0, 6.0),
      double2(2.0, 2.0),
      double2(4.0, 2.0),
  };
  const Array<int> chain = {0, 1, 2, 3, 4, 5};

  StraightSkeletonInput input;
  input.vert = verts.as_span();
  input.edge_chain = chain.as_span();
  input.offset = 1.5;
  input.segments = 2;
  input.label = "EdgeChain_RectilinearSpiral_TwoBands";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_GT(output.events_processed_num, 0);
  EXPECT_NEAR(total_faces_area(output), 68.0, 1e-4);

  int band0_count = 0;
  int band1_count = 0;
  for (const int b : output.face_band) {
    if (b == 0) {
      band0_count++;
    }
    else if (b == 1) {
      band1_count++;
    }
  }
  EXPECT_GT(band0_count, 0);
  EXPECT_GT(band1_count, 0);
}

TEST(straight_skeleton, MutualExclusionValidation)
{
  const Array<double2> verts = {
      double2(0.0, 0.0), double2(10.0, 0.0), double2(10.0, 10.0), double2(0.0, 10.0)};
  const Array<int> poly = {0, 1, 2, 3};
  const Array<int> chain = {0, 1, 2};

  /* Both outer_polygon and edge_chain provided: invalid. */
  {
    StraightSkeletonInput input;
    input.vert = verts.as_span();
    input.outer_polygon = poly.as_span();
    input.edge_chain = chain.as_span();
    input.offset = 1.0;
    const StraightSkeletonOutput output = straight_skeleton_calc(input);
    EXPECT_EQ(output.face_offsets.size(), 0);
  }

  /* Neither provided: invalid. */
  {
    StraightSkeletonInput input;
    input.vert = verts.as_span();
    input.offset = 1.0;
    const StraightSkeletonOutput output = straight_skeleton_calc(input);
    EXPECT_EQ(output.face_offsets.size(), 0);
  }

  /* edge_chain with hole: invalid. */
  {
    const Array<int> hole_indices = {0, 1, 2};
    const Array<int> hole_offsets_data = {0, 3};
    StraightSkeletonInput input;
    input.vert = verts.as_span();
    input.edge_chain = chain.as_span();
    input.hole_vert_indices = hole_indices.as_span();
    input.hole_offsets = OffsetIndices<int>(hole_offsets_data.as_span());
    input.offset = 1.0;
    const StraightSkeletonOutput output = straight_skeleton_calc(input);
    EXPECT_EQ(output.face_offsets.size(), 0);
  }
}

/** \} */

} /* namespace blender::straightskeleton::test */
