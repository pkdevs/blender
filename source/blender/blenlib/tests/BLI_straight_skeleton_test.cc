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
  int nverts, nedges, nfaces;
  hdrss >> nverts >> nedges >> nfaces;

  r_storage.vert.reinitialize(nverts);
  r_storage.edge.reinitialize(nedges);
  r_storage.face_offsets_buf.reinitialize(nfaces + 1);

  int i = 0;
  while (i < nverts && getline(ss, line)) {
    std::istringstream iss(line);
    double x, y;
    iss >> x >> y;
    r_storage.vert[i] = double2(x, y);
    i++;
  }

  i = 0;
  while (i < nedges && getline(ss, line)) {
    std::istringstream ess(line);
    int e0, e1;
    ess >> e0 >> e1;
    r_storage.edge[i] = int2(e0, e1);
    i++;
  }

  i = 0;
  while (i < nfaces && getline(ss, line)) {
    r_storage.face_offsets_buf[i] = int(r_storage.face_vert_indices.size());
    std::istringstream fss(line);
    int v;
    while (fss >> v) {
      r_storage.face_vert_indices.append(v);
    }
    i++;
  }
  r_storage.face_offsets_buf[nfaces] = int(r_storage.face_vert_indices.size());

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
  StraightSkeletonOutput output = straight_skeleton_calc(input);

#if DO_DEBUG_DRAW
  write_debug_html(input, output, "/tmp/skel_debug_draw.html");
#endif

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
  int new_verts = 0;
  for (const int vi : IndexRange(output.vert.size())) {
    if (output.vert_orig[vi] == -1) {
      new_verts++;
      /* Incenter of an equilateral triangle with side 1 is at (0.5, 1/sqrt(3)/2). */
      EXPECT_NEAR(output.vert[vi].x, 0.5, 1e-6);
      EXPECT_NEAR(output.vert[vi].y, 0.5 / 1.7320508075688772, 1e-6);
    }
  }
  EXPECT_EQ(new_verts, 1);

  /* face_orig: every output face should derive from input face 0. */
  EXPECT_EQ(output.face_orig.size(), int64_t(output.face_offsets.size()));
  for (const int fi : IndexRange(output.face_orig.size())) {
    EXPECT_EQ(output.face_orig[fi], 0);
  }
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
  StraightSkeletonOutput output = straight_skeleton_calc(input);

#if DO_DEBUG_DRAW
  write_debug_html(input, output, "/tmp/skel_debug_draw.html");
#endif
}

/** \} */

}  // namespace blender::straightskeleton::test
