/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

#include "testing/testing.h"

#ifdef WITH_TRACY
#  include "tracy/Tracy.hpp"
#endif

#include "BLI_array.hh"
#include "BLI_function_ref.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_straight_skeleton.hh"
#include "BLI_time.hh"
#include "BLI_vector.hh"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace blender::straightskeleton {

/* -------------------------------------------------------------------- */
/** \name Timing and Table Formatting Helpers
 * \{ */

static double run_iterations(const FunctionRef<void()> function, const int default_iterations = 1)
{
  int iterations = default_iterations;
  if (const char *env = std::getenv("BENCHMARK_ITERATIONS")) {
    iterations = std::max(1, std::atoi(env));
  }
  double min_time = 1e30;
  for (int repetition = 0; repetition < iterations; repetition++) {
    const double time_before = BLI_time_now_seconds();
    function();
    const double time_after = BLI_time_now_seconds();
    min_time = std::min(min_time, time_after - time_before);
  }
  return min_time;
}

static void print_time(const double seconds)
{
  if (seconds < 0.001) {
    printf(" %8.1f µs |", seconds * 1000000.0);
  }
  else if (seconds < 1.0) {
    printf(" %8.2f ms |", seconds * 1000.0);
  }
  else {
    printf(" %8.3f s  |", seconds);
  }
}

static bool is_debug_drawings_enabled()
{
  if (const char *env = std::getenv("STRAIGHT_SKELETON_DEBUG")) {
    return (std::string_view(env) != "0" && std::string_view(env) != "none");
  }
  return true;
}

static void print_table_header(const char *title, const char *n_label)
{
  printf("\n#### %s\n\n", title);
  printf("| %s | vertices | full skeleton | 5 segments | events | faces |\n", n_label);
  printf("|---:|---------:|--------------:|-----------:|-------:|------:|\n");
  fflush(stdout);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name SVG Debug Drawings
 * \{ */

namespace test {

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

}  // namespace test

/** \} */

/* -------------------------------------------------------------------- */
/** \name Geometry Storage and Construction Helpers
 * \{ */

struct GeneratedPolygon {
  Array<double2> vert;
  Vector<int> outer_polygon;
  Vector<int> hole_offsets_buf;
  Vector<int> hole_vert_indices;

  StraightSkeletonInput to_input() const
  {
    StraightSkeletonInput input;
    input.vert = vert.as_span();
    input.outer_polygon = outer_polygon.as_span();
    if (!hole_offsets_buf.is_empty()) {
      input.hole_offsets = OffsetIndices<int>(hole_offsets_buf.as_span());
      input.hole_vert_indices = hole_vert_indices.as_span();
    }
    return input;
  }
};

/**
 * Circle approximating a regular n-gon with n vertices, oriented CCW.
 */
static GeneratedPolygon make_circle(const int n, const double radius = 100.0)
{
  GeneratedPolygon geom;
  geom.vert.reinitialize(n);
  geom.outer_polygon.reserve(n);
  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    geom.vert[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    geom.outer_polygon.append(i);
  }
  return geom;
}

/**
 * Square with subdivided edges, where n is the total number of vertices on each side.
 * Each side has n vertices (n - 1 segments). Total vertices: 4 * (n - 1), oriented CCW.
 */
static GeneratedPolygon make_subdivided_square(const int n, const double half_size = 100.0)
{
  BLI_assert(n >= 2);
  const int segments_per_side = n - 1;
  const int total_verts_num = 4 * segments_per_side;

  GeneratedPolygon geom;
  geom.vert.reinitialize(total_verts_num);
  geom.outer_polygon.reserve(total_verts_num);

  const double l = half_size;
  int idx = 0;

  /* Side 0: Bottom edge (left to right, -l to +l at y = -l). */
  for (const int k : IndexRange(segments_per_side)) {
    const double x = -l + (2.0 * l * double(k)) / double(segments_per_side);
    geom.vert[idx] = double2(x, -l);
    geom.outer_polygon.append(idx);
    idx++;
  }

  /* Side 1: Right edge (bottom to top, -l to +l at x = +l). */
  for (const int k : IndexRange(segments_per_side)) {
    const double y = -l + (2.0 * l * double(k)) / double(segments_per_side);
    geom.vert[idx] = double2(l, y);
    geom.outer_polygon.append(idx);
    idx++;
  }

  /* Side 2: Top edge (right to left, +l to -l at y = +l). */
  for (const int k : IndexRange(segments_per_side)) {
    const double x = l - (2.0 * l * double(k)) / double(segments_per_side);
    geom.vert[idx] = double2(x, l);
    geom.outer_polygon.append(idx);
    idx++;
  }

  /* Side 3: Left edge (top to bottom, +l to -l at x = -l). */
  for (const int k : IndexRange(segments_per_side)) {
    const double y = l - (2.0 * l * double(k)) / double(segments_per_side);
    geom.vert[idx] = double2(-l, y);
    geom.outer_polygon.append(idx);
    idx++;
  }

  return geom;
}

/**
 * Circle with circular holes inside:
 * n vertices on outer circle (CCW), and n smaller non-touching circular 8-gon holes (CW)
 * centered on the circle at half-radius.
 */
static GeneratedPolygon make_circle_with_holes(const int n,
                                               const int hole_verts_num = 8,
                                               const double radius = 100.0)
{
  BLI_assert(n >= 3);
  const int total_verts_num = n + n * hole_verts_num;

  GeneratedPolygon geom;
  geom.vert.reinitialize(total_verts_num);
  geom.outer_polygon.reserve(n);

  /* Outer circle (CCW). */
  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    geom.vert[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    geom.outer_polygon.append(i);
  }

  /* Holes centered at half-radius. */
  const double r_mid = radius * 0.5;
  const double d_adj = 2.0 * r_mid * std::sin(M_PI / double(n));
  const double r_hole = std::min(0.35 * r_mid, 0.35 * d_adj);

  geom.hole_offsets_buf.reserve(n + 1);
  geom.hole_offsets_buf.append(0);
  geom.hole_vert_indices.reserve(n * hole_verts_num);

  int v_idx = n;
  for (const int j : IndexRange(n)) {
    /* Offset by half-step so holes do not lie on radial rays of outer vertices. */
    const double phi = (2.0 * M_PI * (double(j) + 0.5)) / double(n);
    const double2 center(r_mid * std::cos(phi), r_mid * std::sin(phi));

    /* 8-gon hole oriented Clockwise (CW): angle decreases. */
    for (const int k : IndexRange(hole_verts_num)) {
      const double alpha = (-2.0 * M_PI * double(k)) / double(hole_verts_num);
      geom.vert[v_idx] = center + double2(r_hole * std::cos(alpha), r_hole * std::sin(alpha));
      geom.hole_vert_indices.append(v_idx);
      v_idx++;
    }
    geom.hole_offsets_buf.append(int(geom.hole_vert_indices.size()));
  }

  return geom;
}

/**
 * Koch snowflake fractal, where n is the number of refinement iterations.
 * Iteration 0 is an equilateral triangle (3 vertices, CCW).
 * Total vertices: 3 * 4^iterations.
 */
static GeneratedPolygon make_koch_snowflake(const int iterations, const double radius = 100.0)
{
  BLI_assert(iterations >= 0);

  /* Base equilateral triangle (iteration 0) oriented CCW. */
  Vector<double2> current_pts;
  current_pts.append(double2(0.0, radius));
  current_pts.append(
      double2(radius * std::cos(7.0 * M_PI / 6.0), radius * std::sin(7.0 * M_PI / 6.0)));
  current_pts.append(
      double2(radius * std::cos(11.0 * M_PI / 6.0), radius * std::sin(11.0 * M_PI / 6.0)));

  for ([[maybe_unused]] const int iter : IndexRange(iterations)) {
    Vector<double2> next_pts;
    const int m = int(current_pts.size());
    next_pts.reserve(4 * m);

    for (const int i : IndexRange(m)) {
      const double2 a = current_pts[i];
      const double2 b = current_pts[(i + 1) % m];
      const double2 v = b - a;

      const double2 p1 = a;
      const double2 p2 = a + (1.0 / 3.0) * v;
      /* Outward equilateral triangle apex on middle third. */
      const double2 p3 = a + 0.5 * v + (std::sqrt(3.0) / 6.0) * double2(v.y, -v.x);
      const double2 p4 = a + (2.0 / 3.0) * v;

      next_pts.append(p1);
      next_pts.append(p2);
      next_pts.append(p3);
      next_pts.append(p4);
    }
    current_pts = std::move(next_pts);
  }

  const int total_verts_num = int(current_pts.size());
  GeneratedPolygon geom;
  geom.vert.reinitialize(total_verts_num);
  geom.outer_polygon.reserve(total_verts_num);

  for (const int i : IndexRange(total_verts_num)) {
    geom.vert[i] = current_pts[i];
    geom.outer_polygon.append(i);
  }

  return geom;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Benchmark Row Runner
 * \{ */

static void run_benchmark_row(const char *name, const int n, const GeneratedPolygon &geom)
{
  char label_full[128];
  std::snprintf(label_full, sizeof(label_full), "%s n=%d (full)", name, n);
  StraightSkeletonInput input_full = geom.to_input();
  input_full.label = label_full;
  StraightSkeletonOutput output_full;

  /* Benchmark full straight skeleton computation. */
  const double time_full = run_iterations(
      [&]() { output_full = straight_skeleton_calc(input_full); });

  /* Benchmark banded offset computation (5 segments). */
  char label_segmented[128];
  std::snprintf(label_segmented, sizeof(label_segmented), "%s n=%d (5 segments)", name, n);
  StraightSkeletonInput input_segmented = geom.to_input();
  input_segmented.segments = 5;
  input_segmented.label = label_segmented;
  StraightSkeletonOutput output_segmented;
  const double time_segmented = run_iterations(
      [&]() { output_segmented = straight_skeleton_calc(input_segmented); });

  if (is_debug_drawings_enabled()) {
    char svg_name_full[128];
    std::snprintf(svg_name_full, sizeof(svg_name_full), "%s_n%d_full", name, n);
    test::write_debug_svg(svg_name_full, input_full, output_full);

    char svg_name_segmented[128];
    std::snprintf(svg_name_segmented, sizeof(svg_name_segmented), "%s_n%d_5segments", name, n);
    test::write_debug_svg(svg_name_segmented, input_segmented, output_segmented);
  }

  printf("| %3d | %8d |", n, int(geom.vert.size()));
  print_time(time_full);
  print_time(time_segmented);
  printf(
      " %6d | %5d |\n", output_full.events_processed_num, int(output_full.face_offsets().size()));
  fflush(stdout);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Performance Tests
 * \{ */

TEST(straight_skeleton_performance, Circle)
{
  print_table_header("Straight Skeleton: Circle (n = vertices on perimeter)", "  n");
  const int n_values[] = {8, 16, 32, 64, 128, 256, 512};
  for (const int n : n_values) {
    const GeneratedPolygon geom = make_circle(n);
    run_benchmark_row("Circle", n, geom);
  }
  printf("\n");
}

TEST(straight_skeleton_performance, SubdividedSquare)
{
  print_table_header("Straight Skeleton: Subdivided Square (n = vertices per side)", "  n");
  const int n_values[] = {2, 4, 8, 16, 32, 64, 128, 256};
  for (const int n : n_values) {
    const GeneratedPolygon geom = make_subdivided_square(n);
    run_benchmark_row("Subdivided Square", n, geom);
  }
  printf("\n");
}

TEST(straight_skeleton_performance, CircleWithHoles)
{
  print_table_header("Straight Skeleton: Circle with Holes (n = outer vertices & hole count)",
                     "  n");
  const int n_values[] = {6, 12, 24, 48, 96};
  for (const int n : n_values) {
    const GeneratedPolygon geom = make_circle_with_holes(n);
    run_benchmark_row("Circle with Holes", n, geom);
  }
  printf("\n");
}

TEST(straight_skeleton_performance, KochSnowflake)
{
#ifdef WITH_TRACY
  if (std::getenv("TRACY_WAIT") != nullptr) {
    printf("\nWaiting for Tracy profiler to connect on port 8086...\n");
    fflush(stdout);
    while (!TracyIsConnected) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("Connected to Tracy! Starting benchmark in 500ms...\n\n");
    fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
#endif

  print_table_header("Straight Skeleton: Koch Snowflake (n = refinement iterations)", "  n");
  const int n_values[] = {0, 1, 2, 3, 4, 5};
  for (const int n : n_values) {
    const GeneratedPolygon geom = make_koch_snowflake(n);
    run_benchmark_row("Koch Snowflake", n, geom);
  }
  printf("\n");

#ifdef WITH_TRACY
  if (TracyIsConnected) {
    /* Allow network buffers to flush to profiler / capture server before exiting. */
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (std::getenv("TRACY_WAIT") != nullptr) {
      printf("\nBenchmark complete and profiled by Tracy! Waiting for disconnect or close...\n");
      fflush(stdout);
      while (TracyIsConnected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }
  }
#endif
}

/** \} */

}  // namespace blender::straightskeleton
