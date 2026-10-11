/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
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
  const OffsetIndices<int> face_offsets = output.face_offsets();

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
  for (const int2 &e : input.edge) {
    input_boundary_edges.add({std::min(e[0], e[1]), std::max(e[0], e[1])});
  }
  for (const int f : IndexRange(input.face_offsets.size())) {
    const IndexRange r = input.face_offsets[f];
    const int n_f = int(r.size());
    for (int k = 0; k < n_f; k++) {
      const int u = input.face_vert_indices[r[k]];
      const int v = input.face_vert_indices[r[(k + 1) % n_f]];
      input_boundary_edges.add({std::min(u, v), std::max(u, v)});
    }
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
    const char *env_dir = std::getenv("STRAIGHT_SKELETON_SVG_DIR");
    const std::string dir = (env_dir && env_dir[0] != '\0') ? env_dir : "/tmp";
    std::string svg_path = dir + "/" + safe_name + ".svg";
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
  Vector<int> face_offsets_data;
  Vector<int> face_vert_indices;
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
  r_storage.face_offsets_data.clear();
  r_storage.face_offsets_data.append(0);
  r_storage.face_vert_indices.clear();

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
    while (fss >> v) {
      r_storage.face_vert_indices.append(v);
    }
    r_storage.face_offsets_data.append(int(r_storage.face_vert_indices.size()));
    i++;
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = r_storage.vert.as_span();
  input.face_offsets = OffsetIndices<int>(r_storage.face_offsets_data.as_span());
  input.face_vert_indices = r_storage.face_vert_indices.as_span();
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
  for (const int fi : output.face_offsets().index_range()) {
    const IndexRange r = output.face_offsets()[fi];
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

  std::cout << "\nFaces (" << output.face_offsets().size() << "):\n";
  const Span<int> fvi = output.face_vert_indices;
  double total_area = 0.0;
  for (const int fi : output.face_offsets().index_range()) {
    const IndexRange r = output.face_offsets()[fi];
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
  EXPECT_EQ(output.face_offsets().size(), 3);

  /* Every output face should be a triangle (3 verts). */
  for (const int fi : output.face_offsets().index_range()) {
    EXPECT_EQ(output.face_offsets()[fi].size(), 3);
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
  EXPECT_EQ(output.face_orig.size(), int64_t(output.face_offsets().size()));
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
  EXPECT_EQ(output.face_offsets().size(), 23);
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
  EXPECT_EQ(output.face_offsets().size(), 7);
  EXPECT_NEAR(total_faces_area(output), 71.1, 1e-4);
  for (const int fi : output.face_offsets().index_range()) {
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
  EXPECT_EQ(output.face_offsets().size(), 4);
  EXPECT_NEAR(total_faces_area(output), 4.0, 1e-6);
  for (const int fi : output.face_offsets().index_range()) {
    EXPECT_EQ(output.face_offsets()[fi].size(), 3);
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
  EXPECT_EQ(output.face_offsets().size(), 6);
  EXPECT_NEAR(total_faces_area(output), 12.0, 1e-6);
  for (const int fi : output.face_offsets().index_range()) {
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

  EXPECT_EQ(output.face_offsets().size(), 4);
  EXPECT_NEAR(total_faces_area(output), 8.0, 1e-6);
  for (const int fi : output.face_offsets().index_range()) {
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

  EXPECT_EQ(output.face_offsets().size(), 4);
  EXPECT_NEAR(total_faces_area(output), 16.0, 1e-6);
  for (const int fi : output.face_offsets().index_range()) {
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
  EXPECT_EQ(output.face_offsets().size(), 6);
  EXPECT_NEAR(total_faces_area(output), 6.0, 1e-6);
  for (const int fi : output.face_offsets().index_range()) {
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
  EXPECT_EQ(output.face_offsets().size(), 20);
  EXPECT_NEAR(total_faces_area(output), 1.021875, 1e-5);
  for (const int fi : output.face_offsets().index_range()) {
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
  for (const int fi : output.face_offsets().index_range()) {
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
  for (const int fi : output.face_offsets().index_range()) {
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
  const Array<int> face_offsets_data = {0, 4};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_verts.as_span();
  input.offset = 1.0;
  input.segments = 1;
  input.label = "OffsetSquarePartial";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* 4 side-strip trapezoids + 1 inner square. */
  EXPECT_EQ(output.face_offsets().size(), 5);

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
  for (const int fi : output.face_offsets().index_range()) {
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
  const Array<int> face_offsets_data = {0, 4};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_verts.as_span();
  input.offset = 5.0;
  input.segments = 1;
  input.label = "OffsetSquareCollapse";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* 4 side-strip triangles (apex at center). */
  EXPECT_EQ(output.face_offsets().size(), 4);

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
  const Array<int> face_offsets_data = {0, 4};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_verts.as_span();
  input.offset = 10.0; /* Exceeds max_collapse_time = 5, clamped. */
  input.segments = 1;
  input.label = "OffsetSquareExcess";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Same as OffsetSquareCollapse: 4 triangular strips to the apex. */
  EXPECT_EQ(output.face_offsets().size(), 4);

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
  const Array<int> face_offsets_data = {0, 4};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_verts.as_span();
  input.offset = 2.0;
  input.segments = 1;
  input.label = "OffsetRectangle";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_EQ(output.face_offsets().size(), 5);

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
  const Array<int> face_offsets_data = {0, 6};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_verts.as_span();
  input.offset = 0.5;
  input.segments = 1;
  input.label = "OffsetLShape";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_EQ(output.face_offsets().size(), 7); /* 6 side-strips + 1 inner L-shape. */

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
  const Array<int> face_offsets_data = {0, 4};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_verts.as_span();
  input.offset = 2.0;
  input.segments = 2;
  input.label = "OffsetSquare_TwoBands";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Band 0 (t=0..1): 4 side-strip trapezoids.
   * Band 1 (t=1..2): 4 side-strip trapezoids + 1 inner square.
   * Total = 9 faces. */
  EXPECT_EQ(output.face_offsets().size(), 9);

  /* Output contains only 4 input corners + 4 cut vertices (t=1) + 4 cut vertices (t=2).
   * Apex at t = 5 is omitted. */
  EXPECT_EQ(output.vert.size(), 12);
  for (const int vi : output.vert.index_range()) {
    EXPECT_LE(output.vert_time[vi], 2.0 + 1e-6);
  }

  /* Check band assignments. */
  int band0_count = 0;
  int band1_count = 0;
  for (const int fi : output.face_offsets().index_range()) {
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
  const Array<int> face_offsets_data = {0, 4};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_verts.as_span();
  /* Default input.offset = infinity; segments = 2 divides the full skeleton into 2 bands. */
  input.segments = 2;
  input.label = "FullSkeletonWithSegments";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Band 0 (t=0..2.5): 4 side-strip trapezoids.
   * Band 1 (t=2.5..5): 4 side-strip triangles converging to the apex.
   *   The apex inner polygon degenerates to a point (area ~= 0) and is silently omitted.
   * Total = 8 faces. */
  EXPECT_EQ(output.face_offsets().size(), 8);

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
  EXPECT_EQ(output.face_offsets().size(), 19);
  for (const int fi : output.face_offsets().index_range()) {
    const IndexRange face_r = output.face_offsets()[fi];
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
  EXPECT_EQ(output.face_offsets().size(), 8);

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
  for (const int fi : output.face_offsets().index_range()) {
    const int orig = output.face_orig[fi];
    const IndexRange r = output.face_offsets()[fi];
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

TEST(straight_skeleton, SquareWithHole_HoleFirst)
{
  /* Same geometry as SquareWithHole, but with the hole face defined before the outer face.
   * Tests that holes appearing earlier in the input than outer boundaries do not prematurely
   * collapse to their centroid. */
  const char *spec = R"(8 2
3.0 7.0
7.0 7.0
7.0 3.0
3.0 3.0
0.0 0.0
10.0 0.0
10.0 10.0
0.0 10.0
0 1 2 3
4 5 6 7
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "SquareWithHole_HoleFirst";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.vert.size(), 12);
  EXPECT_EQ(output.edge.size(), 20);
  EXPECT_EQ(output.face_offsets().size(), 8);
  EXPECT_NEAR(total_faces_area(output), 84.0, 1e-5);
}

TEST(straight_skeleton, CAD_Sharps_Spline19)
{
  const Array<double2> verts = {
      double2(-16.926633834838867, 2.4982891082763672),
      double2(-15.84699821472168, 1.7297611236572266),
      double2(-13.759855270385742, 1.4625186920166016),
      double2(-12.884267807006836, 1.9274272918701172),
      double2(-13.941900253295898, 2.4826946258544922),
      double2(-12.826894760131836, 2.0158023834228516),
      double2(-12.361112594604492, 2.4609584808349609),
      double2(-13.017965316772461, 4.2290325164794922),
      double2(-13.535337448120117, 3.7918605804443359),
      double2(-13.391790390014648, 4.7127132415771484),
      double2(-13.870275497436523, 5.0940914154052734),
      double2(-14.506338119506836, 5.3221263885498047),
      double2(-15.39472770690918, 5.3651180267333984),
      double2(-16.07838249206543, 5.3129138946533203),
      double2(-17.202375411987305, 4.4607753753662109),
  };
  const Array<int> face_offsets_data = {0, 15};
  const Array<int> face_vert_indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Inside;
  input.full_verification = true;
  input.label = "CAD_Sharps_Spline19";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_FALSE(output.hit_max_events);
}

TEST(straight_skeleton, CAD_Sharps_Spline0)
{
  const Array<double2> verts = {
      double2(-7.423315048217773, -10.645959854125977),
      double2(-7.766538619995117, -10.937929153442383),
      double2(-6.685323715209961, -11.172159194946289),
      double2(-6.608587265014648, -10.997278213500977),
      double2(-6.050561904907227, -11.08903694152832),
      double2(-6.100496292114258, -10.987569808959961),
      double2(-6.450246810913086, -10.049703598022461),
      double2(-6.065572738647461, -10.751306533813477),
      double2(-6.047491073608398, -10.726449966430664),
      double2(-6.047239303588867, -10.67747688293457),
      double2(-6.323675155639648, -9.883687973022461),
      double2(-6.436563491821289, -9.632322311401367),
      double2(-5.664213180541992, -10.247285842895508),
      double2(-5.607183456420898, -10.789583206176758),
      double2(-5.398881912231445, -11.030412673950195),
      double2(-4.835329055786133, -10.99528694152832),
      double2(-4.541448593139648, -10.876871109008789),
      double2(-5.239187240600586, -10.475061416625977),
      double2(-5.56505012512207, -10.162683486938477),
      double2(-4.870882034301758, -10.246461868286133),
      double2(-4.550771713256836, -10.56266975402832),
      double2(-3.700704574584961, -11.034887313842773),
      double2(-3.5234909057617188, -10.895456314086914),
      double2(-2.7203292846679688, -10.800275802612305),
      double2(-3.570772171020508, -10.661653518676758),
      double2(-1.9879093170166016, -10.693498611450195),
      double2(-1.8352012634277344, -10.634729385375977),
      double2(-1.9547290802001953, -9.916547775268555),
      double2(-1.8688011169433594, -9.679498672485352),
      double2(-3.220125198364258, -10.57762336730957),
      double2(-1.9299297332763672, -9.69770622253418),
      double2(-3.0427513122558594, -10.086965560913086),
      double2(-2.896413803100586, -9.852293014526367),
      double2(-2.3250904083251953, -9.368131637573242),
      double2(-1.8499469757080078, -8.214136123657227),
      double2(-2.8171520233154297, -9.183469772338867),
      double2(-2.1311702728271484, -7.756200790405273),
      double2(-2.5096988677978516, -8.205636978149414),
      double2(-3.0441226959228516, -7.800649642944336),
      double2(-3.459085464477539, -8.03400993347168),
      double2(-3.785165786743164, -8.426229476928711),
      double2(-3.795633316040039, -8.691343307495117),
      double2(-3.668428421020508, -8.777776718139648),
      double2(-3.030548095703125, -8.620344161987305),
      double2(-3.4702091217041016, -8.984582901000977),
      double2(-3.643728256225586, -9.014223098754883),
      double2(-4.493200302124023, -7.729387283325195),
      double2(-3.909801483154297, -9.371236801147461),
      double2(-4.852804183959961, -7.860849380493164),
      double2(-6.047536849975586, -8.014352798461914),
      double2(-5.348600387573242, -8.128133773803711),
      double2(-5.382551193237305, -8.246183395385742),
      double2(-5.385274887084961, -8.525846481323242),
      double2(-5.769628524780273, -8.403963088989258),
      double2(-5.21531867980957, -8.892698287963867),
      double2(-5.020147323608398, -9.065610885620117),
      double2(-4.822568893432617, -8.629716873168945),
      double2(-4.808202743530273, -8.927995681762695),
      double2(-4.315244674682617, -9.575395584106445),
      double2(-4.708112716674805, -9.397298812866211),
      double2(-4.743219375610352, -9.464879989624023),
      double2(-3.4280529022216797, -10.094743728637695),
      double2(-3.4941234588623047, -10.206621170043945),
      double2(-4.352670669555664, -9.988199234008789),
      double2(-3.999094009399414, -10.411325454711914),
      double2(-4.432435989379883, -10.242311477661133),
      double2(-4.397912979125977, -10.40953254699707),
      double2(-5.150224685668945, -9.798318862915039),
      double2(-5.666860580444336, -9.197397232055664),
      double2(-5.456037521362305, -9.324007034301758),
      double2(-5.323118209838867, -9.305551528930664),
      double2(-6.190290451049805, -8.419900894165039),
      double2(-6.17192268371582, -8.452585220336914),
      double2(-6.683645248413086, -8.180074691772461),
      double2(-5.935667037963867, -9.006486892700195),
      double2(-7.102537155151367, -7.829160690307617),
      double2(-6.430887222290039, -9.076318740844727),
      double2(-7.052103042602539, -8.095758438110352),
      double2(-7.311166763305664, -8.312437057495117),
      double2(-7.311285018920898, -8.245416641235352),
      double2(-7.834836959838867, -7.829061508178711),
      double2(-6.570524215698242, -9.289899826049805),
      double2(-7.502630233764648, -8.364953994750977),
      double2(-7.305395126342773, -8.606313705444336),
      double2(-7.474081039428711, -8.49650764465332),
      double2(-7.825399398803711, -8.246423721313477),
      double2(-8.109434127807617, -8.30207633972168),
      double2(-7.424917221069336, -8.986295700073242),
      double2(-7.058954238891602, -9.38575553894043),
      double2(-6.768457412719727, -9.900476455688477),
      double2(-6.644025802612305, -10.358118057250977),
      double2(-7.048879623413086, -9.560251235961914),
      double2(-7.099859237670898, -9.601694107055664),
      double2(-7.721853256225586, -9.151330947875977),
      double2(-8.174314498901367, -9.213506698608398),
      double2(-7.160772323608398, -10.03639030456543),
      double2(-8.157880783081055, -9.657285690307617),
      double2(-8.004156112670898, -9.908254623413086),
      double2(-8.322149276733398, -10.86253547668457),
      double2(-8.031339645385742, -10.417657852172852),
  };
  const Array<int> face_offsets_data = {0, 100};
  Array<int> face_vert_indices(100);
  for (int i = 0; i < 100; i++)
    face_vert_indices[i] = i;

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Inside;
  input.full_verification = true;
  input.label = "CAD_Sharps_Spline0";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_TRUE(output.is_valid);
  EXPECT_FALSE(output.hit_max_events);
}

TEST(straight_skeleton, Multi_Spline113)
{
  const Array<double2> verts = {
      double2(-25.87818145751953, 18.453819274902344),
      double2(-25.320636749267578, 18.538463592529297),
      double2(-25.871074676513672, 18.411121368408203),
      double2(-25.754817962646484, 18.053333282470703),
      double2(-25.245319366455078, 18.306896209716797),
      double2(-25.734909057617188, 18.01491928100586),
      double2(-25.51382827758789, 17.710628509521484),
      double2(-25.102088928222656, 18.11001968383789),
      double2(-25.483070373535156, 17.680206298828125),
      double2(-25.178829193115234, 17.45916748046875),
      double2(-24.905086517333984, 17.967010498046875),
      double2(-25.140216827392578, 17.439666748046875),
      double2(-24.782604217529297, 17.323471069335938),
      double2(-24.673568725585938, 17.89188003540039),
      double2(-24.739871978759766, 17.316753387451172),
      double2(-24.363872528076172, 17.316753387451172),
      double2(-24.430179595947266, 17.891876220703125),
      double2(-24.32114028930664, 17.323471069335938),
      double2(-23.96352767944336, 17.439666748046875),
      double2(-24.198654174804688, 17.967002868652344),
      double2(-23.924915313720703, 17.45916748046875),
      double2(-23.62067413330078, 17.680206298828125),
      double2(-24.001625061035156, 18.10997772216797),
      double2(-23.589923858642578, 17.710628509521484),
      double2(-23.36884307861328, 18.01491928100586),
      double2(-23.858448028564453, 18.306903839111328),
      double2(-23.348926544189453, 18.053333282470703),
      double2(-23.232669830322266, 18.411121368408203),
      double2(-23.78308868408203, 18.53845977783203),
      double2(-23.225570678710938, 18.453819274902344),
      double2(-23.225570678710938, 18.76810073852539),
      double2(-31.672035217285156, 18.76810073852539),
      double2(-31.672035217285156, 18.453819274902344),
      double2(-31.114490509033203, 18.538463592529297),
      double2(-31.664928436279297, 18.411121368408203),
      double2(-31.54867172241211, 18.053333282470703),
      double2(-31.039173126220703, 18.306896209716797),
      double2(-31.528762817382812, 18.01491928100586),
      double2(-31.307682037353516, 17.710628509521484),
      double2(-30.89594268798828, 18.11001968383789),
      double2(-31.27692413330078, 17.680206298828125),
      double2(-30.97268295288086, 17.45916748046875),
      double2(-30.69894027709961, 17.967010498046875),
      double2(-30.934070587158203, 17.439666748046875),
      double2(-30.576457977294922, 17.323471069335938),
      double2(-30.467422485351562, 17.89188003540039),
      double2(-30.53372573852539, 17.316753387451172),
      double2(-30.157726287841797, 17.316753387451172),
      double2(-30.22403335571289, 17.891876220703125),
      double2(-30.114994049072266, 17.323471069335938),
      double2(-29.757381439208984, 17.439666748046875),
      double2(-29.992507934570312, 17.967002868652344),
      double2(-29.718769073486328, 17.45916748046875),
      double2(-29.414527893066406, 17.680206298828125),
      double2(-29.79547882080078, 18.10997772216797),
      double2(-29.383777618408203, 17.710628509521484),
      double2(-29.162696838378906, 18.01491928100586),
      double2(-29.652301788330078, 18.306903839111328),
      double2(-29.142780303955078, 18.053333282470703),
      double2(-29.02652359008789, 18.411121368408203),
      double2(-29.576942443847656, 18.53845977783203),
      double2(-29.019424438476562, 18.453819274902344),
      double2(-28.7159423828125, 18.453819274902344),
      double2(-28.158397674560547, 18.538463592529297),
      double2(-28.70883560180664, 18.411121368408203),
      double2(-28.592578887939453, 18.053333282470703),
      double2(-28.083080291748047, 18.306896209716797),
      double2(-28.572669982910156, 18.01491928100586),
      double2(-28.35158920288086, 17.710628509521484),
      double2(-27.939849853515625, 18.11001968383789),
      double2(-28.320831298828125, 17.680206298828125),
      double2(-28.016590118408203, 17.45916748046875),
      double2(-27.742847442626953, 17.967010498046875),
      double2(-27.977977752685547, 17.439666748046875),
      double2(-27.620365142822266, 17.323471069335938),
      double2(-27.511329650878906, 17.89188003540039),
      double2(-27.577632904052734, 17.316753387451172),
      double2(-27.20163345336914, 17.316753387451172),
      double2(-27.267940521240234, 17.891876220703125),
      double2(-27.15890121459961, 17.323471069335938),
      double2(-26.801288604736328, 17.439666748046875),
      double2(-27.036415100097656, 17.967002868652344),
      double2(-26.762676239013672, 17.45916748046875),
      double2(-26.45843505859375, 17.680206298828125),
      double2(-26.839385986328125, 18.10997772216797),
      double2(-26.427684783935547, 17.710628509521484),
      double2(-26.20660400390625, 18.01491928100586),
      double2(-26.696208953857422, 18.306903839111328),
      double2(-26.186687469482422, 18.053333282470703),
      double2(-26.070430755615234, 18.411121368408203),
      double2(-26.620849609375, 18.53845977783203),
      double2(-26.063331604003906, 18.453819274902344),
  };
  const Array<int> face_offsets_data = {0, 92};
  Array<int> face_vert_indices(92);
  for (int i = 0; i < 92; i++) {
    face_vert_indices[i] = i;
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Inside;
  input.full_verification = true;
  input.label = "Multi_Spline113";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_TRUE(output.is_valid);
  EXPECT_FALSE(output.hit_max_events);
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
  for (const int fi : output.face_offsets().index_range()) {
    const IndexRange face_r = output.face_offsets()[fi];
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
  for (const int fi : output.face_offsets().index_range()) {
    const IndexRange face_r = output.face_offsets()[fi];
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
    EXPECT_EQ(output.face_offsets().size(), 15);
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
  Vector<int> face_vert_indices(n);
  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    verts[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    face_vert_indices[i] = i;
  }
  const Array<int> face_offsets_data = {0, n};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.label = "Circle64";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets().size(), n);

  /* All faces must be triangles connecting consecutive boundary vertices to the center node. */
  for (const int fi : output.face_offsets().index_range()) {
    EXPECT_EQ(output.face_offsets()[fi].size(), 3);
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
  Vector<int> face_offsets_data;
  face_offsets_data.reserve(n + 2);
  face_offsets_data.append(0);
  face_offsets_data.append(n);
  Vector<int> face_vert_indices;
  face_vert_indices.reserve(total_verts_num);

  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    verts[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    face_vert_indices.append(i);
  }

  const double r_mid = radius * 0.5;
  const double d_adj = 2.0 * r_mid * std::sin(M_PI / double(n));
  const double r_hole = std::min(0.35 * r_mid, 0.35 * d_adj);

  int v_idx = n;
  for (const int j : IndexRange(n)) {
    const double phi = (2.0 * M_PI * (double(j) + 0.5)) / double(n);
    const double2 center(r_mid * std::cos(phi), r_mid * std::sin(phi));

    for (const int k : IndexRange(hole_verts_num)) {
      const double alpha = (-2.0 * M_PI * double(k)) / double(hole_verts_num);
      verts[v_idx] = center + double2(r_hole * std::cos(alpha), r_hole * std::sin(alpha));
      face_vert_indices.append(v_idx);
      v_idx++;
    }
    face_offsets_data.append(int(face_vert_indices.size()));
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.label = "CircleWithHoles6";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets().size(), 54);
}

TEST(straight_skeleton, CircleWithHoles24)
{
  const int n = 24;
  const int hole_verts_num = 8;
  const double radius = 100.0;
  const int total_verts_num = n + n * hole_verts_num;

  Array<double2> verts(total_verts_num);
  Vector<int> face_offsets_data;
  face_offsets_data.reserve(n + 2);
  face_offsets_data.append(0);
  face_offsets_data.append(n);
  Vector<int> face_vert_indices;
  face_vert_indices.reserve(total_verts_num);

  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    verts[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    face_vert_indices.append(i);
  }

  const double r_mid = radius * 0.5;
  const double d_adj = 2.0 * r_mid * std::sin(M_PI / double(n));
  const double r_hole = std::min(0.35 * r_mid, 0.35 * d_adj);

  int v_idx = n;
  for (const int j : IndexRange(n)) {
    const double phi = (2.0 * M_PI * (double(j) + 0.5)) / double(n);
    const double2 center(r_mid * std::cos(phi), r_mid * std::sin(phi));

    for (const int k : IndexRange(hole_verts_num)) {
      const double alpha = (-2.0 * M_PI * double(k)) / double(hole_verts_num);
      verts[v_idx] = center + double2(r_hole * std::cos(alpha), r_hole * std::sin(alpha));
      face_vert_indices.append(v_idx);
      v_idx++;
    }
    face_offsets_data.append(int(face_vert_indices.size()));
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.label = "CircleWithHoles24";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets().size(), 216);
  EXPECT_NEAR(total_faces_area(output), 29641.554, 1e-2);
}

TEST(straight_skeleton, CircleWithHoles48)
{
  const int n = 48;
  const int hole_verts_num = 8;
  const double radius = 100.0;
  const int total_verts_num = n + n * hole_verts_num;

  Array<double2> verts(total_verts_num);
  Vector<int> face_offsets_data;
  face_offsets_data.reserve(n + 2);
  face_offsets_data.append(0);
  face_offsets_data.append(n);
  Vector<int> face_vert_indices;
  face_vert_indices.reserve(total_verts_num);

  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    verts[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    face_vert_indices.append(i);
  }

  const double r_mid = radius * 0.5;
  const double d_adj = 2.0 * r_mid * std::sin(M_PI / double(n));
  const double r_hole = std::min(0.35 * r_mid, 0.35 * d_adj);

  int v_idx = n;
  for (const int j : IndexRange(n)) {
    const double phi = (2.0 * M_PI * (double(j) + 0.5)) / double(n);
    const double2 center(r_mid * std::cos(phi), r_mid * std::sin(phi));

    for (const int k : IndexRange(hole_verts_num)) {
      const double alpha = (-2.0 * M_PI * double(k)) / double(hole_verts_num);
      verts[v_idx] = center + double2(r_hole * std::cos(alpha), r_hole * std::sin(alpha));
      face_vert_indices.append(v_idx);
      v_idx++;
    }
    face_offsets_data.append(int(face_vert_indices.size()));
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.label = "CircleWithHoles48";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets().size(), 432);
}

TEST(straight_skeleton, CircleWithHoles96)
{
  const int n = 96;
  const int hole_verts_num = 8;
  const double radius = 100.0;
  const int total_verts_num = n + n * hole_verts_num;

  Array<double2> verts(total_verts_num);
  Vector<int> face_offsets_data;
  face_offsets_data.reserve(n + 2);
  face_offsets_data.append(0);
  face_offsets_data.append(n);
  Vector<int> face_vert_indices;
  face_vert_indices.reserve(total_verts_num);

  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    verts[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    face_vert_indices.append(i);
  }

  const double r_mid = radius * 0.5;
  const double d_adj = 2.0 * r_mid * std::sin(M_PI / double(n));
  const double r_hole = std::min(0.35 * r_mid, 0.35 * d_adj);

  int v_idx = n;
  for (const int j : IndexRange(n)) {
    const double phi = (2.0 * M_PI * (double(j) + 0.5)) / double(n);
    const double2 center(r_mid * std::cos(phi), r_mid * std::sin(phi));

    for (const int k : IndexRange(hole_verts_num)) {
      const double alpha = (-2.0 * M_PI * double(k)) / double(hole_verts_num);
      verts[v_idx] = center + double2(r_hole * std::cos(alpha), r_hole * std::sin(alpha));
      face_vert_indices.append(v_idx);
      v_idx++;
    }
    face_offsets_data.append(int(face_vert_indices.size()));
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.label = "CircleWithHoles96";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets().size(), 864);
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

TEST(straight_skeleton, SquareWithHole_AnnularOffset)
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
  input.label = "SquareWithHole_AnnularOffset";
  input.offset = 0.5;
  input.segments = 1;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_TRUE(output.is_valid);
  EXPECT_TRUE(output.error_message.empty());

  /* Total area must equal initial domain area 100 - 16 = 84. */
  EXPECT_NEAR(total_faces_area(output), 84.0, 1e-4);

  /* Side strips: 4 outer + 4 hole = 8 faces.
   * Cap: 2 annular split faces. Total = 10 faces. */
  EXPECT_EQ(output.face_offsets().size(), 10);

  /* Verify no vertex repeats in any face. */
  for (const int fi : output.face_offsets().index_range()) {
    const IndexRange face_r = output.face_offsets()[fi];
    EXPECT_GE(face_r.size(), 3);
    Set<int> unique_verts;
    for (const int i : face_r) {
      EXPECT_TRUE(unique_verts.add(output.face_vert_indices[i]));
    }
  }
}

TEST(straight_skeleton, LetterBWithHoles)
{
  /* Letter 'B' with an outer boundary loop (52 vertices) and two inner counter holes
   * (27 vertices each), totaling 106 vertices. Tests micro-contour resolution and
   * reflex split detection on adjacent edges without stalling or producing pinch vertices. */
  const char *spec = R"(106 3
0.075000 0.000000
0.309000 0.000000
0.349526 0.001977
0.385708 0.007648
0.417703 0.016625
0.445667 0.028519
0.469755 0.042940
0.490125 0.059500
0.506932 0.077810
0.520333 0.097481
0.530484 0.118125
0.537542 0.139352
0.541662 0.160773
0.543000 0.182000
0.541721 0.204466
0.537935 0.225810
0.531719 0.245953
0.523148 0.264815
0.512300 0.282315
0.499250 0.298375
0.484075 0.312914
0.466852 0.325852
0.447656 0.337109
0.426565 0.346606
0.403654 0.354263
0.379000 0.360000
0.379000 0.362000
0.395961 0.369609
0.411356 0.377954
0.425203 0.387063
0.437519 0.396963
0.448320 0.407683
0.457625 0.419250
0.465451 0.431692
0.471815 0.445037
0.476734 0.459312
0.480227 0.474546
0.482310 0.490766
0.483000 0.508000
0.481675 0.529490
0.477653 0.550750
0.470859 0.571469
0.461222 0.591333
0.448668 0.610031
0.433125 0.627250
0.414519 0.642677
0.392778 0.656000
0.367828 0.666906
0.339597 0.675083
0.308012 0.680219
0.273000 0.682000
0.075000 0.682000
0.173000 0.594000
0.263000 0.594000
0.284248 0.593105
0.303065 0.590505
0.319562 0.586328
0.333852 0.580704
0.346044 0.573760
0.356250 0.565625
0.364581 0.556428
0.371148 0.546296
0.376062 0.535359
0.379435 0.523745
0.381377 0.511583
0.382000 0.499000
0.381019 0.481520
0.378069 0.465579
0.373141 0.451172
0.366222 0.438296
0.357304 0.426948
0.346375 0.417125
0.333425 0.408822
0.318444 0.402037
0.301422 0.396766
0.282347 0.393005
0.261210 0.390751
0.238000 0.390000
0.173000 0.390000
0.173000 0.302000
0.291000 0.302000
0.315608 0.301098
0.337949 0.298454
0.358047 0.294156
0.375926 0.288296
0.391611 0.280964
0.405125 0.272250
0.416494 0.262244
0.425741 0.251037
0.432891 0.238719
0.437968 0.225380
0.440996 0.211110
0.442000 0.196000
0.441324 0.183161
0.439171 0.170292
0.435359 0.157609
0.429704 0.145333
0.422020 0.133682
0.412125 0.122875
0.399834 0.113130
0.384963 0.104667
0.367328 0.097703
0.346745 0.092458
0.323031 0.089151
0.296000 0.088000
0.173000 0.088000
0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51
52 53 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70 71 72 73 74 75 76 77 78
79 80 81 82 83 84 85 86 87 88 89 90 91 92 93 94 95 96 97 98 99 100 101 102 103 104 105
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "LetterBWithHoles";
  input.offset = std::numeric_limits<double>::infinity();
  input.segments = 1;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_TRUE(output.is_valid);
  EXPECT_TRUE(output.error_message.empty());

  /* All 106 boundary edges must have corresponding valid roof faces. */
  EXPECT_EQ(output.face_offsets().size(), 106);

  /* Total area must match the domain area: 0.273197 - 0.038033 - 0.051810 ~= 0.18335. */
  EXPECT_NEAR(total_faces_area(output), 0.18335, 1e-4);

  /* Verify no vertex repeats in any face. */
  for (const int fi : output.face_offsets().index_range()) {
    const IndexRange face_r = output.face_offsets()[fi];
    EXPECT_GE(face_r.size(), 3);
    Set<int> unique_verts;
    for (const int i : face_r) {
      EXPECT_TRUE(unique_verts.add(output.face_vert_indices[i]));
    }
  }
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
  EXPECT_EQ(output.face_offsets().size(), 12);
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
  EXPECT_EQ(output.face_offsets().size(), 12);
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
  EXPECT_EQ(output.face_offsets().size(), 10);
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
  EXPECT_EQ(output.face_offsets().size(), 6);
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
  EXPECT_EQ(output.face_offsets().size(), 9);
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
  EXPECT_EQ(output.face_offsets().size(), 8);
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
  EXPECT_EQ(output.face_offsets().size(), 16);
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
/** \name Zero / Negative Offset Tests
 * \{ */

TEST(straight_skeleton, NegativeOrZeroOffset_NoOp)
{
  const Array<double2> verts = {
      double2(0.0, 0.0), double2(10.0, 0.0), double2(10.0, 10.0), double2(0.0, 10.0)};
  const Array<int> face_offsets_data = {0, 4};
  const Array<int> face_vert_indices = {0, 1, 2, 3};

  /* Negative offset: no-op. */
  {
    StraightSkeletonInput input = make_test_input();
    input.vert = verts.as_span();
    input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
    input.face_vert_indices = face_vert_indices.as_span();
    input.offset = -2.0;

    const StraightSkeletonOutput output = straight_skeleton_calc(input);
    EXPECT_TRUE(output.is_valid);
    EXPECT_EQ(output.face_offsets().size(), 0);
    EXPECT_EQ(output.vert.size(), 0);
  }

  /* Zero offset: no-op. */
  {
    StraightSkeletonInput input = make_test_input();
    input.vert = verts.as_span();
    input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
    input.face_vert_indices = face_vert_indices.as_span();
    input.offset = 0.0;

    const StraightSkeletonOutput output = straight_skeleton_calc(input);
    EXPECT_TRUE(output.is_valid);
    EXPECT_EQ(output.face_offsets().size(), 0);
    EXPECT_EQ(output.vert.size(), 0);
  }
}

/** \} */

TEST(straight_skeleton, PSLG_ArbitraryWindingHoles)
{
  const Array<double2> verts = {
      /* Outer square 0..3 (CCW) */
      double2(0.0, 0.0),
      double2(10.0, 0.0),
      double2(10.0, 10.0),
      double2(0.0, 10.0),
      /* Inner square 4..7 (deliberately CCW instead of CW!) */
      double2(3.0, 3.0),
      double2(7.0, 3.0),
      double2(7.0, 7.0),
      double2(3.0, 7.0),
  };

  const Array<int> face_offsets_data = {0, 4, 8};
  const Array<int> face_vert_indices = {0, 1, 2, 3, 4, 5, 6, 7};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Inside;
  input.offset = 1.0;
  input.label = "PSLG_ArbitraryWindingHoles";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_GT(output.face_offsets().size(), 0);
  EXPECT_GT(output.edge.size(), 0);
}

TEST(straight_skeleton, PSLG_MultiNestedHierarchy)
{
  const Array<double2> verts = {
      /* Inner island (6..14) - verts 0..3 */
      double2(6.0, 6.0),
      double2(14.0, 6.0),
      double2(14.0, 14.0),
      double2(6.0, 14.0),
      /* Outermost island (0..20) - verts 4..7 */
      double2(0.0, 0.0),
      double2(20.0, 0.0),
      double2(20.0, 20.0),
      double2(0.0, 20.0),
      /* Middle hole (3..17) - verts 8..11 */
      double2(3.0, 3.0),
      double2(17.0, 3.0),
      double2(17.0, 17.0),
      double2(3.0, 17.0),
  };

  const Array<int> face_offsets_data = {0, 4, 8, 12};
  const Array<int> face_vert_indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Inside;
  input.offset = 1.0;
  input.label = "PSLG_MultiNestedHierarchy";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_GT(output.face_offsets().size(), 0);
  EXPECT_GT(output.edge.size(), 0);
}

TEST(straight_skeleton, PSLG_WireEdges_Polyline)
{
  const Array<double2> verts = {
      double2(0.0, 0.0),
      double2(5.0, 5.0),
      double2(10.0, 0.0),
  };
  const Array<int2> edges = {
      int2(0, 1),
      int2(1, 2),
  };

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.edge = edges.as_span();
  input.offset = 1.0;
  input.label = "PSLG_WireEdges_Polyline";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_GT(output.face_offsets().size(), 0);
  EXPECT_GT(output.edge.size(), 0);
}

TEST(straight_skeleton, PSLG_WireEdges_InsideFace)
{
  const Array<double2> verts = {
      /* Square outer face */
      double2(0.0, 0.0),
      double2(20.0, 0.0),
      double2(20.0, 20.0),
      double2(0.0, 20.0),
      /* Interior wire edge */
      double2(7.0, 10.0),
      double2(13.0, 10.0),
  };
  const Array<int> face_offsets_data = {0, 4};
  const Array<int> face_vert_indices = {0, 1, 2, 3};
  const Array<int2> edges = {int2(4, 5)};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.edge = edges.as_span();
  input.direction = PropagationDirection::Inside;
  input.offset = 1.5;
  input.label = "PSLG_WireEdges_InsideFace";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_GT(output.face_offsets().size(), 0);
  EXPECT_GT(output.edge.size(), 0);
}

TEST(straight_skeleton, PSLG_Outside_WithBoundingBox)
{
  const Array<double2> verts = {
      double2(0.0, 0.0),
      double2(10.0, 0.0),
      double2(10.0, 10.0),
      double2(0.0, 10.0),
  };
  const Array<int> face_offsets_data = {0, 4};
  const Array<int> face_vert_indices = {0, 1, 2, 3};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Outside;
  input.offset = 2.0;
  input.bbox_expansion_pct = 50.0;
  input.label = "PSLG_Outside_WithBoundingBox";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_GT(output.face_offsets().size(), 0);
  EXPECT_GT(output.edge.size(), 0);
}

TEST(straight_skeleton, PSLG_Both_Directions)
{
  const Array<double2> verts = {
      double2(0.0, 0.0),
      double2(10.0, 0.0),
      double2(10.0, 10.0),
      double2(0.0, 10.0),
  };
  const Array<int> face_offsets_data = {0, 4};
  const Array<int> face_vert_indices = {0, 1, 2, 3};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Both;
  input.offset = 1.0;
  input.bbox_expansion_pct = 50.0;
  input.label = "PSLG_Both_Directions";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_FALSE(output.hit_max_events);
  EXPECT_GT(output.face_offsets().size(), 0);
  EXPECT_GT(output.edge.size(), 0);
}

TEST(straight_skeleton, PSLG_MultipleDisjointIslands)
{
  /* Two separate 10x10 squares separated by 20 units in X.
   * With Inside propagation, each island forms an independent straight skeleton
   * with 4 roof faces, total area 200.0, and no cross-island interaction. */
  const Array<double2> verts = {
      /* Square 1 */
      double2(0.0, 0.0),
      double2(10.0, 0.0),
      double2(10.0, 10.0),
      double2(0.0, 10.0),
      /* Square 2 */
      double2(30.0, 0.0),
      double2(40.0, 0.0),
      double2(40.0, 10.0),
      double2(30.0, 10.0),
  };
  const Array<int> face_offsets_data = {0, 4, 8};
  const Array<int> face_vert_indices = {0, 1, 2, 3, 4, 5, 6, 7};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Inside;
  input.offset = std::numeric_limits<double>::infinity();
  input.label = "PSLG_MultipleDisjointIslands";
  input.full_verification = true;

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_FALSE(output.hit_max_events);
  EXPECT_EQ(output.face_offsets().size(), 8);
  EXPECT_NEAR(total_faces_area(output), 200.0, 1e-4);

  /* Verify no edge spans between Square 1 (x <= 10) and Square 2 (x >= 30). */
  for (const int2 &e : output.edge) {
    const double2 &p1 = output.vert[e[0]];
    const double2 &p2 = output.vert[e[1]];
    const bool p1_sq1 = (p1.x <= 15.0);
    const bool p2_sq1 = (p2.x <= 15.0);
    EXPECT_EQ(p1_sq1, p2_sq1);
  }
}

TEST(straight_skeleton, PSLG_MultipleDisjointIslands_Both)
{
  /* Two separate 10x10 squares separated by 20 units in X with Both direction.
   * Tests inward/outward decoupling and deterministic component merging. */
  const Array<double2> verts = {
      /* Square 1 */
      double2(0.0, 0.0),
      double2(10.0, 0.0),
      double2(10.0, 10.0),
      double2(0.0, 10.0),
      /* Square 2 */
      double2(30.0, 0.0),
      double2(40.0, 0.0),
      double2(40.0, 10.0),
      double2(30.0, 10.0),
  };
  const Array<int> face_offsets_data = {0, 4, 8};
  const Array<int> face_vert_indices = {0, 1, 2, 3, 4, 5, 6, 7};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Both;
  input.offset = 1.0;
  input.bbox_expansion_pct = 20.0;
  input.label = "PSLG_MultipleDisjointIslands_Both";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_FALSE(output.hit_max_events);
  EXPECT_GT(output.face_offsets().size(), 8);
  EXPECT_GT(output.edge.size(), 0);

  /* Verify original input vertices 0..7 are preserved at the front of output.vert. */
  ASSERT_GE(output.vert.size(), 8);
  for (int i = 0; i < 8; i++) {
    EXPECT_DOUBLE_EQ(output.vert[i].x, verts[i].x);
    EXPECT_DOUBLE_EQ(output.vert[i].y, verts[i].y);
    EXPECT_DOUBLE_EQ(output.vert_time[i], 0.0);
  }
}

TEST(straight_skeleton, PSLG_SquareWithHole_Both)
{
  /* Square [0, 10] x [0, 10] with square hole [3, 7] x [3, 7].
   * In Both mode, three disjoint regions are roofed:
   * 1. The solid frame [0, 10]^2 \ [3, 7]^2 (area = 84.0)
   * 2. The interior of the hole [3, 7]^2 (area = 16.0)
   * 3. The exterior domain from [0, 10]^2 outward to the bounding box.
   * Total area inside the [0, 10]^2 boundary must equal 100.0 (no missing faces!). */
  const Array<double2> verts = {
      /* Outer square (CCW) */
      double2(0.0, 0.0),
      double2(10.0, 0.0),
      double2(10.0, 10.0),
      double2(0.0, 10.0),
      /* Hole (CW) */
      double2(3.0, 7.0),
      double2(7.0, 7.0),
      double2(7.0, 3.0),
      double2(3.0, 3.0),
  };
  const Array<int> face_offsets_data = {0, 4, 8};
  const Array<int> face_vert_indices = {0, 1, 2, 3, 4, 5, 6, 7};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Both;
  input.offset = std::numeric_limits<double>::infinity();
  input.bbox_expansion_pct = 20.0;
  input.label = "PSLG_SquareWithHole_Both";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_FALSE(output.hit_max_events);

  /* Calculate area of all faces whose vertices lie inside [0, 10] x [0, 10]. */
  const OffsetIndices<int> faces = output.face_offsets();
  double interior_area = 0.0;
  for (const int fi : faces.index_range()) {
    Vector<double2> poly;
    bool all_inside_square = true;
    for (const int idx : faces[fi]) {
      const double2 &p = output.vert[output.face_vert_indices[idx]];
      poly.append(p);
      if (p.x < -1e-4 || p.x > 10.0 + 1e-4 || p.y < -1e-4 || p.y > 10.0 + 1e-4) {
        all_inside_square = false;
      }
    }
    if (all_inside_square) {
      double poly_area = 0.0;
      for (int i = 0; i < poly.size(); i++) {
        const double2 &p1 = poly[i];
        const double2 &p2 = poly[(i + 1) % poly.size()];
        poly_area += math::cross(p1, p2);
      }
      interior_area += std::abs(0.5 * poly_area);
    }
  }

  /* Must cover exactly 100.0 (solid 84.0 + hole interior 16.0 = 100.0). */
  EXPECT_NEAR(interior_area, 100.0, 1e-3);
}

inline const double2 capital_G_verts[262] = {
    double2(17.440000534057617, 1.3440001010894775),
    double2(16.524002075195312, 1.3440001010894775),
    double2(16.524002075195312, 0.9920000433921814),
    double2(17.04800033569336, 0.9920000433921814),
    double2(17.04800033569336, 0.4320000112056732),
    double2(17.030363082885742, 0.4264112710952759),
    double2(17.012699127197266, 0.4208994209766388),
    double2(16.995006561279297, 0.41547107696533203),
    double2(16.97728157043457, 0.41013282537460327),
    double2(16.959522247314453, 0.4048912525177002),
    double2(16.94172477722168, 0.3997529447078705),
    double2(16.923885345458984, 0.3947244882583618),
    double2(16.906002044677734, 0.38981249928474426),
    double2(16.888071060180664, 0.3850235641002655),
    double2(16.87009048461914, 0.3803642690181732),
    double2(16.85205841064453, 0.3758412003517151),
    double2(16.833969116210938, 0.3714609444141388),
    double2(16.81582260131836, 0.3672301173210144),
    double2(16.79761505126953, 0.3631552755832672),
    double2(16.779342651367188, 0.3592430353164673),
    double2(16.761001586914062, 0.3554999828338623),
    double2(16.742591857910156, 0.35193273425102234),
    double2(16.72410774230957, 0.3485478460788727),
    double2(16.705547332763672, 0.3453519344329834),
    double2(16.686908721923828, 0.3423515558242798),
    double2(16.668188095092773, 0.3395533263683319),
    double2(16.649381637573242, 0.33696386218070984),
    double2(16.6304874420166, 0.33458971977233887),
    double2(16.611501693725586, 0.3324374854564667),
    double2(16.592422485351562, 0.33051377534866333),
    double2(16.5732479095459, 0.3288251757621765),
    double2(16.553972244262695, 0.3273782730102539),
    double2(16.534595489501953, 0.3261796534061432),
    double2(16.515111923217773, 0.32523593306541443),
    double2(16.495521545410156, 0.3245536684989929),
    double2(16.475818634033203, 0.32413947582244873),
    double2(16.45600128173828, 0.3240000009536743),
    double2(16.395822525024414, 0.32524633407592773),
    double2(16.336816787719727, 0.3289550840854645),
    double2(16.27903175354004, 0.3350808024406433),
    double2(16.222517013549805, 0.34357813000679016),
    double2(16.167320251464844, 0.3544016182422638),
    double2(16.11349105834961, 0.36750587821006775),
    double2(16.061077117919922, 0.3828454911708832),
    double2(16.0101261138916, 0.40037503838539124),
    double2(15.960686683654785, 0.42004913091659546),
    double2(15.91280746459961, 0.4418223202228546),
    double2(15.866537094116211, 0.46564921736717224),
    double2(15.82192325592041, 0.4914844334125519),
    double2(15.779014587402344, 0.5192825198173523),
    double2(15.737858772277832, 0.5489981174468994),
    double2(15.698505401611328, 0.580585777759552),
    double2(15.661002159118652, 0.6140000820159912),
    double2(15.625396728515625, 0.649195671081543),
    double2(15.5917387008667, 0.6861270666122437),
    double2(15.560075759887695, 0.7247489094734192),
    double2(15.530455589294434, 0.7650157809257507),
    double2(15.502927780151367, 0.8068822622299194),
    double2(15.477540016174316, 0.8503029346466064),
    double2(15.454340934753418, 0.8952323794364929),
    double2(15.433378219604492, 0.9416252374649048),
    double2(15.414700508117676, 0.9894360303878784),
    double2(15.398356437683105, 1.0386193990707397),
    double2(15.384394645690918, 1.08912992477417),
    double2(15.372862815856934, 1.14092218875885),
    double2(15.363809585571289, 1.1939507722854614),
    double2(15.357282638549805, 1.248170256614685),
    double2(15.353331565856934, 1.3035352230072021),
    double2(15.352001190185547, 1.3600000143051147),
    double2(15.353339195251465, 1.4150803089141846),
    double2(15.357312202453613, 1.4692832231521606),
    double2(15.363860130310059, 1.5225515365600586),
    double2(15.372922897338867, 1.5748281478881836),
    double2(15.384441375732422, 1.6260559558868408),
    double2(15.398354530334473, 1.676177740097046),
    double2(15.414603233337402, 1.7251365184783936),
    double2(15.433126449584961, 1.7728750705718994),
    double2(15.453864097595215, 1.8193362951278687),
    double2(15.476757049560547, 1.864462971687317),
    double2(15.501744270324707, 1.9081981182098389),
    double2(15.528766632080078, 1.9504845142364502),
    double2(15.55776309967041, 1.991265058517456),
    double2(15.588674545288086, 2.030482530593872),
    double2(15.621439933776855, 2.068079948425293),
    double2(15.656000137329102, 2.1040000915527344),
    double2(15.69229507446289, 2.138185977935791),
    double2(15.730263710021973, 2.1705803871154785),
    double2(15.76984691619873, 2.2011260986328125),
    double2(15.81098461151123, 2.2297658920288086),
    double2(15.853615760803223, 2.2564427852630615),
    double2(15.89768123626709, 2.281099796295166),
    double2(15.943120956420898, 2.3036797046661377),
    double2(15.989874839782715, 2.324125289916992),
    double2(16.03788185119629, 2.342379570007324),
    double2(16.08708381652832, 2.3583850860595703),
    double2(16.137418746948242, 2.3720850944519043),
    double2(16.188827514648438, 2.383422374725342),
    double2(16.24125099182129, 2.3923397064208984),
    double2(16.294626235961914, 2.39877986907959),
    double2(16.348896026611328, 2.4026858806610107),
    double2(16.404001235961914, 2.4040000438690186),
    double2(16.434762954711914, 2.403730869293213),
    double2(16.465543746948242, 2.402924060821533),
    double2(16.4963436126709, 2.401581048965454),
    double2(16.527156829833984, 2.39970326423645),
    double2(16.5579833984375, 2.397292375564575),
    double2(16.588817596435547, 2.3943498134613037),
    double2(16.619657516479492, 2.3908770084381104),
    double2(16.650501251220703, 2.3868753910064697),
    double2(16.681344985961914, 2.3823463916778564),
    double2(16.71218490600586, 2.377291440963745),
    double2(16.743019104003906, 2.3717122077941895),
    double2(16.773845672607422, 2.365609884262085),
    double2(16.804658889770508, 2.3589861392974854),
    double2(16.835458755493164, 2.3518424034118652),
    double2(16.866239547729492, 2.344180107116699),
    double2(16.897001266479492, 2.336000680923462),
    double2(16.927738189697266, 2.327305555343628),
    double2(16.958450317382812, 2.318096399307251),
    double2(16.989131927490234, 2.3083744049072266),
    double2(17.0197811126709, 2.2981412410736084),
    double2(17.050395965576172, 2.287398338317871),
    double2(17.08097267150879, 2.2761471271514893),
    double2(17.111507415771484, 2.2643890380859375),
    double2(17.141998291015625, 2.2521255016326904),
    double2(17.172443389892578, 2.2393581867218018),
    double2(17.202838897705078, 2.226088523864746),
    double2(17.23318099975586, 2.212317705154419),
    double2(17.26346778869629, 2.198047399520874),
    double2(17.2936954498291, 2.183279037475586),
    double2(17.323862075805664, 2.1680140495300293),
    double2(17.35396385192871, 2.152254104614258),
    double2(17.384000778198242, 2.136000156402588),
    double2(17.384000778198242, 2.5320000648498535),
    double2(17.344465255737305, 2.5485024452209473),
    double2(17.306068420410156, 2.5642685890197754),
    double2(17.268756866455078, 2.579312562942505),
    double2(17.23247718811035, 2.5936484336853027),
    double2(17.197172164916992, 2.607290029525757),
    double2(17.16278839111328, 2.620251178741455),
    double2(17.129270553588867, 2.6325457096099854),
    double2(17.0965633392334, 2.6441876888275146),
    double2(17.064611434936523, 2.655190944671631),
    double2(17.033361434936523, 2.665569543838501),
    double2(17.002756118774414, 2.675337314605713),
    double2(16.972742080688477, 2.6845080852508545),
    double2(16.94326400756836, 2.6930959224700928),
    double2(16.914268493652344, 2.7011146545410156),
    double2(16.885698318481445, 2.708578109741211),
    double2(16.857500076293945, 2.7155003547668457),
    double2(16.829618453979492, 2.721895217895508),
    double2(16.801998138427734, 2.7277767658233643),
    double2(16.77458381652832, 2.733158826828003),
    double2(16.74732208251953, 2.7380552291870117),
    double2(16.720155715942383, 2.7424798011779785),
    double2(16.693031311035156, 2.7464466094970703),
    double2(16.6658935546875, 2.749969720840454),
    double2(16.638689041137695, 2.7530629634857178),
    double2(16.611360549926758, 2.755740165710449),
    double2(16.58385467529297, 2.7580151557922363),
    double2(16.556116104125977, 2.759902000427246),
    double2(16.52808952331543, 2.7614145278930664),
    double2(16.499719619750977, 2.7625668048858643),
    double2(16.470951080322266, 2.7633726596832275),
    double2(16.441730499267578, 2.763845920562744),
    double2(16.41200065612793, 2.764000177383423),
    double2(16.3346004486084, 2.7622201442718506),
    double2(16.258451461791992, 2.7569310665130615),
    double2(16.183631896972656, 2.7482097148895264),
    double2(16.11022186279297, 2.736133098602295),
    double2(16.038299560546875, 2.720778226852417),
    double2(15.967944145202637, 2.7022218704223633),
    double2(15.899235725402832, 2.6805410385131836),
    double2(15.832252502441406, 2.6558127403259277),
    double2(15.767074584960938, 2.6281137466430664),
    double2(15.703780174255371, 2.5975208282470703),
    double2(15.642448425292969, 2.5641109943389893),
    double2(15.583158493041992, 2.527961254119873),
    double2(15.52599048614502, 2.4891483783721924),
    double2(15.471022605895996, 2.447749376296997),
    double2(15.418334007263184, 2.403841018676758),
    double2(15.368003845214844, 2.3575003147125244),
    double2(15.320110321044922, 2.3088040351867676),
    double2(15.274733543395996, 2.257829189300537),
    double2(15.231952667236328, 2.204652786254883),
    double2(15.19184684753418, 2.1493515968322754),
    double2(15.154495239257812, 2.0920026302337646),
    double2(15.119976043701172, 2.0326826572418213),
    double2(15.088369369506836, 1.9714686870574951),
    double2(15.05975341796875, 1.9084374904632568),
    double2(15.034208297729492, 1.8436660766601562),
    double2(15.011812210083008, 1.7772313356399536),
    double2(14.992644309997559, 1.7092101573944092),
    double2(14.976784706115723, 1.6396794319152832),
    double2(14.964311599731445, 1.5687161684036255),
    double2(14.955304145812988, 1.4963971376419067),
    double2(14.949841499328613, 1.4227993488311768),
    double2(14.94800090789795, 1.3480000495910645),
    double2(14.949793815612793, 1.2736440896987915),
    double2(14.955122947692871, 1.2006211280822754),
    double2(14.963916778564453, 1.1289985179901123),
    double2(14.976102828979492, 1.058843731880188),
    double2(14.991607666015625, 0.9902241230010986),
    double2(15.010359764099121, 0.9232070446014404),
    double2(15.032285690307617, 0.8578598499298096),
    double2(15.057313919067383, 0.794249951839447),
    double2(15.085371017456055, 0.7324447631835938),
    double2(15.116385459899902, 0.672511637210846),
    double2(15.150283813476562, 0.6145179867744446),
    double2(15.186993598937988, 0.5585311651229858),
    double2(15.22644329071045, 0.5046185255050659),
    double2(15.268559455871582, 0.45284751057624817),
    double2(15.31326961517334, 0.40328550338745117),
    double2(15.360501289367676, 0.3559998571872711),
    double2(15.41018295288086, 0.3110579550266266),
    double2(15.462241172790527, 0.26852717995643616),
    double2(15.516603469848633, 0.2284749299287796),
    double2(15.573197364807129, 0.1909685730934143),
    double2(15.631950378417969, 0.15607549250125885),
    double2(15.692790031433105, 0.12386307865381241),
    double2(15.755643844604492, 0.09439870715141296),
    double2(15.820439338684082, 0.06774976849555969),
    double2(15.887104034423828, 0.043983642011880875),
    double2(15.955565452575684, 0.02316771075129509),
    double2(16.0257511138916, 0.005369357764720917),
    double2(16.09758758544922, -0.009344033896923065),
    double2(16.171003341674805, -0.020905081182718277),
    double2(16.245925903320312, -0.02924640290439129),
    double2(16.322282791137695, -0.03430061414837837),
    double2(16.400001525878906, -0.036000002175569534),
    double2(16.43538475036621, -0.03581152483820915),
    double2(16.470304489135742, -0.03524218872189522),
    double2(16.50478744506836, -0.03428613394498825),
    double2(16.538862228393555, -0.03293750062584877),
    double2(16.572555541992188, -0.031190430745482445),
    double2(16.60589599609375, -0.029039062559604645),
    double2(16.6389102935791, -0.02647753804922104),
    double2(16.671627044677734, -0.023499999195337296),
    double2(16.70407485961914, -0.020100584253668785),
    double2(16.73628044128418, -0.016273435205221176),
    double2(16.768272399902344, -0.012012692168354988),
    double2(16.800079345703125, -0.007312495727092028),
    double2(16.831727981567383, -0.0021669864654541016),
    double2(16.863245010375977, 0.0034296950325369835),
    double2(16.89466094970703, 0.00948340818285942),
    double2(16.926000595092773, 0.016000011935830116),
    double2(16.957294464111328, 0.022985365241765976),
    double2(16.988569259643555, 0.03044532798230648),
    double2(17.019853591918945, 0.03838576003909111),
    double2(17.05117416381836, 0.0468125194311142),
    double2(17.08255958557129, 0.05573146790266037),
    double2(17.114036560058594, 0.06514846533536911),
    double2(17.145633697509766, 0.0750693678855896),
    double2(17.177377700805664, 0.08550003170967102),
    double2(17.209299087524414, 0.09644632041454315),
    double2(17.241422653198242, 0.10791409760713577),
    double2(17.273778915405273, 0.11990921944379807),
    double2(17.306394577026367, 0.1324375420808792),
    double2(17.339296340942383, 0.1455049216747284),
    double2(17.372512817382812, 0.15911723673343658),
    double2(17.40607261657715, 0.17328032851219177),
    double2(17.440000534057617, 0.18800000846385956),
};

inline const double2 lowercase_e_outer[194] = {
    double2(9.332000732421875, -3.4800000190734863),
    double2(9.312512397766113, -3.4925851821899414),
    double2(9.293044090270996, -3.50483775138855),
    double2(9.273592948913574, -3.5167534351348877),
    double2(9.254157066345215, -3.5283279418945312),
    double2(9.234732627868652, -3.5395567417144775),
    double2(9.215316772460938, -3.5504353046417236),
    double2(9.195906639099121, -3.560959577560425),
    double2(9.17650032043457, -3.571124792098999),
    double2(9.15709400177002, -3.5809268951416016),
    double2(9.137683868408203, -3.5903611183166504),
    double2(9.118268013000488, -3.599423408508301),
    double2(9.098843574523926, -3.60810923576355),
    double2(9.079407691955566, -3.6164140701293945),
    double2(9.059956550598145, -3.6243338584899902),
    double2(9.040488243103027, -3.631863832473755),
    double2(9.020999908447266, -3.6389999389648438),
    double2(9.001487731933594, -3.645737409591675),
    double2(8.981948852539062, -3.6520721912384033),
    double2(8.962381362915039, -3.6579997539520264),
    double2(8.942781448364258, -3.663515567779541),
    double2(8.92314624786377, -3.6686153411865234),
    double2(8.903472900390625, -3.67329478263855),
    double2(8.883758544921875, -3.677549362182617),
    double2(8.86400032043457, -3.6813747882843018),
    double2(8.844195365905762, -3.6847665309906006),
    double2(8.8243408203125, -3.68772029876709),
    double2(8.80443286895752, -3.6902318000793457),
    double2(8.784469604492188, -3.6922965049743652),
    double2(8.764447212219238, -3.6939101219177246),
    double2(8.744363784790039, -3.695068120956421),
    double2(8.724215507507324, -3.6957662105560303),
    double2(8.704000473022461, -3.696000099182129),
    double2(8.672161102294922, -3.6954615116119385),
    double2(8.641142845153809, -3.6938469409942627),
    double2(8.61094856262207, -3.6911585330963135),
    double2(8.581578254699707, -3.687398672103882),
    double2(8.553034782409668, -3.6825695037841797),
    double2(8.52531909942627, -3.676673173904419),
    double2(8.498432159423828, -3.6697118282318115),
    double2(8.472375869750977, -3.6616878509521484),
    double2(8.447151184082031, -3.6526031494140625),
    double2(8.422760009765625, -3.6424601078033447),
    double2(8.39920425415039, -3.631261110305786),
    double2(8.376484870910645, -3.6190080642700195),
    double2(8.35460376739502, -3.605703353881836),
    double2(8.333561897277832, -3.591348886489868),
    double2(8.313360214233398, -3.5759470462799072),
    double2(8.294001579284668, -3.559500217437744),
    double2(8.27548599243164, -3.5420103073120117),
    double2(8.257816314697266, -3.523479700088501),
    double2(8.240992546081543, -3.503910541534424),
    double2(8.225017547607422, -3.483304977416992),
    double2(8.209892272949219, -3.461665153503418),
    double2(8.19561767578125, -3.438993453979492),
    double2(8.182195663452148, -3.4152917861938477),
    double2(8.16962718963623, -3.3905625343322754),
    double2(8.157914161682129, -3.3648080825805664),
    double2(8.147058486938477, -3.3380303382873535),
    double2(8.13706111907959, -3.3102316856384277),
    double2(8.127923965454102, -3.281414270401001),
    double2(8.119647979736328, -3.251580238342285),
    double2(8.112235069274902, -3.220731735229492),
    double2(8.10568618774414, -3.188870906829834),
    double2(8.100001335144043, -3.1559998989105225),
    double2(9.364001274108887, -3.1559998989105225),
    double2(9.363114356994629, -3.094745635986328),
    double2(9.36047077178955, -3.035480499267578),
    double2(9.356094360351562, -2.9782016277313232),
    double2(9.350008964538574, -2.9229061603546143),
    double2(9.342239379882812, -2.869591236114502),
    double2(9.332809448242188, -2.818253755569458),
    double2(9.32174301147461, -2.768890857696533),
    double2(9.309064865112305, -2.7214996814727783),
    double2(9.294798851013184, -2.676077365875244),
    double2(9.278968811035156, -2.6326208114624023),
    double2(9.261598587036133, -2.5911271572113037),
    double2(9.24271297454834, -2.551593542098999),
    double2(9.222336769104004, -2.51401686668396),
    double2(9.200493812561035, -2.4783942699432373),
    double2(9.177207946777344, -2.444722890853882),
    double2(9.15250301361084, -2.4129996299743652),
    double2(9.12640380859375, -2.3832218647003174),
    double2(9.098934173583984, -2.35538649559021),
    double2(9.070117950439453, -2.3294904232025146),
    double2(9.039979934692383, -2.3055310249328613),
    double2(9.008543968200684, -2.2835052013397217),
    double2(8.975833892822266, -2.2634100914001465),
    double2(8.941874504089355, -2.2452425956726074),
    double2(8.906689643859863, -2.2289998531341553),
    double2(8.870304107666016, -2.214679002761841),
    double2(8.832741737365723, -2.202277183532715),
    double2(8.794026374816895, -2.191791296005249),
    double2(8.754181861877441, -2.183218479156494),
    double2(8.71323299407959, -2.176555871963501),
    double2(8.67120361328125, -2.1718006134033203),
    double2(8.628118515014648, -2.168949604034424),
    double2(8.584000587463379, -2.167999744415283),
    double2(8.537951469421387, -2.169071912765503),
    double2(8.492828369140625, -2.1722652912139893),
    double2(8.4486665725708, -2.177544593811035),
    double2(8.405500411987305, -2.1848747730255127),
    double2(8.36336612701416, -2.194220542907715),
    double2(8.322298049926758, -2.2055466175079346),
    double2(8.282331466674805, -2.218818187713623),
    double2(8.243501663208008, -2.233999729156494),
    double2(8.205843925476074, -2.251056432723999),
    double2(8.169392585754395, -2.2699530124664307),
    double2(8.134183883666992, -2.290654182434082),
    double2(8.100252151489258, -2.313124895095825),
    double2(8.067632675170898, -2.337329864501953),
    double2(8.036360740661621, -2.363234043121338),
    double2(8.006471633911133, -2.3908023834228516),
    double2(7.978001117706299, -2.419999599456787),
    double2(7.95098352432251, -2.4507906436920166),
    double2(7.925454139709473, -2.483140230178833),
    double2(7.9014482498168945, -2.5170133113861084),
    double2(7.879001140594482, -2.5523746013641357),
    double2(7.858147621154785, -2.589189052581787),
    double2(7.83892297744751, -2.6274213790893555),
    double2(7.821362495422363, -2.667036533355713),
    double2(7.8055009841918945, -2.7079994678497314),
    double2(7.791374206542969, -2.750274896621704),
    double2(7.779016971588135, -2.793827533721924),
    double2(7.768464088439941, -2.838622570037842),
    double2(7.759751319885254, -2.884624481201172),
    double2(7.752913475036621, -2.931798219680786),
    double2(7.74798583984375, -2.9801087379455566),
    double2(7.7450032234191895, -3.0295207500457764),
    double2(7.7440009117126465, -3.0799999237060547),
    double2(7.744913101196289, -3.1263816356658936),
    double2(7.747641563415527, -3.1724746227264404),
    double2(7.7521748542785645, -3.2182013988494873),
    double2(7.758501052856445, -3.263484477996826),
    double2(7.766608238220215, -3.308245897293091),
    double2(7.776484966278076, -3.3524081707000732),
    double2(7.788119792938232, -3.3958938121795654),
    double2(7.8015007972717285, -3.4386250972747803),
    double2(7.816616058349609, -3.4805243015289307),
    double2(7.833454132080078, -3.5215137004852295),
    double2(7.85200309753418, -3.5615158081054688),
    double2(7.872251033782959, -3.6004531383514404),
    double2(7.894186496734619, -3.6382477283477783),
    double2(7.917798042297363, -3.6748220920562744),
    double2(7.943073272705078, -3.7100987434387207),
    double2(7.970001220703125, -3.74399995803833),
    double2(7.998569488525391, -3.7764480113983154),
    double2(8.028766632080078, -3.8073651790618896),
    double2(8.06058120727539, -3.8366739749908447),
    double2(8.094000816345215, -3.8642966747283936),
    double2(8.12901496887207, -3.890155792236328),
    double2(8.165610313415527, -3.9141736030578613),
    double2(8.203776359558105, -3.936272382736206),
    double2(8.243500709533691, -3.9563746452331543),
    double2(8.284771919250488, -3.974402666091919),
    double2(8.3275785446167, -3.990278959274292),
    double2(8.371908187866211, -4.003925800323486),
    double2(8.417750358581543, -4.015265464782715),
    double2(8.465091705322266, -4.0242204666137695),
    double2(8.513921737670898, -4.030713081359863),
    double2(8.564228057861328, -4.034665584564209),
    double2(8.61600112915039, -4.035999774932861),
    double2(8.64418888092041, -4.035800457000732),
    double2(8.671767234802246, -4.035201072692871),
    double2(8.698759078979492, -4.034200191497803),
    double2(8.725189208984375, -4.032796859741211),
    double2(8.751079559326172, -4.030989170074463),
    double2(8.77645492553711, -4.028775691986084),
    double2(8.801338195800781, -4.0261549949646),
    double2(8.825752258300781, -4.023125648498535),
    double2(8.84972095489502, -4.019685745239258),
    double2(8.873268127441406, -4.015834331512451),
    double2(8.896416664123535, -4.011569976806641),
    double2(8.919190406799316, -4.006891250610352),
    double2(8.941612243652344, -4.001796245574951),
    double2(8.963706016540527, -3.996283769607544),
    double2(8.985494613647461, -3.9903523921966553),
    double2(9.007002830505371, -3.9840006828308105),
    double2(9.028252601623535, -3.977226972579956),
    double2(9.049267768859863, -3.9700300693511963),
    double2(9.070072174072266, -3.9624083042144775),
    double2(9.090689659118652, -3.954360246658325),
    double2(9.111143112182617, -3.9458844661712646),
    double2(9.131455421447754, -3.9369795322418213),
    double2(9.151650428771973, -3.9276437759399414),
    double2(9.171751976013184, -3.9178760051727295),
    double2(9.19178295135498, -3.907674551010132),
    double2(9.211767196655273, -3.897037982940674),
    double2(9.231728553771973, -3.885964870452881),
    double2(9.251689910888672, -3.8744537830352783),
    double2(9.271674156188965, -3.8625032901763916),
    double2(9.291705131530762, -3.850111961364746),
    double2(9.311806678771973, -3.837278127670288),
    double2(9.332000732421875, -3.8239998817443848),
};

inline const double2 lowercase_e_inner[65] = {
    double2(8.108000755310059, -2.9200000762939453),
    double2(8.11284065246582, -2.8937394618988037),
    double2(8.118358612060547, -2.8682119846343994),
    double2(8.124553680419922, -2.8434243202209473),
    double2(8.131423950195312, -2.819382905960083),
    double2(8.138967514038086, -2.7960944175720215),
    double2(8.147183418273926, -2.7735655307769775),
    double2(8.156070709228516, -2.751802682876587),
    double2(8.165627479553223, -2.7308125495910645),
    double2(8.175851821899414, -2.710601806640625),
    double2(8.186742782592773, -2.6911768913269043),
    double2(8.198298454284668, -2.672544479370117),
    double2(8.210517883300781, -2.6547110080718994),
    double2(8.22339916229248, -2.637683391571045),
    double2(8.23694133758545, -2.6214678287506104),
    double2(8.251142501831055, -2.6060712337493896),
    double2(8.26600170135498, -2.5915000438690186),
    double2(8.281517028808594, -2.577760934829712),
    double2(8.297687530517578, -2.5648603439331055),
    double2(8.3145112991333, -2.552804946899414),
    double2(8.331986427307129, -2.5416014194488525),
    double2(8.350111961364746, -2.5312561988830566),
    double2(8.368886947631836, -2.5217761993408203),
    double2(8.388309478759766, -2.5131676197052),
    double2(8.408377647399902, -2.5054373741149902),
    double2(8.42909049987793, -2.498591899871826),
    double2(8.450446128845215, -2.4926376342773438),
    double2(8.472443580627441, -2.487581253051758),
    double2(8.495080947875977, -2.483429431915283),
    double2(8.518357276916504, -2.4801888465881348),
    double2(8.54227066040039, -2.4778659343719482),
    double2(8.56682014465332, -2.4764673709869385),
    double2(8.592000961303711, -2.4759998321533203),
    double2(8.61534309387207, -2.47650146484375),
    double2(8.638113975524902, -2.4779977798461914),
    double2(8.66030502319336, -2.480475664138794),
    double2(8.681906700134277, -2.483921766281128),
    double2(8.702911376953125, -2.4883229732513428),
    double2(8.723309516906738, -2.493665933609009),
    double2(8.74309253692627, -2.4999377727508545),
    double2(8.762250900268555, -2.50712513923645),
    double2(8.780776977539062, -2.515214681625366),
    double2(8.798661231994629, -2.524193525314331),
    double2(8.815895080566406, -2.534048318862915),
    double2(8.832469940185547, -2.5447659492492676),
    double2(8.848376274108887, -2.556333065032959),
    double2(8.863606452941895, -2.5687365531921387),
    double2(8.878150939941406, -2.581963300704956),
    double2(8.892001152038574, -2.5960001945495605),
    double2(8.90514850616455, -2.6108338832855225),
    double2(8.917583465576172, -2.626451253890991),
    double2(8.92929744720459, -2.642839193344116),
    double2(8.940281867980957, -2.6599843502044678),
    double2(8.950528144836426, -2.6778738498687744),
    double2(8.960027694702148, -2.6964941024780273),
    double2(8.968770980834961, -2.715832233428955),
    double2(8.976750373840332, -2.735874891281128),
    double2(8.983956336975098, -2.7566089630126953),
    double2(8.990379333496094, -2.7780213356018066),
    double2(8.996011734008789, -2.8000986576080322),
    double2(9.00084400177002, -2.8228278160095215),
    double2(9.004867553710938, -2.846195697784424),
    double2(9.008073806762695, -2.8701891899108887),
    double2(9.010454177856445, -2.8947949409484863),
    double2(9.012001037597656, -2.9200000762939453),
};

inline const double2 capital_C_verts[258] = {
    double2(2.624000072479248, 2.5400002002716064),
    double2(2.5893802642822266, 2.554368019104004),
    double2(2.55526065826416, 2.5682239532470703),
    double2(2.5216259956359863, 2.581571578979492),
    double2(2.4884607791900635, 2.594414472579956),
    double2(2.455749750137329, 2.6067564487457275),
    double2(2.4234774112701416, 2.618601083755493),
    double2(2.3916285037994385, 2.6299519538879395),
    double2(2.360187530517578, 2.640812873840332),
    double2(2.329138994216919, 2.6511874198913574),
    double2(2.2984676361083984, 2.6610794067382812),
    double2(2.268158197402954, 2.67049241065979),
    double2(2.2381951808929443, 2.6794300079345703),
    double2(2.2085633277893066, 2.6878960132598877),
    double2(2.1792471408843994, 2.6958940029144287),
    double2(2.150231122970581, 2.70342755317688),
    double2(2.121500015258789, 2.710500478744507),
    double2(2.093038558959961, 2.717116355895996),
    double2(2.064831256866455, 2.723278760910034),
    double2(2.03686261177063, 2.7289915084838867),
    double2(2.009117364883423, 2.7342581748962402),
    double2(1.981580138206482, 2.7390825748443604),
    double2(1.954235553741455, 2.7434682846069336),
    double2(1.9270682334899902, 2.7474188804626465),
    double2(1.9000626802444458, 2.7509379386901855),
    double2(1.8732036352157593, 2.7540292739868164),
    double2(1.8464757204055786, 2.7566967010498047),
    double2(1.8198635578155518, 2.758943557739258),
    double2(1.7933517694473267, 2.7607736587524414),
    double2(1.7669248580932617, 2.762190818786621),
    double2(1.7405675649642944, 2.7631986141204834),
    double2(1.7142645120620728, 2.763800621032715),
    double2(1.6880000829696655, 2.764000177383423),
    double2(1.6073400974273682, 2.762230634689331),
    double2(1.528157353401184, 2.7569689750671387),
    double2(1.4505228996276855, 2.7482852935791016),
    double2(1.3745079040527344, 2.736250162124634),
    double2(1.3001832962036133, 2.720933675765991),
    double2(1.2276201248168945, 2.702406406402588),
    double2(1.15688955783844, 2.6807384490966797),
    double2(1.0880625247955322, 2.6560001373291016),
    double2(1.0212100744247437, 2.6282618045806885),
    double2(0.9564033150672913, 2.5975937843322754),
    double2(0.8937132358551025, 2.5640664100646973),
    double2(0.8332109451293945, 2.527750015258789),
    double2(0.7749674320220947, 2.4887149333953857),
    double2(0.7190537452697754, 2.447031259536743),
    double2(0.6655409336090088, 2.4027695655822754),
    double2(0.6145000457763672, 2.3559999465942383),
    double2(0.5660021305084229, 2.306792974472046),
    double2(0.520118236541748, 2.255218744277954),
    double2(0.47691938281059265, 2.201347589492798),
    double2(0.4364766478538513, 2.145249843597412),
    double2(0.3988610506057739, 2.086995840072632),
    double2(0.36414363980293274, 2.026655912399292),
    double2(0.3323954939842224, 1.9643003940582275),
    double2(0.3036876320838928, 1.8999996185302734),
    double2(0.27809107303619385, 1.833823800086975),
    double2(0.25567689538002014, 1.765843391418457),
    double2(0.23651614785194397, 1.6961286067962646),
    double2(0.2206798493862152, 1.6247496604919434),
    double2(0.20823906362056732, 1.5517770051956177),
    double2(0.19926483929157257, 1.4772809743881226),
    double2(0.19382821023464203, 1.4013317823410034),
    double2(0.19200000166893005, 1.3240001201629639),
    double2(0.19331641495227814, 1.2697728872299194),
    double2(0.19728125631809235, 1.2144639492034912),
    double2(0.2039179801940918, 1.158257007598877),
    double2(0.21325001120567322, 1.1013360023498535),
    double2(0.22530078887939453, 1.0438847541809082),
    double2(0.24009375274181366, 0.9860870242118835),
    double2(0.2576523423194885, 0.9281266927719116),
    double2(0.27799999713897705, 0.8701875805854797),
    double2(0.30116015672683716, 0.81245356798172),
    double2(0.3271562457084656, 0.7551084756851196),
    double2(0.3560117185115814, 0.698336124420166),
    double2(0.3877499997615814, 0.6423203945159912),
    double2(0.4223945140838623, 0.5872450470924377),
    double2(0.4599687159061432, 0.5332939624786377),
    double2(0.5004960298538208, 0.48065102100372314),
    double2(0.5439999103546143, 0.429500013589859),
    double2(0.5905038118362427, 0.380024790763855),
    double2(0.6400311589241028, 0.33240920305252075),
    double2(0.6926053762435913, 0.28683704137802124),
    double2(0.748249888420105, 0.24349218606948853),
    double2(0.8069881796836853, 0.2025584727525711),
    double2(0.8688436150550842, 0.1642197221517563),
    double2(0.9338396787643433, 0.12865978479385376),
    double2(1.001999855041504, 0.09606249630451202),
    double2(1.0733474493026733, 0.06661169230937958),
    double2(1.1479060649871826, 0.04049120843410492),
    double2(1.2256990671157837, 0.017884884029626846),
    double2(1.3067498207092285, -0.0010234415531158447),
    double2(1.3910818099975586, -0.016049930825829506),
    double2(1.4787185192108154, -0.02701074630022049),
    double2(1.569683313369751, -0.03372205048799515),
    double2(1.6640000343322754, -0.036000002175569534),
    double2(1.7037299871444702, -0.03570886328816414),
    double2(1.7426836490631104, -0.03484277427196503),
    double2(1.780881404876709, -0.03341272100806236),
    double2(1.8183437585830688, -0.03142968937754631),
    double2(1.8550913333892822, -0.028904665261507034),
    double2(1.8911445140838623, -0.0258486345410347),
    double2(1.9265239238739014, -0.022272584959864616),
    double2(1.9612500667572021, -0.018187502399086952),
    double2(1.9953433275222778, -0.013604373671114445),
    double2(2.0288243293762207, -0.008534184657037258),
    double2(2.061713457107544, -0.002987921703606844),
    double2(2.09403133392334, 0.0030234288424253464),
    double2(2.125798463821411, 0.009488880634307861),
    double2(2.1570353507995605, 0.016397448256611824),
    double2(2.187762498855591, 0.02373814396560192),
    double2(2.2180004119873047, 0.03149998188018799),
    double2(2.247769355773926, 0.03967197611927986),
    double2(2.277090072631836, 0.048243142664432526),
    double2(2.305983066558838, 0.05720249190926552),
    double2(2.3344688415527344, 0.06653903424739838),
    double2(2.362567901611328, 0.07624179124832153),
    double2(2.390300750732422, 0.08629976958036423),
    double2(2.4176878929138184, 0.09670198708772659),
    double2(2.4447498321533203, 0.10743746161460876),
    double2(2.4715070724487305, 0.11849519610404968),
    double2(2.4979803562164307, 0.12986421585083008),
    double2(2.5241899490356445, 0.1415335237979889),
    double2(2.550156354904175, 0.15349213778972626),
    double2(2.575900077819824, 0.1657290756702423),
    double2(2.6014416217803955, 0.178233340382576),
    double2(2.6268014907836914, 0.19099396467208862),
    double2(2.6520001888275146, 0.20400001108646393),
    double2(2.6520001888275146, 0.6320000290870667),
    double2(2.623044967651367, 0.6135200262069702),
    double2(2.59393572807312, 0.5955820083618164),
    double2(2.564682960510254, 0.578188955783844),
    double2(2.53529691696167, 0.5613437294960022),
    double2(2.5057878494262695, 0.5450493097305298),
    double2(2.476166009902954, 0.5293086171150208),
    double2(2.446441650390625, 0.5141245126724243),
    double2(2.4166250228881836, 0.49950000643730164),
    double2(2.3867263793945312, 0.4854379892349243),
    double2(2.3567559719085693, 0.47194141149520874),
    double2(2.326724052429199, 0.4590131938457489),
    double2(2.2966408729553223, 0.4466562569141388),
    double2(2.26651668548584, 0.4348735511302948),
    double2(2.2363617420196533, 0.4236679971218109),
    double2(2.206186294555664, 0.41304251551628113),
    double2(2.1760005950927734, 0.40300002694129944),
    double2(2.145814895629883, 0.3935434818267822),
    double2(2.1156394481658936, 0.3846758008003235),
    double2(2.085484504699707, 0.3763999342918396),
    double2(2.0553603172302246, 0.36871880292892456),
    double2(2.0252771377563477, 0.36163532733917236),
    double2(1.995245099067688, 0.355152428150177),
    double2(1.965274691581726, 0.34927305579185486),
    double2(1.9353760480880737, 0.34400010108947754),
    double2(1.9055594205856323, 0.3393365442752838),
    double2(1.8758350610733032, 0.3352852761745453),
    double2(1.8462132215499878, 0.33184924721717834),
    double2(1.8167041540145874, 0.329031378030777),
    double2(1.7873181104660034, 0.32683461904525757),
    double2(1.7580654621124268, 0.3252618908882141),
    double2(1.7289563417434692, 0.3243161141872406),
    double2(1.7000000476837158, 0.3240000009536743),
    double2(1.6391167640686035, 0.325234979391098),
    double2(1.579496145248413, 0.3289111256599426),
    double2(1.5211821794509888, 0.33498522639274597),
    double2(1.4642188549041748, 0.3434140682220459),
    double2(1.4086500406265259, 0.35415443778038025),
    double2(1.3545197248458862, 0.3671630918979645),
    double2(1.3018717765808105, 0.3823968470096588),
    double2(1.250750184059143, 0.3998124897480011),
    double2(1.201198935508728, 0.4193668067455292),
    double2(1.1532618999481201, 0.4410165846347809),
    double2(1.1069830656051636, 0.46471861004829407),
    double2(1.0624064207077026, 0.49042966961860657),
    double2(1.019575834274292, 0.5181065797805786),
    double2(0.9785352945327759, 0.5477060675621033),
    double2(0.9393287301063538, 0.5791849493980408),
    double2(0.9020001292228699, 0.612500011920929),
    double2(0.8665933609008789, 0.6476080417633057),
    double2(0.8331524133682251, 0.6844658255577087),
    double2(0.8017212748527527, 0.723030149936676),
    double2(0.7723438143730164, 0.7632578015327454),
    double2(0.7450640201568604, 0.8051055669784546),
    double2(0.7199258208274841, 0.8485302329063416),
    double2(0.6969731450080872, 0.8934885859489441),
    double2(0.6762499809265137, 0.9399374127388),
    double2(0.6578002572059631, 0.9878334999084473),
    double2(0.641667902469635, 1.0371336936950684),
    double2(0.6278969049453735, 1.0877946615219116),
    double2(0.6165311932563782, 1.1397732496261597),
    double2(0.6076146960258484, 1.1930263042449951),
    double2(0.6011913418769836, 1.247510552406311),
    double2(0.5973051190376282, 1.30318284034729),
    double2(0.5960000157356262, 1.3600000143051147),
    double2(0.5973501205444336, 1.415784239768982),
    double2(0.6013633012771606, 1.4706016778945923),
    double2(0.6079838871955872, 1.5243995189666748),
    double2(0.6171562671661377, 1.577125072479248),
    double2(0.6288247108459473, 1.6287256479263306),
    double2(0.6429336071014404, 1.679148554801941),
    double2(0.659427285194397, 1.728340983390808),
    double2(0.6782500147819519, 1.7762501239776611),
    double2(0.6993461847305298, 1.822823405265808),
    double2(0.7226601839065552, 1.868008017539978),
    double2(0.7481362819671631, 1.9117511510849),
    double2(0.7757188081741333, 1.9540002346038818),
    double2(0.8053520917892456, 1.9947023391723633),
    double2(0.8369805216789246, 2.0338048934936523),
    double2(0.87054842710495, 2.0712552070617676),
    double2(0.9060000777244568, 2.1070003509521484),
    double2(0.9432798624038696, 2.1409876346588135),
    double2(0.9823321104049683, 2.1731643676757812),
    double2(1.0231012105941772, 2.2034778594970703),
    double2(1.0655313730239868, 2.231875419616699),
    double2(1.1095670461654663, 2.2583041191101074),
    double2(1.1551525592803955, 2.2827112674713135),
    double2(1.2022321224212646, 2.305044174194336),
    double2(1.250750184059143, 2.3252501487731934),
    double2(1.3006510734558105, 2.3432765007019043),
    double2(1.3518791198730469, 2.3590705394744873),
    double2(1.4043786525726318, 2.372579336166382),
    double2(1.4580940008163452, 2.3837502002716064),
    double2(1.5129694938659668, 2.3925304412841797),
    double2(1.5689494609832764, 2.39886736869812),
    double2(1.6259783506393433, 2.4027082920074463),
    double2(1.6840001344680786, 2.4040000438690186),
    double2(1.7135454416275024, 2.4037415981292725),
    double2(1.7429405450820923, 2.402963876724243),
    double2(1.7721993923187256, 2.401663303375244),
    double2(1.8013359308242798, 2.3998360633850098),
    double2(1.8303641080856323, 2.3974785804748535),
    double2(1.859297752380371, 2.3945870399475098),
    double2(1.888150930404663, 2.391157865524292),
    double2(1.9169374704360962, 2.3871874809265137),
    double2(1.9456712007522583, 2.3826723098754883),
    double2(1.9743661880493164, 2.37760853767395),
    double2(2.0030362606048584, 2.371992349624634),
    double2(2.0316953659057617, 2.3658204078674316),
    double2(2.0603573322296143, 2.359088897705078),
    double2(2.089036226272583, 2.3517940044403076),
    double2(2.117745876312256, 2.3439323902130127),
    double2(2.1465001106262207, 2.3355002403259277),
    double2(2.1753129959106445, 2.326493740081787),
    double2(2.2041983604431152, 2.3169093132019043),
    double2(2.2331700325012207, 2.3067433834075928),
    double2(2.262242078781128, 2.295992374420166),
    double2(2.291428565979004, 2.2846524715423584),
    double2(2.3207430839538574, 2.2727200984954834),
    double2(2.3501996994018555, 2.2601914405822754),
    double2(2.379812240600586, 2.247062921524048),
    double2(2.409594774246216, 2.2333309650421143),
    double2(2.439561128616333, 2.218991756439209),
    double2(2.4697253704071045, 2.2040417194366455),
    double2(2.500101089477539, 2.188477039337158),
    double2(2.530702590942383, 2.1722943782806396),
    double2(2.5615434646606445, 2.155489921569824),
    double2(2.592637777328491, 2.1380598545074463),
    double2(2.624000072479248, 2.120000123977661),
};

/**
 * Font glyph lowercase 'e': outer boundary (194 vertices) + inner hole (65 vertices).
 * Tests complete coverage without dropped faces / gaps.
 */
TEST(straight_skeleton, FontGlyph_LowercaseE)
{
  Vector<double2> verts;
  for (const double2 &p : lowercase_e_outer) {
    verts.append(p);
  }
  for (const double2 &p : lowercase_e_inner) {
    verts.append(p);
  }

  const Array<int> face_offsets_data = {0, 194, 259};
  Array<int> face_vert_indices(259);
  for (int i = 0; i < 259; i++) {
    face_vert_indices[i] = i;
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Inside;
  input.offset = std::numeric_limits<double>::infinity();
  input.label = "FontGlyph_LowercaseE";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Verify 2-manifold edge topology: each interior edge is shared by at most 2 faces. */
  std::map<std::pair<int, int>, int> edge_face_count;
  for (const int fi : output.face_offsets().index_range()) {
    const IndexRange r = output.face_offsets()[fi];
    const int n = int(r.size());
    for (const int i : IndexRange(n)) {
      int u = output.face_vert_indices[r[i]];
      int v = output.face_vert_indices[r[(i + 1) % n]];
      if (u > v) {
        std::swap(u, v);
      }
      edge_face_count[{u, v}]++;
    }
  }
  for (const auto &[edge, count] : edge_face_count) {
    EXPECT_LE(count, 2) << "Overloaded edge (" << edge.first << ", " << edge.second
                        << ") shared by " << count << " faces";
  }

  EXPECT_TRUE(output.is_valid);
  EXPECT_FALSE(output.hit_max_events);
}

/**
 * Font glyph capital 'G': 262 vertices.
 * Tests that no roof faces or skeleton edges extend outside the input boundary.
 */
TEST(straight_skeleton, FontGlyph_CapitalG)
{
  Vector<double2> verts;
  for (const double2 &p : capital_G_verts) {
    verts.append(p);
  }

  const Array<int> face_offsets_data = {0, 262};
  Array<int> face_vert_indices(262);
  for (int i = 0; i < 262; i++) {
    face_vert_indices[i] = i;
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Inside;
  input.offset = std::numeric_limits<double>::infinity();
  input.label = "FontGlyph_CapitalG";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_TRUE(output.is_valid);
  EXPECT_FALSE(output.hit_max_events);
  std::cout << "Capital G: output faces = " << output.face_offsets().size()
            << ", total area = " << total_faces_area(output) << std::endl;
}

/**
 * Font glyph capital 'C': 258 vertices.
 */
TEST(straight_skeleton, FontGlyph_CapitalC)
{
  Vector<double2> verts;
  for (const double2 &p : capital_C_verts) {
    verts.append(p);
  }

  const Array<int> face_offsets_data = {0, 258};
  Array<int> face_vert_indices(258);
  for (int i = 0; i < 258; i++) {
    face_vert_indices[i] = i;
  }

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Inside;
  input.offset = std::numeric_limits<double>::infinity();
  input.label = "FontGlyph_CapitalC";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Verify 2-manifold edge topology: each interior edge is shared by at most 2 faces. */
  std::map<std::pair<int, int>, int> edge_face_count;
  for (const int fi : output.face_offsets().index_range()) {
    const IndexRange r = output.face_offsets()[fi];
    const int n = int(r.size());
    for (const int i : IndexRange(n)) {
      int u = output.face_vert_indices[r[i]];
      int v = output.face_vert_indices[r[(i + 1) % n]];
      if (u > v) {
        std::swap(u, v);
      }
      edge_face_count[{u, v}]++;
    }
  }
  for (const auto &[edge, count] : edge_face_count) {
    if (count > 2) {
      std::cout << "Overloaded edge (" << edge.first << ", " << edge.second
                << ") shared by faces: ";
      for (const int fi : output.face_offsets().index_range()) {
        const IndexRange r = output.face_offsets()[fi];
        const int n = int(r.size());
        for (const int i : IndexRange(n)) {
          int u = output.face_vert_indices[r[i]];
          int v = output.face_vert_indices[r[(i + 1) % n]];
          if ((u == edge.first && v == edge.second) || (v == edge.first && u == edge.second)) {
            std::cout << fi << " (dir: " << u << "->" << v << ", orig=" << output.face_orig[fi]
                      << ") ";
            break;
          }
        }
      }
      std::cout << std::endl;
      for (const int fi : output.face_offsets().index_range()) {
        const IndexRange r = output.face_offsets()[fi];
        const int n = int(r.size());
        for (const int i : IndexRange(n)) {
          int u = output.face_vert_indices[r[i]];
          int v = output.face_vert_indices[r[(i + 1) % n]];
          if ((u == edge.first && v == edge.second) || (v == edge.first && u == edge.second)) {
            std::cout << "  face " << fi << " verts: ";
            for (int vi : r)
              std::cout << output.face_vert_indices[vi] << " ";
            std::cout << std::endl;
            break;
          }
        }
      }
    }
    EXPECT_LE(count, 2) << "Overloaded edge (" << edge.first << ", " << edge.second
                        << ") shared by " << count << " faces";
  }

  EXPECT_TRUE(output.is_valid);
  EXPECT_FALSE(output.hit_max_events);
  std::cout << "Capital C: output faces = " << output.face_offsets().size()
            << ", total area = " << total_faces_area(output) << std::endl;
}

/**
 * Font glyph capital 'A'.
 */
TEST(straight_skeleton, FontGlyph_CapitalA)
{
  const Vector<double2> verts = {
      double2(1.4480, 2.7440),
      double2(1.1720, 2.7440),
      double2(0.0000, 0.0000),
      double2(0.4000, 0.0000),
      double2(0.7440, 0.8040),
      double2(1.9040, 0.8040),
      double2(2.2680, 0.0000),
      double2(2.6680, 0.0000),
      double2(1.3040, 2.1440),
      double2(1.7440, 1.1560),
      double2(0.8960, 1.1560),
  };

  const Array<int> face_offsets_data = {0, 8, 11};
  const Array<int> face_vert_indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  StraightSkeletonInput input = make_test_input();
  input.vert = verts.as_span();
  input.face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
  input.face_vert_indices = face_vert_indices.as_span();
  input.direction = PropagationDirection::Inside;
  input.offset = std::numeric_limits<double>::infinity();
  input.label = "FontGlyph_CapitalA";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
}

/**
 * Font glyph circular dot (from Text-ss comp 94).
 */
TEST(straight_skeleton, FontDot_Comp94)
{
  const char *spec = R"(128 1
28.0955562592 -7.5572447777
28.0835819244 -7.5587782860
28.0717945099 -7.5608983040
28.0602092743 -7.5635895729
28.0488414764 -7.5668368340
28.0377063751 -7.5706243515
28.0268192291 -7.5749373436
28.0161952972 -7.5797600746
28.0058517456 -7.5850772858
27.9958019257 -7.5908732414
27.9860610962 -7.5971326828
27.9766464233 -7.6038403511
27.9675712585 -7.6109809875
27.9588527679 -7.6185393333
27.9505062103 -7.6264996529
27.9425468445 -7.6348466873
27.9349880219 -7.6435651779
27.9278469086 -7.6526393890
27.9211387634 -7.6620545387
27.9148788452 -7.6717948914
27.9090824127 -7.6818447113
27.9037647247 -7.6921892166
27.8989429474 -7.7028126717
27.8946304321 -7.7136998177
27.8908424377 -7.7248353958
27.8875961304 -7.7362036705
27.8849048615 -7.7477893829
27.8827857971 -7.7595772743
27.8812522888 -7.7715520859
27.8803215027 -7.7836980820
27.8800048828 -7.7960000038
27.8803195953 -7.8083019257
27.8812503815 -7.8204479218
27.8827838898 -7.8324227333
27.8849029541 -7.8442106247
27.8875942230 -7.8557963371
27.8908405304 -7.8671646118
27.8946285248 -7.8783001900
27.8989410400 -7.8891873360
27.9037628174 -7.8998107910
27.9090805054 -7.9101552963
27.9148769379 -7.9202051163
27.9211368561 -7.9299454689
27.9278450012 -7.9393606186
27.9349861145 -7.9484348297
27.9425449371 -7.9571533203
27.9505043030 -7.9655003548
27.9588508606 -7.9734606743
27.9675693512 -7.9810190201
27.9766445160 -7.9881596565
27.9860591888 -7.9948678017
27.9958000183 -8.0011272430
28.0058498383 -8.0069236755
28.0161933899 -8.0122404099
28.0268173218 -8.0170631409
28.0377044678 -8.0213756561
28.0488395691 -8.0251636505
28.0602073669 -8.0284109116
28.0717926025 -8.0311021805
28.0835800171 -8.0332221985
28.0955543518 -8.0347557068
28.1077003479 -8.0356874466
28.1200046539 -8.0360002518
28.1323070526 -8.0356864929
28.1444530487 -8.0347547531
28.1564273834 -8.0332212448
28.1682147980 -8.0311012268
28.1798000336 -8.0284099579
28.1911678314 -8.0251626968
28.2023029327 -8.0213747025
28.2131900787 -8.0170621872
28.2238140106 -8.0122394562
28.2341575623 -8.0069227219
28.2442073822 -8.0011262894
28.2539482117 -7.9948668480
28.2633628845 -7.9881591797
28.2724380493 -7.9810185432
28.2811565399 -7.9734601974
28.2895030975 -7.9654998779
28.2974624634 -7.9571528435
28.3050212860 -7.9484343529
28.3121623993 -7.9393601418
28.3188705444 -7.9299449921
28.3251304626 -7.9202046394
28.3309268951 -7.9101548195
28.3362445831 -7.8998103142
28.3410663605 -7.8891868591
28.3453788757 -7.8782997131
28.3491668701 -7.8671641350
28.3524131775 -7.8557958603
28.3551044464 -7.8442101479
28.3572235107 -7.8324222565
28.3587570190 -7.8204474449
28.3596878052 -7.8083014488
28.3600044250 -7.7960000038
28.3596897125 -7.7836980820
28.3587589264 -7.7715520859
28.3572254181 -7.7595772743
28.3551063538 -7.7477893829
28.3524150848 -7.7362036705
28.3491687775 -7.7248353958
28.3453807831 -7.7136998177
28.3410682678 -7.7028126717
28.3362464905 -7.6921892166
28.3309288025 -7.6818447113
28.3251323700 -7.6717948914
28.3188724518 -7.6620545387
28.3121643066 -7.6526393890
28.3050231934 -7.6435651779
28.2974643707 -7.6348466873
28.2895050049 -7.6264996529
28.2811584473 -7.6185393333
28.2724399567 -7.6109809875
28.2633647919 -7.6038403511
28.2539501190 -7.5971326828
28.2442092896 -7.5908732414
28.2341594696 -7.5850772858
28.2238159180 -7.5797600746
28.2131919861 -7.5749373436
28.2023048401 -7.5706243515
28.1911697388 -7.5668368340
28.1798019409 -7.5635895729
28.1682167053 -7.5608983040
28.1564292908 -7.5587782860
28.1444549561 -7.5572447777
28.1323089600 -7.5563135147
28.1200046539 -7.5559997559
28.1077022552 -7.5563135147
0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70 71 72 73 74 75 76 77 78 79 80 81 82 83 84 85 86 87 88 89 90 91 92 93 94 95 96 97 98 99 100 101 102 103 104 105 106 107 108 109 110 111 112 113 114 115 116 117 118 119 120 121 122 123 124 125 126 127
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.direction = PropagationDirection::Inside;
  input.offset = std::numeric_limits<double>::infinity();
  input.label = "FontDot_Comp94";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_TRUE(output.is_valid);
}

TEST(straight_skeleton, Degenerate)
{
  const char *spec = R"(523 12
11.8568601608 -7.6549425125
10.5437030792 -7.6549425125
10.5437030792 -8.9680986404
9.2305488586 -8.9680986404
9.2305488586 -10.2812595367
7.9173879623 -10.2812595367
7.9173879623 -11.5944147110
6.6042366028 -11.5944147110
6.6042366028 -10.2812595367
5.2910795212 -10.2812595367
5.2910795212 -8.9680986404
3.9779253006 -8.9680986404
2.6647710800 -8.9680986404
2.6647710800 -10.2812595367
3.9779253006 -10.2812595367
3.9779253006 -11.5944147110
2.6647710800 -11.5944147110
1.3516169786 -11.5944147110
1.3516169786 -10.2812595367
0.0384659767 -10.2812595367
-1.2746944427 -10.2812595367
-2.5878458023 -10.2812595367
-3.9010028839 -10.2812595367
-3.9010028839 -8.9680986404
-2.5878458023 -8.9680986404
-1.2746944427 -8.9680986404
-1.2746944427 -7.6549425125
0.0384659767 -7.6549425125
1.3516169786 -7.6549425125
2.6647710800 -7.6549425125
3.9779253006 -7.6549425125
5.2910795212 -7.6549425125
6.6042366028 -7.6549425125
6.6042366028 -6.3417921066
6.6042366028 -5.0286369324
6.6042366028 -3.7154836655
5.2910795212 -3.7154836655
3.9779253006 -3.7154836655
3.9779253006 -2.4023208618
5.2910795212 -2.4023208618
6.6042366028 -2.4023208618
6.6042366028 -1.0891697407
7.9173879623 -1.0891697407
9.2305488586 -1.0891697407
10.5437030792 -1.0891697407
10.5437030792 0.2239854336
9.2305488586 0.2239854336
7.9173879623 0.2239854336
6.6042366028 0.2239854336
5.2910795212 0.2239854336
5.2910795212 -1.0891697407
3.9779253006 -1.0891697407
2.6647710800 -1.0891697407
1.3516169786 -1.0891697407
1.3516169786 -2.4023208618
0.0384659767 -2.4023208618
0.0384659767 -3.7154836655
1.3516169786 -3.7154836655
2.6647710800 -3.7154836655
2.6647710800 -5.0286369324
1.3516169786 -5.0286369324
0.0384659767 -5.0286369324
-1.2746944427 -5.0286369324
-2.5878458023 -5.0286369324
-2.5878458023 -6.3417921066
-3.9010028839 -6.3417921066
-3.9010028839 -7.6549425125
-5.2141599655 -7.6549425125
-5.2141599655 -6.3417921066
-6.5273113251 -6.3417921066
-6.5273113251 -5.0286369324
-7.8404684067 -5.0286369324
-7.8404684067 -3.7154836655
-6.5273113251 -3.7154836655
-5.2141599655 -3.7154836655
-3.9010028839 -3.7154836655
-2.5878458023 -3.7154836655
-2.5878458023 -2.4023208618
-3.9010028839 -2.4023208618
-3.9010028839 -1.0891697407
-5.2141599655 -1.0891697407
-5.2141599655 0.2239854336
-6.5273113251 0.2239854336
-6.5273113251 1.5371364355
-7.8404684067 1.5371364355
-7.8404684067 2.8502936363
-7.8404684067 4.1634492874
-6.5273113251 4.1634492874
-5.2141599655 4.1634492874
-5.2141599655 5.4766068459
-5.2141599655 6.7897577286
-6.5273113251 6.7897577286
-6.5273113251 5.4766068459
-7.8404684067 5.4766035080
-9.1536264420 5.4766035080
-9.1536264420 6.7897577286
-7.8404684067 6.7897577286
-7.8404684067 8.1029167175
-6.5273113251 8.1029167175
-6.5273113251 9.4160680771
-5.2141599655 9.4160680771
-3.9010028839 9.4160680771
-3.9010028839 10.7292213440
-5.2141599655 10.7292213440
-5.2141599655 12.0423765182
-3.9010028839 12.0423765182
-3.9010028839 13.3555355072
-3.9010028839 14.6686859131
-2.5878458023 14.6686859131
-1.2746944427 14.6686859131
0.0384659767 14.6686859131
0.0384659767 13.3555355072
-1.2746944427 13.3555355072
-2.5878458023 13.3555355072
-2.5878458023 12.0423765182
-1.2746944427 12.0423765182
-1.2746944427 10.7292213440
-1.2746944427 9.4160680771
0.0384659767 9.4160680771
1.3516169786 9.4160680771
1.3516169786 10.7292213440
1.3516169786 12.0423765182
1.3516169786 13.3555355072
2.6647710800 13.3555355072
2.6647710800 14.6686859131
2.6647710800 15.9818372726
1.3516169786 15.9818372726
1.3516169786 17.2949943542
2.6647710800 17.2949943542
3.9779253006 17.2949943542
3.9779253006 15.9818372726
3.9779253006 14.6686859131
5.2910795212 14.6686859131
5.2910795212 13.3555355072
5.2910795212 12.0423765182
3.9779253006 12.0423765182
3.9779253006 10.7292213440
5.2910795212 10.7292213440
6.6042366028 10.7292213440
6.6042366028 12.0423765182
6.6042366028 13.3555355072
7.9173879623 13.3555355072
7.9173879623 14.6686859131
7.9173879623 15.9818372726
7.9173879623 17.2949943542
9.2305488586 17.2949943542
10.5437030792 17.2949943542
10.5437030792 15.9818372726
10.5437030792 14.6686859131
10.5437030792 13.3555355072
10.5437030792 12.0423765182
9.2305488586 12.0423765182
7.9173879623 12.0423765182
7.9173879623 10.7292213440
7.9173879623 9.4160680771
7.9173879623 8.1029167175
9.2305488586 8.1029167175
10.5437030792 8.1029167175
10.5437030792 6.7897577286
11.8568601608 6.7897577286
11.8568601608 8.1029167175
11.8568601608 9.4160680771
13.1700143814 9.4160680771
13.1700143814 10.7292213440
13.1700143814 12.0423765182
13.1700143814 13.3555355072
14.4831647873 13.3555355072
14.4831647873 14.6686859131
14.4831647873 15.9818372726
15.7963256836 15.9818372726
15.7963256836 14.6686859131
17.1094779968 14.6686859131
17.1094779968 13.3555355072
18.4226379395 13.3555355072
18.4226379395 12.0423765182
18.4226379395 10.7292213440
17.1094779968 10.7292213440
17.1094779968 12.0423765182
15.7963256836 12.0423765182
15.7963256836 10.7292213440
14.4831647873 10.7292213440
14.4831647873 9.4160680771
15.7963256836 9.4160680771
17.1094779968 9.4160680771
17.1094779968 8.1029167175
18.4226379395 8.1029167175
18.4226379395 6.7897577286
18.4226379395 5.4766068459
19.7357883453 5.4766068459
19.7357883453 4.1634492874
19.7357883453 2.8502936363
19.7357883453 1.5371364355
18.4226379395 1.5371364355
18.4226379395 2.8502936363
18.4226379395 4.1634492874
17.1094779968 4.1634492874
17.1094779968 5.4766068459
17.1094779968 6.7897577286
15.7963256836 6.7897577286
15.7963256836 8.1029167175
14.4831647873 8.1029167175
14.4831647873 6.7897577286
13.1700143814 6.7897577286
13.1700143814 5.4766068459
11.8568601608 5.4766068459
11.8568601608 4.1634492874
11.8568601608 2.8502936363
13.1700143814 2.8502936363
14.4831647873 2.8502936363
14.4831647873 4.1634492874
14.4831647873 5.4766068459
15.7963256836 5.4766068459
15.7963256836 4.1634492874
15.7963256836 2.8502936363
15.7963256836 1.5371364355
15.7963256836 0.2239854336
17.1094779968 0.2239854336
18.4226379395 0.2239854336
19.7357883453 0.2239854336
19.7357883453 -1.0891697407
18.4226379395 -1.0891697407
18.4226379395 -2.4023208618
18.4226379395 -3.7154836655
19.7357883453 -3.7154836655
19.7357883453 -5.0286369324
19.7357883453 -6.3417921066
19.7357883453 -7.6549425125
18.4226379395 -7.6549425125
18.4226379395 -8.9680986404
17.1094779968 -8.9680986404
17.1094779968 -10.2812595367
15.7963256836 -10.2812595367
15.7963256836 -11.5944147110
14.4831647873 -11.5944147110
14.4831647873 -10.2812595367
14.4831647873 -8.9680986404
14.4831647873 -7.6549425125
15.7963256836 -7.6549425125
15.7963256836 -6.3417921066
17.1094779968 -6.3417921066
17.1094779968 -5.0286369324
17.1094779968 -3.7154836655
17.1094779968 -2.4023208618
15.7963256836 -2.4023208618
15.7963256836 -3.7154836655
14.4831647873 -3.7154836655
14.4831647873 -5.0286369324
13.1700143814 -5.0286369324
13.1700143814 -6.3417921066
11.8568601608 -6.3417921066
6.6042366028 2.8502936363
7.9173879623 2.8502936363
7.9173879623 4.1634492874
9.2305488586 4.1634492874
9.2305488586 5.4766068459
10.5437030792 5.4766068459
10.5437030792 4.1634492874
10.5437030792 2.8502936363
9.2305488586 2.8502936363
9.2305488586 1.5371364355
7.9173879623 1.5371364355
6.6042366028 1.5371364355
5.2910795212 1.5371364355
5.2910795212 2.8502936363
3.9779253006 2.8502936363
3.9779253006 4.1634492874
3.9779253006 5.4766068459
2.6647710800 5.4766068459
2.6647710800 4.1634492874
1.3516169786 4.1634492874
0.0384659767 4.1634492874
0.0384659767 2.8502936363
0.0384659767 1.5371364355
1.3516169786 1.5371364355
2.6647710800 1.5371364355
3.9779253006 1.5371364355
3.9779253006 0.2239854336
2.6647710800 0.2239854336
1.3516169786 0.2239854336
0.0384659767 0.2239854336
-1.2746944427 0.2239854336
-2.5878458023 0.2239854336
-3.9010028839 0.2239854336
-3.9010028839 1.5371364355
-3.9010028839 2.8502936363
-2.5878458023 2.8502936363
-2.5878458023 4.1634492874
-2.5878458023 5.4766068459
-3.9010028839 5.4766068459
-3.9010028839 6.7897577286
-3.9010028839 8.1029167175
-2.5878458023 8.1029167175
-1.2746944427 8.1029167175
-1.2746944427 6.7897577286
-1.2746944427 5.4766068459
0.0384659767 5.4766068459
1.3516169786 5.4766068459
1.3516169786 6.7897577286
0.0384659767 6.7897577286
0.0384659767 8.1029167175
1.3516169786 8.1029167175
2.6647710800 8.1029167175
3.9779253006 8.1029167175
5.2910795212 8.1029167175
6.6042366028 8.1029167175
6.6042366028 6.7897577286
6.6042366028 5.4766068459
5.2910795212 5.4766068459
5.2910795212 4.1634492874
6.6042366028 4.1634492874
11.8568601608 1.5371364355
13.1700143814 1.5371364355
13.1700143814 0.2239854336
14.4831647873 0.2239854336
14.4831647873 -1.0891697407
13.1700143814 -1.0891697407
13.1700143814 -2.4023208618
11.8568601608 -2.4023208618
11.8568601608 -1.0891697407
11.8568601608 0.2239854336
23.6815071106 -6.4697246552
23.6815071106 -8.0362596512
25.2480392456 -8.0362596512
25.2480392456 -9.6027927399
26.8145828247 -9.6027927399
26.8145828247 -11.1693267822
28.3811092377 -11.1693267822
28.3811092377 -12.7358665466
29.9476470947 -12.7358665466
29.9476470947 -11.1693267822
31.5141811371 -11.1693267822
31.5141811371 -9.6027927399
33.0807189941 -9.6027927399
33.0807189941 -8.0362596512
34.6472549438 -8.0362596512
34.6472549438 -6.4697246552
36.2137870789 -6.4697246552
36.2137870789 -4.9031882286
34.6472549438 -4.9031882286
34.6472549438 -3.3366551399
33.0807189941 -3.3366551399
33.0807189941 -1.7701210976
31.5141811371 -1.7701210976
31.5141811371 -0.2035849094
29.9476470947 -0.2035849094
29.9476470947 1.3629510403
28.3811092377 1.3629510403
28.3811092377 -0.2035849094
26.8145828247 -0.2035849094
26.8145828247 -1.7701210976
25.2480392456 -1.7701210976
25.2480392456 -3.3366551399
23.6815071106 -3.3366551399
23.6815071106 -4.9031882286
22.1149711609 -4.9031910896
22.1149711609 -6.4697246552
-13.7338123322 -1.7815699577
-13.1525421143 -4.6454281807
-10.5510692596 -3.3142976761
-12.4277076721 -5.5543432236
-9.7649955750 -6.7583112717
-12.6863985062 -6.6877374649
-11.9675216675 -9.5201902390
-13.7338123322 -7.1921472549
-15.5001049042 -9.5201902390
-14.7812271118 -6.6877374649
-17.7026309967 -6.7583112717
-15.0399188995 -5.5543432236
-16.9165515900 -3.3142976761
-14.3150806427 -4.6454281807
24.9640674591 4.6126074791
24.9640674591 6.3037137985
23.2729568481 6.3037137985
23.9170074463 8.7983493805
21.5394248962 7.8058333397
23.4299259186 9.5562553406
20.8931827545 10.0067214966
23.4299259186 10.4571838379
21.5394248962 12.2076053619
23.9170074463 11.2150926590
23.2729625702 13.7097263336
24.9640674591 13.7097234726
24.9640674591 15.4008312225
27.4587059021 14.7567844391
26.4661865234 17.1343650818
28.2166080475 15.2438621521
28.6670742035 17.7806072235
29.1175384521 15.2438621521
30.8679599762 17.1343650818
29.8754425049 14.7567844391
32.3700790405 15.4008312225
32.3700790405 13.7097234726
34.0611839294 13.7097234726
33.4171371460 11.2150897980
35.7947235107 12.2076053619
33.9042129517 10.4571838379
36.4409599304 10.0067176819
33.9042129517 9.5562553406
35.7947235107 7.8058333397
33.4171371460 8.7983493805
34.0611839294 6.3037118912
32.3700790405 6.3037137985
32.3700790405 4.6126074791
29.8754425049 5.2566547394
30.8679599762 2.8790695667
29.1175384521 4.7695760727
28.6670742035 2.2328307629
28.2166080475 4.7695760727
26.4661865234 2.8790695667
27.4586982727 5.2566547394
-20.5931663513 -4.3127665520
-19.8408088684 -5.5382900238
-19.8408088684 -6.8183174133
-20.5931663513 -7.8538713455
-21.8105354309 -8.2493934631
-23.0278854370 -7.8538713455
-23.7802753448 -6.8183174133
-23.7802753448 -5.5382900238
-23.0278854370 -4.3127665520
-23.0278854370 -2.3172845840
-20.5931663513 -2.3172845840
-21.0333309174 9.4777526855
-21.2109584808 9.1089000702
-21.5310363770 8.8536434174
-21.9301815033 8.8300952911
-22.3293132782 8.8536434174
-22.6493892670 9.1089000702
-22.8270187378 9.4777526855
-22.8270187378 9.8871555328
-23.4109687805 9.8871555328
-23.4109687805 9.4777526855
-23.5885982513 9.1089000702
-23.9086875916 8.8536434174
-24.3078193665 8.7625598907
-24.7069511414 8.8536434174
-25.0270290375 9.1089000702
-25.2046585083 9.4777526855
-25.2046585083 9.8871555328
-25.8264675140 9.8871555328
-25.8264675140 9.3186302185
-26.1606426239 8.8586711884
-26.7013492584 8.6829738617
-27.2420578003 8.8586711884
-27.5762329102 9.3186302185
-27.5762329102 9.8871555328
-28.0521850586 9.8871555328
-28.3216171265 9.2366933823
-28.9720916748 8.9672365189
-29.6225624084 9.2366933823
-29.8919944763 9.8871555328
-30.8534126282 9.8871555328
-30.9192810059 13.6844892502
-10.9222354889 13.6844892502
-10.9222354889 9.8871555328
-12.0269079208 9.8871555328
-11.8472452164 9.4404096603
-11.7546844482 9.0352516174
-11.7546844482 8.6034383774
-11.9926481247 8.1449699402
-12.5792379379 7.9797077179
-13.1658153534 8.1449699402
-13.4037914276 8.6034383774
-13.4037914276 9.0352516174
-13.3112182617 9.4404096603
-13.1315689087 9.8871555328
-14.2362270355 9.8871555328
-14.2362270355 9.3689842224
-14.4997844696 9.0357818604
-15.0081653595 8.8284778595
-15.4645633698 8.8284778595
-15.9144535065 8.9580402374
-16.1715049744 9.3689842224
-16.1715049744 9.8871555328
-16.9445571899 9.8871555328
-16.9445571899 8.9308032990
-17.8961849213 8.9308032990
-17.8961849213 9.8871555328
-18.4796161652 9.8871555328
-18.5888900757 9.2674674988
-19.0709171295 8.8629913330
-19.7001686096 8.8629913330
-20.1822090149 9.2674674988
-20.2914714813 9.8871555328
-21.0333309174 9.8871555328
-19.8342399597 6.5941524506
-19.8342399597 3.7889194489
-22.5618133545 3.7889194489
-22.5618133545 1.8572807312
-25.2893981934 1.8572933674
-25.2893981934 3.7889194489
-28.0169773102 3.7889194489
-28.0169773102 6.5941524506
-26.8435821533 -3.2007102966
-26.8435821533 -3.8182110786
-27.1114978790 -4.3745498657
-27.5942726135 -4.7595543861
-28.1962795258 -4.8969717026
-28.7982940674 -4.7595543861
-29.2810821533 -4.3745498657
-29.5489959717 -3.8182110786
-29.5489959717 -3.2007102966
-30.9620971680 -3.2007102966
-30.9620971680 -0.0094728470
-25.2746372223 -0.0094728470
-25.2746372223 -3.2007102966
-16.0709095001 3.1954288483
-14.2367839813 3.1954288483
-15.6482295990 0.0183727741
-11.1821203232 0.0183727741
-12.5935697556 3.1954288483
-10.7594375610 3.1954288483
-10.7594375610 4.2634882927
-12.6451272964 4.2634882927
-11.7598800659 6.6289963722
-15.0704708099 6.6289963722
-14.1852302551 4.2634882927
-16.0709095001 4.2634882927
-28.2069740295 -8.9100227356
-30.5849151611 -10.2761173248
-30.5849151611 -5.7920284271
-28.2069740295 -7.1581363678
-25.8290195465 -5.7920284271
-25.8290195465 -10.2761173248
0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70 71 72 73 74 75 76 77 78 79 80 81 82 83 84 85 86 87 88 89 90 91 92 93 94 95 96 97 98 99 100 101 102 103 104 105 106 107 108 109 110 111 112 113 114 115 116 117 118 119 120 121 122 123 124 125 126 127 128 129 130 131 132 133 134 135 136 137 138 139 140 141 142 143 144 145 146 147 148 149 150 151 152 153 154 155 156 157 158 159 160 161 162 163 164 165 166 167 168 169 170 171 172 173 174 175 176 177 178 179 180 181 182 183 184 185 186 187 188 189 190 191 192 193 194 195 196 197 198 199 200 201 202 203 204 205 206 207 208 209 210 211 212 213 214 215 216 217 218 219 220 221 222 223 224 225 226 227 228 229 230 231 232 233 234 235 236 237 238 239 240 241 242 243 244 245 246 247 248 249
250 251 252 253 254 255 256 257 258 259 260 261 262 263 264 265 266 267 268 269 270 271 272 273 274 275 276 277 278 279 280 281 282 283 284 285 286 287 288 289 290 291 292 293 294 295 296 297 298 299 300 301 302 303 304 305 306 307 308 309
310 311 312 313 314 315 316 317 318 319
320 321 322 323 324 325 326 327 328 329 330 331 332 333 334 335 336 337 338 339 340 341 342 343 344 345 346 347 348 349 350 351 352 353 354 355
356 357 358 359 360 361 362 363 364 365 366 367 368 369
370 371 372 373 374 375 376 377 378 379 380 381 382 383 384 385 386 387 388 389 390 391 392 393 394 395 396 397 398 399 400 401 402 403 404 405 406 407 408 409
410 411 412 413 414 415 416 417 418 419 420
421 422 423 424 425 426 427 428 429 430 431 432 433 434 435 436 437 438 439 440 441 442 443 444 445 446 447 448 449 450 451 452 453 454 455 456 457 458 459 460 461 462 463 464 465 466 467 468 469 470 471 472 473 474 475 476 477 478 479 480 481 482 483
484 485 486 487 488 489 490 491
492 493 494 495 496 497 498 499 500 501 502 503 504
505 506 507 508 509 510 511 512 513 514 515 516
517 518 519 520 521 522
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.direction = PropagationDirection::Inside;
  input.offset = std::numeric_limits<double>::infinity();
  input.label = "Degenerate";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_TRUE(output.is_valid);
}

/** \} */

} /* namespace blender::straightskeleton::test */
