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
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"
#include "BLI_straight_skeleton.hh"
#include "BLI_vector.hh"

/* Debug output mode for tests:
 *   DebugOutputMode::NONE   (0) - No debug output (standard quiet test run, assert failures only).
 *   DebugOutputMode::STEPS  (1) - Algorithm event loop steps (kinetic triangulation, priority
 * queue). DebugOutputMode::OUTPUT (2) - Final straight skeleton output: vertices, edges, faces,
 * orig_ values, and SVG diagrams. queue). DebugOutputMode::OUTPUT (2) - Final straight skeleton
 * output: vertices, edges, faces, and orig_ values.
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

namespace blender::straightskeleton::test {

/* -------------------------------------------------------------------- */
/** \name Input helpers
 * \{ */

/**
 * Owns the backing storage for a #StraightSkeletonInput.
 *
 * The spec string has the form:
 *   #verts #edges #faces
 *   <double> <double>          [#verts lines -- 2D x y]
 *   <int> <int>                [#edges lines]
 *   <int> <int> ... <int>      [#faces lines]
 */
struct InputStorage {
  Array<double2> vert;
  Array<int2> edge;
  Array<int> face_offsets_buf;
  Vector<int> face_vert_indices;
};

static StraightSkeletonInput fill_input_from_string(const char *spec, InputStorage &r_storage)
{
  std::istringstream ss(spec);
  std::string line;
  getline(ss, line);
  std::istringstream hdrss(line);
  int verts_num, edges_num, faces_num;
  hdrss >> verts_num >> edges_num >> faces_num;

  r_storage.vert.reinitialize(verts_num);
  r_storage.edge.reinitialize(edges_num);
  r_storage.face_offsets_buf.reinitialize(faces_num + 1);

  int i = 0;
  while (i < verts_num && getline(ss, line)) {
    std::istringstream iss(line);
    double x, y;
    iss >> x >> y;
    r_storage.vert[i] = double2(x, y);
    i++;
  }

  i = 0;
  while (i < edges_num && getline(ss, line)) {
    std::istringstream ess(line);
    int e0, e1;
    ess >> e0 >> e1;
    r_storage.edge[i] = int2(e0, e1);
    i++;
  }

  i = 0;
  while (i < faces_num && getline(ss, line)) {
    r_storage.face_offsets_buf[i] = int(r_storage.face_vert_indices.size());
    std::istringstream fss(line);
    int v;
    while (fss >> v) {
      r_storage.face_vert_indices.append(v);
    }
    i++;
  }
  r_storage.face_offsets_buf[faces_num] = int(r_storage.face_vert_indices.size());

  StraightSkeletonInput input;
  input.vert = r_storage.vert.as_span();
  input.edge = r_storage.edge.as_span();
  input.face_offsets = OffsetIndices<int>(r_storage.face_offsets_buf.as_span());
  input.face_vert_indices = r_storage.face_vert_indices.as_span();
  return input;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug drawing
 * \{ */

static void write_debug_svg(const char *test_name,
                            const StraightSkeletonInput &input,
                            const StraightSkeletonOutput &output)
{
  const char *svg_dir_env = std::getenv("STRAIGHT_SKELETON_SVG_DIR");
  std::string dir = svg_dir_env ? svg_dir_env : "/tmp";
  std::string file_path = dir + "/" + (test_name ? test_name : "test") + ".svg";

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

  const double base_size = 650.0;
  const double scale = std::min(base_size / dx, base_size / dy);
  const double width = dx * scale + 100.0;
  const double height = dy * scale + 120.0;

  auto tx = [&](double x) { return 50.0 + (x - xmin) * scale; };
  auto ty = [&](double y) { return height - 50.0 - (y - ymin) * scale; };

  std::ofstream f(file_path);
  if (!f.is_open()) {
    return;
  }

  f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  f << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << width << " " << height
    << "\" width=\"" << width << "\" height=\"" << height
    << "\" style=\"background-color:#ffffff; overflow:hidden; font-family: -apple-system, "
       "BlinkMacSystemFont, "
       "'Segoe UI', Roboto, Helvetica, Arial, sans-serif;\">\n";

  /* Definitions & styles. */
  f << "<defs>\n"
    << "  <filter id=\"shadow\" x=\"-10%\" y=\"-10%\" width=\"120%\" height=\"120%\">\n"
    << "    <feDropShadow dx=\"1\" dy=\"1\" stdDeviation=\"1.5\" flood-opacity=\"0.2\"/>\n"
    << "  </filter>\n"
    << "</defs>\n";

  /* Title. */
  f << "<text x=\"30\" y=\"40\" font-size=\"20\" font-weight=\"bold\" fill=\"#2c3e50\">"
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

  /* Draw faces. */
  const Span<int> fvi = output.face_vert_indices;
  for (const int fi : output.face_offsets.index_range()) {
    const IndexRange r = output.face_offsets[fi];
    const int n = int(r.size());
    if (n < 3) {
      continue;
    }
    const char *col = face_colors[fi % num_colors];
    f << "<polygon points=\"";
    double cx = 0.0, cy = 0.0;
    for (const int idx : r) {
      const double2 p = output.vert[fvi[idx]];
      f << tx(p.x) << "," << ty(p.y) << " ";
      cx += p.x;
      cy += p.y;
    }
    cx /= double(n);
    cy /= double(n);
    f << "\" fill=\"" << col << "\" stroke=\"#7f8c8d\" stroke-width=\"1\" opacity=\"0.85\"/>\n";

    /* Face label at centroid. */
    f << "<text x=\"" << tx(cx) << "\" y=\"" << ty(cy)
      << "\" font-size=\"14\" font-weight=\"bold\" fill=\"#34495e\" text-anchor=\"middle\" "
         "dominant-baseline=\"middle\" filter=\"url(#shadow)\">f"
      << fi << "</text>\n";
  }

  /* Identify input boundary edges to distinguish from straight skeleton internal edges. */
  Set<std::pair<int, int>> input_boundary_edges;
  const Span<int> ivi = input.face_vert_indices;
  for (const int fi : input.face_offsets.index_range()) {
    const IndexRange r = input.face_offsets[fi];
    const int n = int(r.size());
    for (int k = 0; k < n; k++) {
      const int u = ivi[r[k]];
      const int v = ivi[r[(k + 1) % n]];
      input_boundary_edges.add({std::min(u, v), std::max(u, v)});
    }
  }
  for (const int2 &e : input.edge) {
    input_boundary_edges.add({std::min(e[0], e[1]), std::max(e[0], e[1])});
  }

  /* Draw straight skeleton edges (non-boundary internal arcs in crimson red). */
  for (const int2 &e : output.edge) {
    const std::pair<int, int> pair_e = {std::min(e[0], e[1]), std::max(e[0], e[1])};
    if (input_boundary_edges.contains(pair_e)) {
      continue;
    }
    const double2 p1 = output.vert[e[0]];
    const double2 p2 = output.vert[e[1]];
    f << "<line x1=\"" << tx(p1.x) << "\" y1=\"" << ty(p1.y) << "\" x2=\"" << tx(p2.x)
      << "\" y2=\"" << ty(p2.y)
      << "\" stroke=\"#e74c3c\" stroke-width=\"2.5\" stroke-linecap=\"round\"/>\n";
  }

  /* Draw input boundary edges in bold dark blue. */
  for (const auto &e : input_boundary_edges) {
    if (e.first < output.vert.size() && e.second < output.vert.size()) {
      const double2 p1 = output.vert[e.first];
      const double2 p2 = output.vert[e.second];
      f << "<line x1=\"" << tx(p1.x) << "\" y1=\"" << ty(p1.y) << "\" x2=\"" << tx(p2.x)
        << "\" y2=\"" << ty(p2.y)
        << "\" stroke=\"#2980b9\" stroke-width=\"3.5\" stroke-linecap=\"round\"/>\n";
    }
  }

  /* Draw vertices and vertex labels. */
  for (const int vi : output.vert.index_range()) {
    const double2 p = output.vert[vi];
    const bool is_input_vert = (vi < input.vert.size());
    const char *circle_color = is_input_vert ? "#2980b9" : "#e74c3c";
    const double r = is_input_vert ? 5.0 : 4.0;

    f << "<circle cx=\"" << tx(p.x) << "\" cy=\"" << ty(p.y) << "\" r=\"" << r << "\" fill=\""
      << circle_color << "\" stroke=\"#ffffff\" stroke-width=\"1.5\" filter=\"url(#shadow)\"/>\n";

    /* Offset label away from vertex. */
    f << "<text x=\"" << (tx(p.x) + 7.0) << "\" y=\"" << (ty(p.y) - 6.0)
      << "\" font-size=\"11\" font-weight=\"bold\" fill=\""
      << (is_input_vert ? "#1a5276" : "#922b21") << "\">v" << vi << "</text>\n";
  }

  /* Legend in top-right corner. */
  const double leg_x = width - 200.0;
  f << "<g transform=\"translate(" << leg_x << ", 15)\">\n"
    << "  <rect width=\"180\" height=\"75\" fill=\"#f8f9fa\" stroke=\"#dee2e6\" rx=\"6\"/>\n"
    << "  <line x1=\"15\" y1=\"20\" x2=\"45\" y2=\"20\" stroke=\"#2980b9\" "
       "stroke-width=\"3.5\"/>\n"
    << "  <text x=\"55\" y=\"24\" font-size=\"12\" fill=\"#2c3e50\">Input Boundary</text>\n"
    << "  <line x1=\"15\" y1=\"40\" x2=\"45\" y2=\"40\" stroke=\"#e74c3c\" "
       "stroke-width=\"2.5\"/>\n"
    << "  <text x=\"55\" y=\"44\" font-size=\"12\" fill=\"#2c3e50\">Skeleton Edge</text>\n"
    << "  <rect x=\"15\" y=\"52\" width=\"30\" height=\"12\" fill=\"#d4e6f1\" stroke=\"#7f8c8d\" "
       "stroke-width=\"1\"/>\n"
    << "  <text x=\"55\" y=\"63\" font-size=\"12\" fill=\"#2c3e50\">Output Face</text>\n"
    << "</g>\n";

  f << "</svg>\n";
  f.close();
  std::cout << "SVG picture written to: " << file_path << "\n";
}

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

static int test_debug_level()
{
  return (get_debug_output_mode() == DebugOutputMode::STEPS) ? 1 : 0;
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
      area += (p1.x * p2.y - p2.x * p1.y);
    }
    total += 0.5 * area;
  }
  return total;
}

static void print_test_output(const char *test_name,
                              const StraightSkeletonInput &input,
                              const StraightSkeletonOutput &output)
{
  if (get_debug_output_mode() != DebugOutputMode::OUTPUT) {
    return;
  }

  write_debug_svg(test_name, input, output);

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
      area += (p1.x * p2.y - p2.x * p1.y);
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
 * Single isolated line segment.
 *
 * Vertices:
 *   v0 = (0, 0)
 *   v1 = (4, 0)
 * Edge:
 *   (0, 1)
 */
TEST(straight_skeleton, SingleSegment)
{
  const char *spec = R"(2 1 0
0.0 0.0
4.0 0.0
0 1
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "SingleSegment";

  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 10, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.events_processed_num, 0);

  StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  const Array<TriangleEvent> events = straight_skeleton_initial_events(input);
  EXPECT_TRUE(events.is_empty());
}

/**
 * Equilateral triangle.
 *
 * The straight skeleton of a triangle is the single point where all three
 * angle bisectors meet (the incenter).  The output should be three triangles,
 * each sharing the incenter as one vertex and one side of the input as their base.
 *
 * Input:
 *   v0 = (0, 0)   v1 = (1, 0)   v2 = (0.5, sqrt(3)/2 ≈ 0.866)
 *
 * Expected:
 *   4 output vertices: the 3 originals plus the incenter at (0.5, 0.5/sqrt(3) ≈ 0.2887).
 *   3 output faces, each a triangle.
 */
TEST(straight_skeleton, Triangle)
{
  const char *spec = R"(3 0 1
0.0 0.0
1.0 0.0
0.5 0.8660254037844387
0 1 2
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "Triangle";

  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 10, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.edge_collapses_num, 1);

  StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  const Array<TriangleEvent> events = straight_skeleton_initial_events(input);
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].triangle_index, 0);
  EXPECT_EQ(events[0].type, TriangleEventType::EDGE_COLLAPSE);
  EXPECT_NEAR(events[0].time, std::sqrt(3.0) / 6.0, 1e-6);
  EXPECT_NEAR(events[0].event_pos.x, 0.5, 1e-5);
  EXPECT_NEAR(events[0].event_pos.y, std::sqrt(3.0) / 6.0, 1e-5);

#if 1 /* Output assertions for the completed straight skeleton. */
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
      /* Incenter of an equilateral triangle with side 1 is at (0.5, 1/sqrt(3)/2). */
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
#endif
}

/**
 * Mixture of connected line segments with free ends and closed faces.
 *
 * Features:
 * - Face 0 (Triangle): verts (0, 1, 2)
 * - Face 1 (Quad): verts (3, 4, 5, 6)
 * - Segment Chain 1: (7, 8) and (8, 9)
 *   End 7 is on the convex hull; end 9 is interior with a free end.
 * - Segment Chain 2: (10, 11) and (11, 12)
 *   End 10 is on the convex hull; end 12 is interior with a free end.
 *   Free ends 9 and 12 are both interior and connected by a Delaunay edge.
 * - Segment Chain 3: (13, 14) and (14, 15)
 *   Both ends (13 and 15) lie on the convex hull.
 */
TEST(straight_skeleton, MixedSegmentsAndFaces)
{
  const char *spec = R"(16 6 2
0.0 0.0
4.0 0.0
1.0 3.0
6.0 0.0
10.0 0.0
10.0 3.0
7.0 2.0
0.0 7.0
3.0 7.0
4.5 6.5
10.0 7.0
7.0 7.0
5.5 6.5
1.0 9.5
5.0 10.0
9.0 9.5
7 8
8 9
10 11
11 12
13 14
14 15
0 1 2
3 4 5 6
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "MixedSegmentsAndFaces";
  StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  const Array<TriangleEvent> events = straight_skeleton_initial_events(input);
  EXPECT_FALSE(events.is_empty());
  /* Earliest events are facing free ends 9 and 12 meeting in the middle. */
  EXPECT_NEAR(events[0].time, 0.395285, 1e-4);
  EXPECT_NEAR(events[0].event_pos.x, 5.0, 1e-4);
  EXPECT_NEAR(events[0].event_pos.y, 6.75, 1e-4);
  EXPECT_EQ(events[0].type, TriangleEventType::MULTI_SPLIT);

  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 10, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
  EXPECT_GT(stats.events_processed_num, 0);

  EXPECT_EQ(output.face_offsets.size(), 7);
  EXPECT_NEAR(total_faces_area(output), 14.5, 1e-4);
  /* The first 3 faces come from input face 0; the next 4 come from input face 1. */
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(output.face_orig[i], 0);
  }
  for (int i = 3; i < 7; i++) {
    EXPECT_EQ(output.face_orig[i], 1);
  }
}

/**
 * Initial Skeleton Graph verification.
 * Confirms that static input vertices and edges remain at t=0 in SkeletonGraph.
 */
TEST(straight_skeleton, SkeletonGraphInitial)
{
  /* Test 1: Single segment with 2 verts, 1 edge. */
  {
    const char *spec = R"(2 1 0
0.0 0.0
4.0 0.0
0 1
)";
    InputStorage storage;
    StraightSkeletonInput input = fill_input_from_string(spec, storage);
    input.label = "SkeletonGraphInitial_Segment";
    straight_skeleton_run_events_test(
        input, 0, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
    SkeletonGraph graph = straight_skeleton_initial_graph(input);

    EXPECT_EQ(graph.nodes.size(), 2);
    for (const int i : IndexRange(2)) {
      EXPECT_EQ(graph.nodes[i].id, i);
      EXPECT_EQ(graph.nodes[i].orig_vert, i);
      EXPECT_EQ(graph.nodes[i].time, 0.0);
      EXPECT_EQ(graph.nodes[i].pos.x, input.vert[i].x);
      EXPECT_EQ(graph.nodes[i].pos.y, input.vert[i].y);
      EXPECT_EQ(graph.nodes[i].incident_edges.size(), 1);
    }

    EXPECT_EQ(graph.edges.size(), 1);
    EXPECT_EQ(graph.edges[0].id, 0);
    EXPECT_EQ(graph.edges[0].node1, 0);
    EXPECT_EQ(graph.edges[0].node2, 1);
    EXPECT_EQ(graph.edges[0].orig_edge, 0);
    EXPECT_TRUE(graph.edges[0].is_input_boundary);
    EXPECT_EQ(graph.edges[0].time_start, 0.0);
    EXPECT_EQ(graph.edges[0].time_end, 0.0);
  }

  /* Test 2: Triangle with 3 verts, 1 face (3 boundary edges). */
  {
    const char *spec = R"(3 0 1
0.0 0.0
1.0 0.0
0.5 0.8660254037844387
0 1 2
)";
    InputStorage storage;
    StraightSkeletonInput input = fill_input_from_string(spec, storage);
    input.label = "SkeletonGraphInitial_Triangle";
    straight_skeleton_run_events_test(
        input, 0, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
    SkeletonGraph graph = straight_skeleton_initial_graph(input);

    EXPECT_EQ(graph.nodes.size(), 3);
    for (const int i : IndexRange(3)) {
      EXPECT_EQ(graph.nodes[i].id, i);
      EXPECT_EQ(graph.nodes[i].orig_vert, i);
      EXPECT_EQ(graph.nodes[i].time, 0.0);
      EXPECT_EQ(graph.nodes[i].incident_edges.size(), 2);
    }

    EXPECT_EQ(graph.edges.size(), 3);
    for (const int ei : IndexRange(3)) {
      EXPECT_EQ(graph.edges[ei].id, ei);
      EXPECT_TRUE(graph.edges[ei].is_input_boundary);
      EXPECT_EQ(graph.edges[ei].input_face, 0);
      EXPECT_EQ(graph.edges[ei].orig_edge, -1);
      EXPECT_EQ(graph.edges[ei].time_start, 0.0);
      EXPECT_EQ(graph.edges[ei].time_end, 0.0);
    }
  }
}

/**
 * Notched polygon from Figure 5 of Palfrader's straight skeleton paper.
 * A reflex vertex at the notch tip moves rightward toward the internal spoke
 * between the top-left and bottom-right vertices, causing a flip event.
 */
TEST(straight_skeleton, FlipEventNotchedPolygon)
{
  const char *spec = R"(7 0 1
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
  input.label = "FlipEventNotchedPolygon";
  StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  const Array<TriangleEvent> events = straight_skeleton_initial_events(input);
  const TriangleEvent *ev6 = nullptr;
  for (const TriangleEvent &ev : events) {
    if (ev.triangle_index == 6) {
      ev6 = &ev;
      break;
    }
  }
  ASSERT_NE(ev6, nullptr);
  /* Yellow triangle 6 has vertices (5, 1, 3). Edge 19 is (1 -> 3), sharing spoke [3, 1]
   * with adjacent triangle 7 (1, 2, 3). Vertex 5 (notch tip) moves over spoke [3, 1]. */
  EXPECT_EQ(ev6->type, TriangleEventType::FLIP);
  EXPECT_EQ(ev6->vert_index, 5);
  EXPECT_EQ(ev6->edge_index, 19);
  EXPECT_NEAR(ev6->time, 2.3914, 1e-3);
  EXPECT_NEAR(ev6->event_pos.x, 5.4222, 1e-3);
  EXPECT_NEAR(ev6->event_pos.y, 4.5000, 1e-3);

  /* Run events through the event loop until the FLIP event on spoke [3, 1] is processed.
   * Other event types (EDGE_COLLAPSE, MULTI_SPLIT) will print "yet to be implemented"
   * and be skipped for this isolated flip test. */
  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 4, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/false);
  EXPECT_GE(stats.flips_num, 1);

  /* After the flip event, spoke [3, 1] is replaced by the new diagonal [5, 2].
   * Yellow triangle (5, 1, 3) and adjacent triangle (1, 2, 3) are replaced by
   * (5, 1, 2) and (5, 2, 3). */
  const Vector<int3> active_tris = straight_skeleton_active_triangles_test(
      input, 4, /*dbg_level=*/0, /*handle_edge_collapses=*/false);
  bool has_edge_3_1 = false;
  bool has_edge_5_2 = false;
  bool has_tri_5_1_2 = false;
  bool has_tri_5_2_3 = false;
  for (const int3 &t : active_tris) {
    const int a = t[0];
    const int b = t[1];
    const int c = t[2];
    auto has_edge = [&](int u, int v) {
      return (a == u && b == v) || (b == u && c == v) || (c == u && a == v) ||
             (a == v && b == u) || (b == v && c == u) || (c == v && a == u);
    };
    if (has_edge(3, 1)) {
      has_edge_3_1 = true;
    }
    if (has_edge(5, 2)) {
      has_edge_5_2 = true;
    }
    auto is_tri = [&](int u, int v, int w) {
      return (a == u && b == v && c == w) || (a == v && b == w && c == u) ||
             (a == w && b == u && c == v) || (a == u && b == w && c == v) ||
             (a == w && b == v && c == u) || (a == v && b == u && c == w);
    };
    if (is_tri(5, 1, 2)) {
      has_tri_5_1_2 = true;
    }
    if (is_tri(5, 2, 3)) {
      has_tri_5_2_3 = true;
    }
  }
  EXPECT_FALSE(has_edge_3_1);
  EXPECT_TRUE(has_edge_5_2);
  EXPECT_TRUE(has_tri_5_1_2);
  EXPECT_TRUE(has_tri_5_2_3);
}

TEST(straight_skeleton, NotchedPolygonFull)
{
  const char *spec = R"(7 0 1
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

  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 20, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
  EXPECT_GT(stats.events_processed_num, 0);

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.vert.size(), 13);
  EXPECT_EQ(output.edge.size(), 20);
  EXPECT_EQ(output.face_offsets.size(), 7);
  EXPECT_NEAR(total_faces_area(output), 71.1, 1e-4);
  for (const int fi : output.face_offsets.index_range()) {
    EXPECT_EQ(output.face_orig[fi], 0);
  }
}

TEST(straight_skeleton, Square)
{
  const char *spec = R"(4 0 1
0.0 0.0
2.0 0.0
2.0 2.0
0.0 2.0
0 1 2 3
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "Square";

  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 10, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
  EXPECT_GT(stats.events_processed_num, 0);

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
  const char *spec = R"(6 0 1
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

  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 20, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
  EXPECT_GT(stats.events_processed_num, 0);

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
  const char *spec = R"(4 0 1
0.0 0.0
4.0 0.0
4.0 2.0
0.0 2.0
0 1 2 3
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "RectangleEdgeCollapse";

  /* Initial events should include edge collapse events for the shorter vertical edges. */
  const Array<TriangleEvent> events = straight_skeleton_initial_events(input);
  ASSERT_GE(events.size(), 1);

  /* Run the event loop with edge collapses enabled. Both width-2 edges collapse at t = 1.0. */
  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 10, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.events_processed_num, 1);
  EXPECT_EQ(stats.edge_collapses_num, 3);

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
  const char *spec = R"(4 0 1
0.0 0.0
8.0 0.0
8.0 2.0
0.0 2.0
0 1 2 3
)";

  InputStorage storage;
  StraightSkeletonInput input = fill_input_from_string(spec, storage);
  input.label = "ElongatedRectangleInfiniteSpeed";

  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 10, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.events_processed_num, 1);
  EXPECT_EQ(stats.edge_collapses_num, 3);

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
  const char *spec = R"(6 0 1
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

  const Array<TriangleEvent> events = straight_skeleton_initial_events(input);
  ASSERT_FALSE(events.is_empty());

  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 20, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.multi_splits_num, 1);
  EXPECT_EQ(stats.edge_collapses_num, 2);

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
  const char *spec = R"(20 0 1
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

  const Array<TriangleEvent> events = straight_skeleton_initial_events(input);
  ASSERT_FALSE(events.is_empty());

  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 50, /*dbg_level=*/test_debug_level(), /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.flips_num, 2);
  EXPECT_EQ(stats.edge_collapses_num, 22);
  EXPECT_EQ(stats.splits_num, 3);
  EXPECT_EQ(stats.multi_splits_num, 1);

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);
  EXPECT_EQ(output.vert.size(), 46);
  EXPECT_EQ(output.edge.size(), 73);
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
  const char *spec = R"(6 0 1
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
  const char *spec = R"(7 0 1
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
  Array<int> face_offsets_buf = {0, 4};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.face_offsets_buf = face_offsets_buf;
  storage.face_vert_indices.extend(face_verts.as_span());

  StraightSkeletonInput input;
  input.vert = storage.vert.as_span();
  input.face_offsets = OffsetIndices<int>(storage.face_offsets_buf.as_span());
  input.face_vert_indices = storage.face_vert_indices.as_span();
  input.offset = 1.0;
  input.segments = 1;
  input.label = "OffsetSquarePartial";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* 4 side-strip trapezoids + 1 inner square. */
  EXPECT_EQ(output.face_offsets.size(), 5);

  /* Cut vertices at the offset boundary should be at vert_time == offset == 1.0.
   * (The skeleton apex at t = 5 also has vert_orig == -1, so filter by vert_time.) */
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
  Array<int> face_offsets_buf = {0, 4};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.face_offsets_buf = face_offsets_buf;
  storage.face_vert_indices.extend(face_verts.as_span());

  StraightSkeletonInput input;
  input.vert = storage.vert.as_span();
  input.face_offsets = OffsetIndices<int>(storage.face_offsets_buf.as_span());
  input.face_vert_indices = storage.face_vert_indices.as_span();
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
  Array<int> face_offsets_buf = {0, 4};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.face_offsets_buf = face_offsets_buf;
  storage.face_vert_indices.extend(face_verts.as_span());

  StraightSkeletonInput input;
  input.vert = storage.vert.as_span();
  input.face_offsets = OffsetIndices<int>(storage.face_offsets_buf.as_span());
  input.face_vert_indices = storage.face_vert_indices.as_span();
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
  Array<int> face_offsets_buf = {0, 4};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.face_offsets_buf = face_offsets_buf;
  storage.face_vert_indices.extend(face_verts.as_span());

  StraightSkeletonInput input;
  input.vert = storage.vert.as_span();
  input.face_offsets = OffsetIndices<int>(storage.face_offsets_buf.as_span());
  input.face_vert_indices = storage.face_vert_indices.as_span();
  input.offset = 2.0;
  input.segments = 1;
  input.label = "OffsetRectangle";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_EQ(output.face_offsets.size(), 5);

  /* Cut vertices at the offset boundary should have vert_time ≈ 2.0.
   * (Interior skeleton nodes with vert_orig == -1 exist at higher times.) */
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
  Array<double2> verts = {
      {0.0, 0.0}, {4.0, 0.0}, {4.0, 2.0}, {2.0, 2.0}, {2.0, 4.0}, {0.0, 4.0}};
  Array<int> face_offsets_buf = {0, 6};
  Array<int> face_verts = {0, 1, 2, 3, 4, 5};

  InputStorage storage;
  storage.vert = verts;
  storage.face_offsets_buf = face_offsets_buf;
  storage.face_vert_indices.extend(face_verts.as_span());

  StraightSkeletonInput input;
  input.vert = storage.vert.as_span();
  input.face_offsets = OffsetIndices<int>(storage.face_offsets_buf.as_span());
  input.face_vert_indices = storage.face_vert_indices.as_span();
  input.offset = 0.5;
  input.segments = 1;
  input.label = "OffsetLShape";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  EXPECT_EQ(output.face_offsets.size(), 7); /* 6 side-strips + 1 inner L-shape. */

  EXPECT_NEAR(total_faces_area(output), 12.0, 1e-6);
}

/**
 * Two-segment offset of a 10x10 square with offset = 2.
 * effective_offset = 2 (< max_collapse = 5).
 * Band 0: t in [0, 1]  — 4 side-strip trapezoids + 1 inner square.
 * Band 1: t in [1, 2]  — 4 side-strip trapezoids + 1 inner square.
 * Total: 10 faces.  Total area = 100 (the whole square).
 */
TEST(straight_skeleton, OffsetSquare_TwoBands)
{
  Array<double2> verts = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}};
  Array<int> face_offsets_buf = {0, 4};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.face_offsets_buf = face_offsets_buf;
  storage.face_vert_indices.extend(face_verts.as_span());

  StraightSkeletonInput input;
  input.vert = storage.vert.as_span();
  input.face_offsets = OffsetIndices<int>(storage.face_offsets_buf.as_span());
  input.face_vert_indices = storage.face_vert_indices.as_span();
  input.offset = 2.0;
  input.segments = 2;
  input.label = "OffsetSquare_TwoBands";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Band 0 (t=0..1): 4 side-strip trapezoids.
   * Band 1 (t=1..2): 4 side-strip trapezoids + 1 inner square.
   * Total = 9 faces. */
  EXPECT_EQ(output.face_offsets.size(), 9);

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
 * Band 0: t in [0, 2.5]  — 4 side-strip faces + 1 inner square at 2.5.
 * Band 1: t in [2.5, 5]  — 4 narrower side-strip faces + apex.
 * Total: 10 faces.  Total area = 100.
 */
TEST(straight_skeleton, FullSkeletonWithSegments)
{
  Array<double2> verts = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}};
  Array<int> face_offsets_buf = {0, 4};
  Array<int> face_verts = {0, 1, 2, 3};

  InputStorage storage;
  storage.vert = verts;
  storage.face_offsets_buf = face_offsets_buf;
  storage.face_vert_indices.extend(face_verts.as_span());

  StraightSkeletonInput input;
  input.vert = storage.vert.as_span();
  input.face_offsets = OffsetIndices<int>(storage.face_offsets_buf.as_span());
  input.face_vert_indices = storage.face_vert_indices.as_span();
  /* Default input.offset = infinity; segments = 2 divides the full skeleton into 2 bands. */
  input.segments = 2;
  input.label = "FullSkeletonWithSegments";

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  print_test_output(input.label, input, output);

  /* Band 0 (t=0..2.5): 4 side-strip trapezoids.
   * Band 1 (t=2.5..5): 4 side-strip triangles converging to the apex.
   *   The apex inner polygon degenerates to a point (area ≈ 0) and is silently omitted.
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

/** \} */

} /* namespace blender::straightskeleton::test */
