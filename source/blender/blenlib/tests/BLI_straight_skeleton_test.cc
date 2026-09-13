/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <fstream>
#include <iostream>
#include <sstream>

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_straight_skeleton.hh"
#include "BLI_vector.hh"

/* Set to 1 to write an HTML debug drawing for each test to /tmp/skel_debug_draw.html. */
#define DO_DEBUG_DRAW 0

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

#if DO_DEBUG_DRAW
static void write_debug_html(const StraightSkeletonInput &input,
                             const StraightSkeletonOutput &output,
                             const char *path)
{
  /* Compute bounding box of input vertices for the SVG viewport. */
  double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30;
  for (const double2 &v : input.vert) {
    xmin = std::min(xmin, v.x);
    xmax = std::max(xmax, v.x);
    ymin = std::min(ymin, v.y);
    ymax = std::max(ymax, v.y);
  }
  for (const double2 &v : output.vert) {
    xmin = std::min(xmin, v.x);
    xmax = std::max(xmax, v.x);
    ymin = std::min(ymin, v.y);
    ymax = std::max(ymax, v.y);
  }
  const double pad = std::max(xmax - xmin, ymax - ymin) * 0.1 + 0.01;
  xmin -= pad;
  xmax += pad;
  ymin -= pad;
  ymax += pad;
  const double width = 600.0;
  const double height = 600.0;
  const double sx = width / (xmax - xmin);
  const double sy = height / (ymax - ymin);

  auto tx = [&](double x) { return (x - xmin) * sx; };
  /* SVG y-axis is flipped vs. math convention. */
  auto ty = [&](double y) { return height - (y - ymin) * sy; };

  std::ofstream f(path);
  f << "<!DOCTYPE html>\n<html><body>\n";
  f << "<svg width=\"" << width << "\" height=\"" << height << "\" "
    << "style=\"border:1px solid black\">\n";

  /* Draw output faces in light gray. */
  const Span<int> fvi = output.face_vert_indices;
  for (const int fi : output.face_offsets.index_range()) {
    const IndexRange r = output.face_offsets[fi];
    f << "<polygon points=\"";
    for (const int idx : r) {
      f << tx(output.vert[fvi[idx]].x) << "," << ty(output.vert[fvi[idx]].y) << " ";
    }
    f << "\" fill=\"#e0e8f0\" stroke=\"#6688aa\" stroke-width=\"1\"/>\n";
  }

  /* Draw input boundary in dark blue. */
  const Span<int> ivi = input.face_vert_indices;
  for (const int fi : input.face_offsets.index_range()) {
    const IndexRange r = input.face_offsets[fi];
    for (int k = 0; k < int(r.size()); k++) {
      const int a = ivi[r[k]];
      const int b = ivi[r[(k + 1) % int(r.size())]];
      f << "<line x1=\"" << tx(input.vert[a].x) << "\" y1=\"" << ty(input.vert[a].y) << "\""
        << " x2=\"" << tx(input.vert[b].x) << "\" y2=\"" << ty(input.vert[b].y) << "\""
        << " stroke=\"#0000cc\" stroke-width=\"2\"/>\n";
    }
  }

  /* Draw input line segments in dark blue. */
  for (const int2 &e : input.edge) {
    f << "<line x1=\"" << tx(input.vert[e[0]].x) << "\" y1=\"" << ty(input.vert[e[0]].y) << "\""
      << " x2=\"" << tx(input.vert[e[1]].x) << "\" y2=\"" << ty(input.vert[e[1]].y) << "\""
      << " stroke=\"#0000cc\" stroke-width=\"2\"/>\n";
  }

  /* Draw output edges in red (skeleton + side edges). */
  for (const int2 &e : output.edge) {
    f << "<line x1=\"" << tx(output.vert[e[0]].x) << "\" y1=\"" << ty(output.vert[e[0]].y) << "\""
      << " x2=\"" << tx(output.vert[e[1]].x) << "\" y2=\"" << ty(output.vert[e[1]].y) << "\""
      << " stroke=\"#cc0000\" stroke-width=\"1\"/>\n";
  }

  /* Label output vertices. */
  for (const int vi : IndexRange(output.vert.size())) {
    f << "<circle cx=\"" << tx(output.vert[vi].x) << "\" cy=\"" << ty(output.vert[vi].y)
      << "\" r=\"3\" fill=\"#cc0000\"/>\n";
    f << "<text x=\"" << (tx(output.vert[vi].x) + 4) << "\" y=\"" << ty(output.vert[vi].y)
      << "\" font-size=\"10\" fill=\"#330000\">" << vi << "</text>\n";
  }

  f << "</svg>\n</body></html>\n";
  f.close();
  std::cout << "Debug drawing written to " << path << "\n";
}
#endif /* DO_DEBUG_DRAW */

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
      input, 10, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.events_processed_num, 0);

  StraightSkeletonOutput output = straight_skeleton_calc(input);

  const Array<TriangleEvent> events = straight_skeleton_initial_events(input);
  EXPECT_TRUE(events.is_empty());

#if DO_DEBUG_DRAW
  write_debug_html(input, output, "/tmp/skel_debug_draw.html");
#endif
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
      input, 10, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.edge_collapses_num, 1);

  StraightSkeletonOutput output = straight_skeleton_calc(input);

  const Array<TriangleEvent> events = straight_skeleton_initial_events(input);
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].triangle_index, 0);
  EXPECT_EQ(events[0].type, TriangleEventType::EDGE_COLLAPSE);
  EXPECT_NEAR(events[0].time, std::sqrt(3.0) / 6.0, 1e-6);
  EXPECT_NEAR(events[0].event_pos.x, 0.5, 1e-5);
  EXPECT_NEAR(events[0].event_pos.y, std::sqrt(3.0) / 6.0, 1e-5);

#if DO_DEBUG_DRAW
  write_debug_html(input, output, "/tmp/skel_debug_draw.html");
#endif

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

  const Array<TriangleEvent> events = straight_skeleton_initial_events(input);
  EXPECT_FALSE(events.is_empty());
  /* Earliest events are facing free ends 9 and 12 meeting in the middle. */
  EXPECT_NEAR(events[0].time, 0.395285, 1e-4);
  EXPECT_NEAR(events[0].event_pos.x, 5.0, 1e-4);
  EXPECT_NEAR(events[0].event_pos.y, 6.75, 1e-4);
  EXPECT_EQ(events[0].type, TriangleEventType::MULTI_SPLIT);

  const EventLoopStats stats = straight_skeleton_run_events_test(
      input, 10, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
  EXPECT_GT(stats.events_processed_num, 0);

#if DO_DEBUG_DRAW
  write_debug_html(input, output, "/tmp/skel_debug_draw.html");
#endif
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
    straight_skeleton_run_events_test(input, 0, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
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
    straight_skeleton_run_events_test(input, 0, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
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
      input, 4, /*dbg_level=*/1, /*handle_edge_collapses=*/false);
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
      input, 20, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
  EXPECT_GT(stats.events_processed_num, 0);

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  EXPECT_EQ(output.vert.size(), 13);
  EXPECT_EQ(output.edge.size(), 20);
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
      input, 10, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
  EXPECT_GT(stats.events_processed_num, 0);

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  EXPECT_EQ(output.vert.size(), 5);
  EXPECT_EQ(output.edge.size(), 8);
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
      input, 20, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
  EXPECT_GT(stats.events_processed_num, 0);

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  EXPECT_EQ(output.vert.size(), 9);
  EXPECT_EQ(output.edge.size(), 14);
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
      input, 10, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.events_processed_num, 1);
  EXPECT_EQ(stats.edge_collapses_num, 3);

  /* Full straight skeleton output: 4 original corners + 2 ridge vertices = 6 vertices. */
  const StraightSkeletonOutput output = straight_skeleton_calc(input);
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
      input, 10, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.events_processed_num, 1);
  EXPECT_EQ(stats.edge_collapses_num, 3);

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
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
      input, 20, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.multi_splits_num, 1);
  EXPECT_EQ(stats.edge_collapses_num, 2);

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  EXPECT_EQ(output.vert.size(), 9);
  EXPECT_EQ(output.edge.size(), 14);
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
      input, 50, /*dbg_level=*/1, /*handle_edge_collapses=*/true);
  EXPECT_EQ(stats.flips_num, 2);
  EXPECT_EQ(stats.edge_collapses_num, 22);
  EXPECT_EQ(stats.splits_num, 3);
  EXPECT_EQ(stats.multi_splits_num, 1);

  const StraightSkeletonOutput output = straight_skeleton_calc(input);
  EXPECT_EQ(output.vert.size(), 46);
  EXPECT_EQ(output.edge.size(), 73);
  for (const double2 &v : output.vert) {
    EXPECT_GE(v.y, -1e-6);
  }
}

/** \} */

} /* namespace blender::straightskeleton::test */
