/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Straight Skeleton implementation.
 */

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

#include "BLI_array.hh"
#include "BLI_delaunay_2d.hh"
#include "BLI_map.hh"
#include "BLI_math_boolean.hh"
#include "BLI_set.hh"
#include "BLI_straight_skeleton.hh"
#include "BLI_vector.hh"

namespace blender::straightskeleton {

bool debug_new_file = true;
const char *debug_html_filepath = "/tmp/skel_tri_debug.html";

void debug_reset_file()
{
  debug_new_file = true;
}

/** Return index of next edge after \a edge_index in #TriangleEdges. */
static inline int next_triangle_edge(int edge_index)
{
  edge_index += edge_index % 3 == 2 ? -2 : 1;
  return edge_index;
}

/** Return index of previous edge before \a edge_index in #TriangleEdges. */
[[maybe_unused]]
static inline int prev_triangle_edge(int edge_index)
{
  edge_index += edge_index % 3 == 0 ? 2 : 1;
  return edge_index;
}

/**
 * Represents a set of triangles by means of half-edges.
 * Each edge of a triangle has zero or one paired edge,
 * which is an edge in another triangle going in the opposite direction.
 * Each edge has an index, and given that index we can find
 * the start and end vertices, the paired edge, and the triangle it belongs to.
 * Vertices are represented by int indices; another structure will hold the coordinates.
 *
 * The implementation uses a concatenation of the three edges of each triangle,
 * one after another. We only need store the start vertex index for each edge, since
 * the end vertex of edge i is the start vertex of edge i+1 (modulo 3 within the triangle).
 *
 * If the start vertex of a triangle is -1, then the triangle is deleted.
 * The third vertex of an unbounded triangle is -1, representing the "ghost" vertex at
 * infinity (used in the straight skeleton algorithm).
 *
 * Convention for unbounded triangles:
 *   For interior hull half-edge ei going u->v, the unbounded triangle is stored as
 *   vertices (v, u, -1), giving three half-edges:
 *     base:  v->u  (index 3*ti+0) -- paired with ei.
 *     right: u->-1 (index 3*ti+1) -- boundary into infinity, no pair.
 *     left:  -1->v (index 3*ti+2) -- boundary into infinity, no pair.
 */
class TriangleEdges {
 public:
  TriangleEdges() = default;

  void reserve_triangles(int triangles_num)
  {
    start_.reserve(3 * triangles_num);
    paired_.reserve(3 * triangles_num);
  }

  void add_triangle(int v0, int v1, int v2)
  {
    start_.append(v0);
    start_.append(v1);
    start_.append(v2);
    paired_.append(-1);
    paired_.append(-1);
    paired_.append(-1);
  }

  void set_paired(int edge_index, int paired_index)
  {
    paired_[edge_index] = paired_index;
  }

  void set_start_vert(int edge_index, int vert)
  {
    start_[edge_index] = vert;
  }

  void set_triangle_verts(int ti, int v0, int v1, int v2)
  {
    start_[3 * ti + 0] = v0;
    start_[3 * ti + 1] = v1;
    start_[3 * ti + 2] = v2;
  }

  int start_vert(int edge_index) const
  {
    return start_[edge_index];
  }

  int end_vert(int edge_index) const
  {
    return start_[next_triangle_edge(edge_index)];
  }

  int paired_edge(int edge_index) const
  {
    return paired_[edge_index];
  }

  int triangle(int edge_index) const
  {
    return edge_index / 3;
  }

  int triangles_num() const
  {
    return int(start_.size()) / 3;
  }

  int edges_num() const
  {
    return int(start_.size());
  }

  void mark_triangle_deleted(int triangle_index)
  {
    start_[3 * triangle_index] = -1;
  }

  bool is_triangle_deleted(int triangle_index) const
  {
    return start_[3 * triangle_index] == -1;
  }

  /** Returns true if triangle \a ti is an unbounded (ghost) triangle. */
  bool is_triangle_unbounded(int triangle_index) const
  {
    /* Third vertex is the ghost vertex at infinity. */
    return start_[3 * triangle_index + 2] == -1;
  }

 private:
  Vector<int> start_;
  Vector<int> paired_;
};

/* -------------------------------------------------------------------- */
/** \name Contour edges and sequences
 * \{ */

/**
 * A directed edge on the wavefront contour.
 */
struct ContourEdge {
  /** Directed start vertex in [0, 2N - 1]. */
  int start{-1};
  /** Directed end vertex in [0, 2N - 1]. */
  int end{-1};
  /** Corresponding input edge index in input.edge, or -1 if none/cap. */
  int input_edge{-1};
  /** Corresponding input face index in input.face_offsets, or -1 if from input.edge or cap. */
  int input_face{-1};
  /** 0 for Side 0 (forward / inner), 1 for Side 1 (backward / outer), -1 for cap. */
  int side{-1};
  /** Next contour edge in the wavefront loop. */
  int next{-1};
  /** Previous contour edge in the wavefront loop. */
  int prev{-1};
};

struct ContourSequences {
  Vector<ContourEdge> edges;
  /** Map from directed vertex pair (start, end) to contour edge index. */
  Map<int2, int> edge_by_verts;
  /** Loops of contour edge indices. */
  Vector<Vector<int>> loops;
};

/** Print a description of \a contours to \a os. */
static void dump_contours(const ContourSequences &contours, std::ostream &os = std::cout)
{
  os << "ContourSequences: " << contours.edges.size() << " edges, " << contours.loops.size()
     << " loops\n";
  for (const int li : contours.loops.index_range()) {
    const Vector<int> &loop = contours.loops[li];
    os << "  loop[" << li << "]: ";
    for (const int cei : loop) {
      const ContourEdge &ce = contours.edges[cei];
      os << ce.start << "->" << ce.end;
      if (ce.side == -1) {
        os << "(cap) ";
      }
      else {
        os << "(s" << ce.side << ") ";
      }
    }
    os << "\n";
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug output
 * \{ */

/** Print a description of \a tri to \a os. */
static void dump_triangles(const TriangleEdges &tri, std::ostream &os = std::cout)
{
  const int triangles_num = tri.triangles_num();
  os << "TriangleEdges: " << triangles_num << " triangles, " << tri.edges_num() << " half-edges\n";
  for (const int ti : IndexRange(triangles_num)) {
    const int e0 = 3 * ti;
    const int e1 = e0 + 1;
    const int e2 = e0 + 2;
    const int v0 = tri.start_vert(e0);
    const int v1 = tri.start_vert(e1);
    const int v2 = tri.start_vert(e2);
    const bool is_unbounded = tri.is_triangle_unbounded(ti);
    const bool is_deleted = (v0 == -1);
    os << "  tri[" << ti << "]"
       << (is_deleted   ? " [DELETED]" :
           is_unbounded ? " [UNBOUNDED]" :
                          "")
       << ": verts=(" << v0 << "," << v1 << "," << v2 << ")"
       << "  edges=(" << e0 << "," << e1 << "," << e2 << ")"
       << "  paired=(" << tri.paired_edge(e0) << "," << tri.paired_edge(e1) << ","
       << tri.paired_edge(e2) << ")\n";
  }
}

/**
 * Write an SVG file at \a path showing the triangulation described by \a tri and \a vert_pos.
 *
 * Bounded triangles are filled light blue, labelled at the centroid.
 * Unbounded triangles are labelled outside the base edge.
 * Each geometric edge is drawn once; half-edge indices are shown on each side, offset by a
 * fixed perpendicular distance so labels on opposite sides never overlap the edge.
 * Wings of unbounded triangles are drawn as short dashed stubs along the exterior angle
 * bisector at each hull vertex.
 */
static Array<double2> compute_cosmetic_displacements(const Span<double2> vert_pos,
                                                     const ContourSequences &contours,
                                                     double scale)
{
  const int verts_num = int(vert_pos.size());
  Array<double2> accum(verts_num, double2(0.0, 0.0));

  for (const ContourEdge &ce : contours.edges) {
    if (ce.side == -1) {
      /* Skip 0-length cap edges (normal is undefined). */
      continue;
    }
    const int u = ce.start;
    const int v = ce.end;
    if (u < 0 || u >= verts_num || v < 0 || v >= verts_num) {
      continue;
    }
    /* Directed vector in SVG pixels (SVG y is flipped relative to math y). */
    const double dx_s = (vert_pos[v].x - vert_pos[u].x) * scale;
    const double dy_s = -(vert_pos[v].y - vert_pos[u].y) * scale;
    const double dist_s = std::sqrt(dx_s * dx_s + dy_s * dy_s);
    if (dist_s < 1e-6) {
      continue;
    }
    /* Left-perpendicular in SVG pixels: (dy_s, -dx_s) / dist_s.
     * Points to the left of the directed edge in math coordinates (into the triangulation area).
     */
    const double px = dy_s / dist_s;
    const double py = -dx_s / dist_s;
    accum[u] += double2(px, py);
    accum[v] += double2(px, py);
  }

  Array<double2> offsets(verts_num, double2(0.0, 0.0));
  const double disp_px = 7.0; /* cosmetic offset in SVG pixels */
  for (const int vi : IndexRange(verts_num)) {
    const double length = std::sqrt(accum[vi].x * accum[vi].x + accum[vi].y * accum[vi].y);
    if (length > 1e-6) {
      offsets[vi] = (accum[vi] / length) * disp_px;
    }
  }
  return offsets;
}

/**
 * Write an SVG file at \a path showing the triangulation described by \a tri and \a vert_pos.
 *
 * Bounded triangles are filled light blue, labelled at the centroid.
 * Unbounded triangles are labelled outside the base edge.
 * Each geometric edge is drawn once; half-edge indices are shown on each side, offset by a
 * fixed perpendicular distance so labels on opposite sides never overlap the edge.
 * Wings of unbounded triangles are drawn as short dashed stubs along the exterior angle
 * bisector at each hull vertex.
 */
static void draw_triangulation_svg(const TriangleEdges &tri,
                                   const Span<double2> vert_pos,
                                   const StraightSkeletonInput &input,
                                   const ContourSequences &contours,
                                   const char *path = nullptr)
{
  const int verts_num = int(vert_pos.size());
  const int triangles_num = tri.triangles_num();
  const int edges_num = tri.edges_num();

  /* Bounding box across all input vertices. */
  double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30;
  for (const double2 &v : input.vert) {
    xmin = std::min(xmin, v.x);
    xmax = std::max(xmax, v.x);
    ymin = std::min(ymin, v.y);
    ymax = std::max(ymax, v.y);
  }
  const double range = std::max(xmax - xmin, ymax - ymin);
  const double pad = range * 0.25 + 0.5;

  const double cx_mid = 0.5 * (xmin + xmax);
  const double cy_mid = 0.5 * (ymin + ymax);
  const double half_range = 0.5 * std::max(xmax - xmin, ymax - ymin) + pad;
  xmin = cx_mid - half_range;
  xmax = cx_mid + half_range;
  ymin = cy_mid - half_range;
  ymax = cy_mid + half_range;

  const double svgw = 700.0;
  const double svgh = 700.0;
  const double scale = svgw / (xmax - xmin);

  /* Map math coordinates to SVG coordinates (SVG y-axis is flipped). */
  auto tx = [&](double x) { return (x - xmin) * scale; };
  auto ty = [&](double y) { return svgh - (y - ymin) * scale; };

  /* Compute cosmetic displacements so duplicate vertices don't completely overlap. */
  const Array<double2> offsets = compute_cosmetic_displacements(vert_pos, contours, scale);
  Array<double2> vert_svg_pos(verts_num);
  for (const int vi : IndexRange(verts_num)) {
    vert_svg_pos[vi] = double2(tx(vert_pos[vi].x) + offsets[vi].x,
                               ty(vert_pos[vi].y) + offsets[vi].y);
  }

  /* Hull adjacency maps: for each hull vertex u: next_hull[u] = v means base went v -> u. */
  Map<int, int> next_hull;
  Map<int, int> prev_hull;
  for (const int ti : IndexRange(triangles_num)) {
    if (!tri.is_triangle_unbounded(ti) || tri.is_triangle_deleted(ti)) {
      continue;
    }
    const int e0 = 3 * ti;
    const int v = tri.start_vert(e0);
    const int u = tri.start_vert(e0 + 1);
    if (u >= 0 && v >= 0) {
      next_hull.add_overwrite(u, v);
      prev_hull.add_overwrite(v, u);
    }
  }

  auto stub_dir_svg = [&](int u) -> std::pair<double, double> {
    const int *nxt = next_hull.lookup_ptr(u);
    const int *prv = prev_hull.lookup_ptr(u);
    double2 sum{0.0, 0.0};
    if (nxt && *nxt >= 0 && *nxt < verts_num) {
      const double2 d = vert_svg_pos[*nxt] - vert_svg_pos[u];
      const double dist = std::sqrt(d.x * d.x + d.y * d.y);
      if (dist > 1e-14) {
        sum.x += d.x / dist;
        sum.y += d.y / dist;
      }
    }
    if (prv && *prv >= 0 && *prv < verts_num) {
      const double2 d = vert_svg_pos[*prv] - vert_svg_pos[u];
      const double dist = std::sqrt(d.x * d.x + d.y * d.y);
      if (dist > 1e-14) {
        sum.x += d.x / dist;
        sum.y += d.y / dist;
      }
    }
    double bx = -sum.x;
    double by = -sum.y;
    const double bdist = std::sqrt(bx * bx + by * by);
    if (bdist < 1e-14) {
      if (nxt && *nxt >= 0 && *nxt < verts_num) {
        const double2 d = vert_svg_pos[*nxt] - vert_svg_pos[u];
        bx = d.y;
        by = -d.x;
      }
    }
    else {
      bx /= bdist;
      by /= bdist;
    }
    return {bx, by};
  };

  const double ray_length = 45.0; /* wing stub length in pixels */
  const double label_off_px = 12.0;

  auto tri_centroid = [&](int ti) -> double2 {
    const int e0 = 3 * ti;
    double2 c{0.0, 0.0};
    int verts_count = 0;
    for (const int k : IndexRange(3)) {
      const int v = tri.start_vert(e0 + k);
      if (v >= 0 && v < verts_num) {
        c += vert_svg_pos[v];
        verts_count++;
      }
    }
    return (verts_count > 0) ? c / double(verts_count) : double2{0.0, 0.0};
  };

  const char *out_path = path ? path : debug_html_filepath;

  static int unnamed_case_counter = 0;
  std::string case_label;
  if (input.label != nullptr && input.label[0] != '\0') {
    case_label = input.label;
  }
  else {
    case_label = "Case " + std::to_string(++unnamed_case_counter);
  }

  std::ofstream f;
  if (debug_new_file) {
    f.open(out_path, std::ios::trunc);
    f << "<!DOCTYPE html>\n"
      << "<html>\n"
      << "<head>\n"
      << "  <meta charset=\"utf-8\">\n"
      << "  <title>Straight Skeleton Triangulations</title>\n"
      << "  <style>\n"
      << "    body {\n"
      << "      font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, "
         "Helvetica, Arial, sans-serif;\n"
      << "      margin: 24px 40px;\n"
      << "      background: #eef2f7;\n"
      << "      color: #1a202c;\n"
      << "    }\n"
      << "    h1 {\n"
      << "      color: #1a365d;\n"
      << "      margin-bottom: 24px;\n"
      << "    }\n"
      << "    .test-case {\n"
      << "      background: #ffffff;\n"
      << "      border: 1px solid #cbd5e0;\n"
      << "      border-radius: 8px;\n"
      << "      padding: 20px 24px 24px 24px;\n"
      << "      margin-bottom: 36px;\n"
      << "      box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.07), 0 2px 4px -1px rgba(0, 0, "
         "0, 0.04);\n"
      << "    }\n"
      << "    .test-case h2 {\n"
      << "      margin-top: 0;\n"
      << "      margin-bottom: 12px;\n"
      << "      color: #2b6cb0;\n"
      << "      font-size: 20px;\n"
      << "      border-bottom: 2px solid #edf2f7;\n"
      << "      padding-bottom: 8px;\n"
      << "    }\n"
      << "    .case-meta {\n"
      << "      margin: 0 0 16px 0;\n"
      << "      color: #718096;\n"
      << "      font-size: 13px;\n"
      << "    }\n"
      << "    .svg-container {\n"
      << "      display: inline-block;\n"
      << "      border: 1px solid #e2e8f0;\n"
      << "      background: #f8fafc;\n"
      << "      border-radius: 4px;\n"
      << "      overflow: hidden;\n"
      << "    }\n"
      << "    svg text {\n"
      << "      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "
         "monospace;\n"
      << "      user-select: none;\n"
      << "    }\n"
      << "  </style>\n"
      << "</head>\n"
      << "<body>\n"
      << "<h1>Straight Skeleton Triangulations</h1>\n";
    debug_new_file = false;
  }
  else {
    f.open(out_path, std::ios::app);
  }

  f << "<section class=\"test-case\" id=\"" << case_label << "\">\n";
  f << "<h2>" << case_label << "</h2>\n";
  f << "<p class=\"case-meta\">Input: " << input.vert.size() << " vertices, " << input.edge.size()
    << " edges, " << input.face_offsets.size() << " faces &bull; Triangles: " << triangles_num
    << "</p>\n";
  f << "<div class=\"svg-container\">\n";
  f << "<svg width=\"" << svgw << "\" height=\"" << svgh << "\" viewBox=\"0 0 " << svgw << " "
    << svgh << "\">\n";
  f << "<text x=\"15\" y=\"20\" font-size=\"11\" fill=\"#000088\" font-weight=\"bold\">― Input "
       "contour edges (bold)</text>\n";
  f << "<text x=\"15\" y=\"35\" font-size=\"11\" fill=\"#cc0088\" font-weight=\"bold\">╌ 0-length "
       "cap edges (magenta)</text>\n";
  f << "<text x=\"15\" y=\"50\" font-size=\"11\" fill=\"#6688aa\">╌ Delaunay edges "
       "(dashed)</text>\n";

  /* Fill bounded triangles. */
  for (const int ti : IndexRange(triangles_num)) {
    if (tri.is_triangle_unbounded(ti) || tri.is_triangle_deleted(ti)) {
      continue;
    }
    const int e0 = 3 * ti;
    const int v0 = tri.start_vert(e0);
    const int v1 = tri.start_vert(e0 + 1);
    const int v2 = tri.start_vert(e0 + 2);
    if (v0 < 0 || v0 >= verts_num || v1 < 0 || v1 >= verts_num || v2 < 0 || v2 >= verts_num) {
      continue;
    }
    f << "<polygon points=\"" << vert_svg_pos[v0].x << "," << vert_svg_pos[v0].y << " "
      << vert_svg_pos[v1].x << "," << vert_svg_pos[v1].y << " " << vert_svg_pos[v2].x << ","
      << vert_svg_pos[v2].y << "\" fill=\"#ddeeff\" stroke=\"none\"/>\n";
  }

  /* Draw geometric edges and half-edge labels. */
  Set<int2> drawn_geom_edges;
  for (const int ei : IndexRange(edges_num)) {
    const int u = tri.start_vert(ei);
    const int v = tri.end_vert(ei);

    if (u < 0 || u >= verts_num || v < 0 || v >= verts_num) {
      continue;
    }

    const int paired = tri.paired_edge(ei);
    if (paired == -1 || ei < paired) {
      drawn_geom_edges.add(int2(std::min(u, v), std::max(u, v)));
      const int *ce_idx = contours.edge_by_verts.lookup_ptr(int2(u, v));
      if (ce_idx == nullptr) {
        ce_idx = contours.edge_by_verts.lookup_ptr(int2(v, u));
      }

      if (ce_idx != nullptr) {
        const ContourEdge &ce = contours.edges[*ce_idx];
        if (ce.side == -1) {
          /* 0-length cap edge. */
          f << "<line x1=\"" << vert_svg_pos[u].x << "\" y1=\"" << vert_svg_pos[u].y << "\" x2=\""
            << vert_svg_pos[v].x << "\" y2=\"" << vert_svg_pos[v].y
            << "\" stroke=\"#cc0088\" stroke-width=\"2.5\" stroke-dasharray=\"4,2\"/>\n";
        }
        else {
          /* Input contour edge. */
          f << "<line x1=\"" << vert_svg_pos[u].x << "\" y1=\"" << vert_svg_pos[u].y << "\" x2=\""
            << vert_svg_pos[v].x << "\" y2=\"" << vert_svg_pos[v].y
            << "\" stroke=\"#000088\" stroke-width=\"2.5\"/>\n";
        }
      }
      else {
        /* Non-contour Delaunay edge. */
        f << "<line x1=\"" << vert_svg_pos[u].x << "\" y1=\"" << vert_svg_pos[u].y << "\" x2=\""
          << vert_svg_pos[v].x << "\" y2=\"" << vert_svg_pos[v].y
          << "\" stroke=\"#6688aa\" stroke-width=\"1.2\" stroke-dasharray=\"5,4\"/>\n";
      }
    }

    /* Half-edge label offset into triangle interior. */
    const double dx_s = vert_svg_pos[v].x - vert_svg_pos[u].x;
    const double dy_s = vert_svg_pos[v].y - vert_svg_pos[u].y;
    const double dist_s = std::sqrt(dx_s * dx_s + dy_s * dy_s);
    if (dist_s > 1e-6) {
      const double px = dy_s / dist_s;
      const double py = -dx_s / dist_s;
      const double mid_sx = 0.5 * (vert_svg_pos[u].x + vert_svg_pos[v].x);
      const double mid_sy = 0.5 * (vert_svg_pos[u].y + vert_svg_pos[v].y);
      const double lx = mid_sx + px * label_off_px;
      const double ly = mid_sy + py * label_off_px;
      f << "<text x=\"" << lx << "\" y=\"" << ly
        << "\" font-size=\"10\" fill=\"#880000\" text-anchor=\"middle\">" << ei << "</text>\n";
    }
  }

  /* Draw wing stubs and label wing edges. */
  for (const int ti : IndexRange(triangles_num)) {
    if (!tri.is_triangle_unbounded(ti) || tri.is_triangle_deleted(ti)) {
      continue;
    }
    const int e0 = 3 * ti;
    const int vi = tri.start_vert(e0);
    const int ui = tri.start_vert(e0 + 1);

    if (ui >= 0 && ui < verts_num) {
      const auto [ndx, ndy] = stub_dir_svg(ui);
      const double x1 = vert_svg_pos[ui].x;
      const double y1 = vert_svg_pos[ui].y;
      const double x2 = x1 + ndx * ray_length;
      const double y2 = y1 + ndy * ray_length;
      f << "<line x1=\"" << x1 << "\" y1=\"" << y1 << "\" x2=\"" << x2 << "\" y2=\"" << y2
        << "\" stroke=\"#999999\" stroke-width=\"1\" stroke-dasharray=\"5,3\"/>\n";
      f << "<text x=\"" << (x1 + ndx * ray_length * 0.6 + ndy * 8.0) << "\" y=\""
        << (y1 + ndy * ray_length * 0.6 - ndx * 8.0)
        << "\" font-size=\"10\" fill=\"#aa6600\" text-anchor=\"middle\">" << (e0 + 1)
        << "</text>\n";
    }

    if (vi >= 0 && vi < verts_num) {
      const auto [ndx, ndy] = stub_dir_svg(vi);
      const double x1 = vert_svg_pos[vi].x;
      const double y1 = vert_svg_pos[vi].y;
      f << "<line x1=\"" << x1 << "\" y1=\"" << y1 << "\" x2=\"" << (x1 + ndx * ray_length * 0.4)
        << "\" y2=\"" << (y1 + ndy * ray_length * 0.4)
        << "\" stroke=\"#bbbbbb\" stroke-width=\"1\" stroke-dasharray=\"3,3\"/>\n";
      f << "<text x=\"" << (x1 + ndx * ray_length * 0.25 - ndy * 8.0) << "\" y=\""
        << (y1 + ndy * ray_length * 0.25 + ndx * 8.0)
        << "\" font-size=\"10\" fill=\"#aa8800\" text-anchor=\"middle\">" << (e0 + 2)
        << "</text>\n";
    }
  }

  /* Triangle labels. */
  for (const int ti : IndexRange(triangles_num)) {
    if (tri.is_triangle_deleted(ti)) {
      continue;
    }
    if (tri.is_triangle_unbounded(ti)) {
      const int e0 = 3 * ti;
      const int vi = tri.start_vert(e0);
      const int ui = tri.start_vert(e0 + 1);
      if (vi < 0 || vi >= verts_num || ui < 0 || ui >= verts_num) {
        continue;
      }
      const double dx_s = vert_svg_pos[ui].x - vert_svg_pos[vi].x;
      const double dy_s = vert_svg_pos[ui].y - vert_svg_pos[vi].y;
      const double dist_s = std::sqrt(dx_s * dx_s + dy_s * dy_s);
      if (dist_s < 1e-6) {
        continue;
      }
      const double px = dy_s / dist_s;
      const double py = -dx_s / dist_s;
      const double mid_sx = 0.5 * (vert_svg_pos[vi].x + vert_svg_pos[ui].x);
      const double mid_sy = 0.5 * (vert_svg_pos[vi].y + vert_svg_pos[ui].y);
      const double lx = mid_sx + px * 26.0;
      const double ly = mid_sy + py * 26.0;
      f << "<text x=\"" << lx << "\" y=\"" << ly
        << "\" font-size=\"11\" fill=\"#886600\" text-anchor=\"middle\" "
        << "font-weight=\"bold\">t" << ti << "</text>\n";
    }
    else {
      const double2 cen = tri_centroid(ti);
      f << "<text x=\"" << cen.x << "\" y=\"" << cen.y
        << "\" font-size=\"11\" fill=\"#004488\" text-anchor=\"middle\" "
        << "font-weight=\"bold\">t" << ti << "</text>\n";
    }
  }

  /* Draw any contour edges (wavefronts and end caps) not already drawn as part of tri. */
  for (const ContourEdge &ce : contours.edges) {
    const int u = ce.start;
    const int v = ce.end;
    if (u < 0 || u >= verts_num || v < 0 || v >= verts_num) {
      continue;
    }
    const int2 geom_edge(std::min(u, v), std::max(u, v));
    if (drawn_geom_edges.add(geom_edge)) {
      if (ce.side == -1) {
        /* 0-length cap edge. */
        f << "<line x1=\"" << vert_svg_pos[u].x << "\" y1=\"" << vert_svg_pos[u].y << "\" x2=\""
          << vert_svg_pos[v].x << "\" y2=\"" << vert_svg_pos[v].y
          << "\" stroke=\"#cc0088\" stroke-width=\"2.5\" stroke-dasharray=\"4,2\"/>\n";
      }
      else {
        /* Wavefront contour edge. */
        f << "<line x1=\"" << vert_svg_pos[u].x << "\" y1=\"" << vert_svg_pos[u].y << "\" x2=\""
          << vert_svg_pos[v].x << "\" y2=\"" << vert_svg_pos[v].y
          << "\" stroke=\"#000088\" stroke-width=\"2.5\"/>\n";
      }
    }
  }

  /* Vertex circles and real labels (including +N duplicates). */
  for (const int vi : IndexRange(verts_num)) {
    const double cx = vert_svg_pos[vi].x;
    const double cy = vert_svg_pos[vi].y;
    f << "<circle cx=\"" << cx << "\" cy=\"" << cy
      << "\" r=\"5\" fill=\"#224488\" stroke=\"white\" stroke-width=\"1\"/>\n";
    f << "<text x=\"" << (cx + 7) << "\" y=\"" << (cy - 5)
      << "\" font-size=\"12\" fill=\"#112233\" font-weight=\"bold\">" << vi << "</text>\n";
  }

  f << "</svg>\n";
  f << "</div>\n";

  /* Dump of Contours and Triangles in readable HTML section. */
  std::stringstream ss_contours;
  dump_contours(contours, ss_contours);

  std::stringstream ss_triangles;
  dump_triangles(tri, ss_triangles);

  f << "<details open style=\"margin-top: 16px;\">\n";
  f << "  <summary style=\"font-weight: bold; cursor: pointer; color: #2b6cb0; margin-bottom: "
       "8px;\">Contours &amp; Triangles Dump</summary>\n";
  f << "  <div style=\"display: flex; gap: 20px; flex-wrap: wrap;\">\n";
  f << "    <div style=\"flex: 1; min-width: 320px;\">\n";
  f << "      <h4 style=\"margin: 4px 0 8px 0; color: #4a5568;\">Contour Sequences</h4>\n";
  f << "      <pre style=\"background: #f7fafc; border: 1px solid #e2e8f0; padding: 10px; "
       "border-radius: 4px; font-family: monospace; font-size: 11px; overflow-x: auto; "
       "max-height: 450px;\">"
    << ss_contours.str() << "</pre>\n";
  f << "    </div>\n";
  f << "    <div style=\"flex: 1; min-width: 320px;\">\n";
  f << "      <h4 style=\"margin: 4px 0 8px 0; color: #4a5568;\">Triangles</h4>\n";
  f << "      <pre style=\"background: #f7fafc; border: 1px solid #e2e8f0; padding: 10px; "
       "border-radius: 4px; font-family: monospace; font-size: 11px; overflow-x: auto; "
       "max-height: 450px;\">"
    << ss_triangles.str() << "</pre>\n";
  f << "    </div>\n";
  f << "  </div>\n";
  f << "</details>\n";

  f << "</section>\n";
  f.close();
  std::cout << "SVG section '" << case_label << "' written to " << out_path << "\n";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Triangulation construction and rewiring
 * \{ */

static void build_contour_sequences(const StraightSkeletonInput &input,
                                    ContourSequences &r_contours)
{
  const int orig_verts_num = int(input.vert.size());

  /* 1. Closed face boundaries. */
  const Span<int> fvi = input.face_vert_indices;
  for (const int fi : input.face_offsets.index_range()) {
    const IndexRange r = input.face_offsets[fi];
    const int face_verts_num = int(r.size());
    for (const int k : IndexRange(face_verts_num)) {
      const int u = fvi[r[k]];
      const int v = fvi[r[(k + 1) % face_verts_num]];

      ContourEdge ce0;
      ce0.start = u;
      ce0.end = v;
      ce0.input_face = fi;
      ce0.side = 0;
      const int idx0 = int(r_contours.edges.size());
      r_contours.edges.append(ce0);
      r_contours.edge_by_verts.add(int2(u, v), idx0);

      ContourEdge ce1;
      ce1.start = v + orig_verts_num;
      ce1.end = u + orig_verts_num;
      ce1.input_face = fi;
      ce1.side = 1;
      const int idx1 = int(r_contours.edges.size());
      r_contours.edges.append(ce1);
      r_contours.edge_by_verts.add(int2(ce1.start, ce1.end), idx1);
    }
  }

  /* 2. Explicit line segment edges. */
  for (const int ei : input.edge.index_range()) {
    const int u = input.edge[ei][0];
    const int v = input.edge[ei][1];

    ContourEdge ce0;
    ce0.start = u;
    ce0.end = v;
    ce0.input_edge = ei;
    ce0.side = 0;
    const int idx0 = int(r_contours.edges.size());
    r_contours.edges.append(ce0);
    r_contours.edge_by_verts.add(int2(u, v), idx0);

    ContourEdge ce1;
    ce1.start = v + orig_verts_num;
    ce1.end = u + orig_verts_num;
    ce1.input_edge = ei;
    ce1.side = 1;
    const int idx1 = int(r_contours.edges.size());
    r_contours.edges.append(ce1);
    r_contours.edge_by_verts.add(int2(ce1.start, ce1.end), idx1);
  }

  /* 3. 0-length cap edges at free ends. */
  Array<int> degree(orig_verts_num, 0);
  Array<int> last_edge(orig_verts_num, -1);
  for (const int ei : input.edge.index_range()) {
    const int u = input.edge[ei][0];
    const int v = input.edge[ei][1];
    degree[u]++;
    last_edge[u] = ei;
    degree[v]++;
    last_edge[v] = ei;
  }
  Array<bool> in_face(orig_verts_num, false);
  for (const int vi : input.face_vert_indices) {
    in_face[vi] = true;
  }

  for (const int vi : IndexRange(orig_verts_num)) {
    if (degree[vi] == 1 && !in_face[vi]) {
      const int ei = last_edge[vi];
      const int v = input.edge[ei][1];
      ContourEdge cap;
      cap.side = -1;
      cap.input_edge = ei;
      if (vi == v) {
        /* Edge arrives at vi: cap goes vi -> vi + N. */
        cap.start = vi;
        cap.end = vi + orig_verts_num;
      }
      else {
        /* Edge leaves vi: cap goes vi + N -> vi. */
        cap.start = vi + orig_verts_num;
        cap.end = vi;
      }
      const int cap_idx = int(r_contours.edges.size());
      r_contours.edges.append(cap);
      r_contours.edge_by_verts.add(int2(cap.start, cap.end), cap_idx);
    }
  }

  /* 4. Link contour edges into loops. */
  Map<int, Vector<int>> outgoing_map;
  for (const int cei : r_contours.edges.index_range()) {
    outgoing_map.lookup_or_add_default(r_contours.edges[cei].start).append(cei);
  }

  for (const int cei : r_contours.edges.index_range()) {
    const int end_v = r_contours.edges[cei].end;
    const Vector<int> *out = outgoing_map.lookup_ptr(end_v);
    if (out != nullptr && !out->is_empty()) {
      r_contours.edges[cei].next = (*out)[0];
      r_contours.edges[(*out)[0]].prev = cei;
    }
  }

  /* 5. Extract loops. */
  Array<bool> visited(r_contours.edges.size(), false);
  for (const int i : r_contours.edges.index_range()) {
    if (visited[i]) {
      continue;
    }
    Vector<int> loop;
    int curr = i;
    while (!visited[curr]) {
      visited[curr] = true;
      loop.append(curr);
      curr = r_contours.edges[curr].next;
      if (curr == -1) {
        break;
      }
    }
    r_contours.loops.append(std::move(loop));
  }
}

static void pair_unbounded_wings(TriangleEdges &triangles)
{
  const int triangles_num = triangles.triangles_num();
  Map<int, int> wing_out; /* vert -> edge index of vert -> -1 */
  Map<int, int> wing_in;  /* vert -> edge index of -1 -> vert */

  for (const int ti : IndexRange(triangles_num)) {
    if (!triangles.is_triangle_unbounded(ti) || triangles.is_triangle_deleted(ti)) {
      continue;
    }
    const int e_right = 3 * ti + 1; /* vert -> -1 */
    const int e_left = 3 * ti + 2;  /* -1 -> vert */
    const int u = triangles.start_vert(e_right);
    const int v = triangles.end_vert(e_left);
    if (u >= 0) {
      wing_out.add_overwrite(u, e_right);
    }
    if (v >= 0) {
      wing_in.add_overwrite(v, e_left);
    }
  }

  for (const auto item : wing_out.items()) {
    const int u = item.key;
    const int e_out = item.value;
    const int *e_in = wing_in.lookup_ptr(u);
    if (e_in != nullptr) {
      triangles.set_paired(e_out, *e_in);
      triangles.set_paired(*e_in, e_out);
    }
  }
}

static int choose_vertex_duplicate(const int v,
                                   const double2 &dir,
                                   const StraightSkeletonInput &input,
                                   const ContourSequences &contours)
{
  const int orig_verts_num = int(input.vert.size());
  const double2 pv = input.vert[v];

  /* Find incident Side 0 contour edges. */
  int w_out = -1;
  int w_in = -1;
  for (const ContourEdge &ce : contours.edges) {
    if (ce.side == 0) {
      if (ce.start == v) {
        w_out = ce.end;
      }
      if (ce.end == v) {
        w_in = ce.start;
      }
    }
  }

  const double2 p_test = pv + dir;

  if (w_out != -1 && w_in != -1) {
    /* Corner (closed face or interior chain vertex). */
    const double2 p_out = input.vert[w_out];
    const double2 p_in = input.vert[w_in];

    const int o_turn = orient2d(p_in, pv, p_out);
    const int o_out = orient2d(pv, p_out, p_test);
    const int o_in = orient2d(p_in, pv, p_test);

    bool is_side0 = false;
    if (o_turn == 0) {
      /* Straight line: Side 0 is left of outgoing edge. */
      is_side0 = (o_out >= 0);
    }
    else if (o_turn > 0) {
      /* Convex corner (interior angle < 180 deg). */
      is_side0 = (o_out >= 0 && o_in >= 0);
    }
    else {
      /* Reflex corner (interior angle > 180 deg). */
      is_side0 = (o_out >= 0 || o_in >= 0);
    }
    return is_side0 ? v : (v + orig_verts_num);
  }
  else if (w_out != -1) {
    /* Start of chain (free end): outgoing edge v -> w_out. */
    const double2 p_out = input.vert[w_out];
    return (orient2d(pv, p_out, p_test) >= 0) ? v : (v + orig_verts_num);
  }
  else if (w_in != -1) {
    /* End of chain (free end): incoming edge w_in -> v. */
    const double2 p_in = input.vert[w_in];
    return (orient2d(p_in, pv, p_test) >= 0) ? v : (v + orig_verts_num);
  }

  return v;
}

static void rewire_kinetic_triangulation(const StraightSkeletonInput &input,
                                         TriangleEdges &triangles,
                                         const ContourSequences &contours)
{
  const int orig_verts_num = int(input.vert.size());
  const int initial_triangles_num = triangles.triangles_num();

  /* 1. Build initial edge set to know which edges existed before rewiring. */
  Set<int2> initial_edges;
  for (const int ei : IndexRange(triangles.edges_num())) {
    const int u = triangles.start_vert(ei);
    const int v = triangles.end_vert(ei);
    if (u >= 0 && v >= 0) {
      initial_edges.add(int2(std::min(u, v), std::max(u, v)));
    }
  }

  /* Identify convex hull vertices from initial unbounded triangles. */
  Set<int> hull_verts;
  for (const int ti : IndexRange(initial_triangles_num)) {
    if (triangles.is_triangle_unbounded(ti) && !triangles.is_triangle_deleted(ti)) {
      const int u = triangles.start_vert(3 * ti);
      const int v = triangles.start_vert(3 * ti + 1);
      if (u >= 0) {
        hull_verts.add(u % orig_verts_num);
      }
      if (v >= 0) {
        hull_verts.add(v % orig_verts_num);
      }
    }
  }

  /* 2. Rewire bounded triangles. */
  for (const int ti : IndexRange(initial_triangles_num)) {
    if (triangles.is_triangle_unbounded(ti) || triangles.is_triangle_deleted(ti)) {
      continue;
    }
    const int e0 = 3 * ti;
    const int v0 = triangles.start_vert(e0) % orig_verts_num;
    const int v1 = triangles.start_vert(e0 + 1) % orig_verts_num;
    const int v2 = triangles.start_vert(e0 + 2) % orig_verts_num;

    const double2 p0 = input.vert[v0];
    const double2 p1 = input.vert[v1];
    const double2 p2 = input.vert[v2];
    const double2 cen = (p0 + p1 + p2) / 3.0;

    const int v0_new = choose_vertex_duplicate(v0, cen - p0, input, contours);
    const int v1_new = choose_vertex_duplicate(v1, cen - p1, input, contours);
    const int v2_new = choose_vertex_duplicate(v2, cen - p2, input, contours);

    triangles.set_triangle_verts(ti, v0_new, v1_new, v2_new);
  }

  /* Map from (u_orig, v_orig) to contour edge (start, end) for wavefront contour edges. */
  Map<int2, int2> contour_by_orig;
  for (const ContourEdge &ce : contours.edges) {
    if (ce.side >= 0) {
      contour_by_orig.add(int2(ce.start % orig_verts_num, ce.end % orig_verts_num),
                          int2(ce.start, ce.end));
    }
  }

  /* 3. Rewire unbounded hull triangles.
   * Each unbounded hull triangle's base edge is paired with a hull edge ei = (u, v)
   * of a bounded triangle. The base edge in CCW hull order goes v -> u.
   * If v -> u corresponds to an input wavefront edge, use the matching contour edge.
   * Otherwise (Delaunay edge between disconnected inputs), use (v_new, u_new) from
   * the rewired bounded Delaunay triangle. */
  for (const int ti : IndexRange(initial_triangles_num)) {
    if (!triangles.is_triangle_unbounded(ti) || triangles.is_triangle_deleted(ti)) {
      continue;
    }
    const int base_edge = 3 * ti;
    const int paired_ei = triangles.paired_edge(base_edge);
    if (paired_ei != -1) {
      const int u_orig = triangles.start_vert(paired_ei) % orig_verts_num;
      const int v_orig = triangles.end_vert(paired_ei) % orig_verts_num;
      const int2 *ce_verts = contour_by_orig.lookup_ptr(int2(v_orig, u_orig));
      if (ce_verts != nullptr) {
        triangles.set_triangle_verts(ti, (*ce_verts)[0], (*ce_verts)[1], -1);
      }
      else {
        const int u_new = triangles.start_vert(paired_ei);
        const int v_new = triangles.end_vert(paired_ei);
        triangles.set_triangle_verts(ti, v_new, u_new, -1);
      }
    }
  }

  /* 4. Handle facing free ends (e.g. verts 9 and 12).
   * For each pair of free ends connected by an initial Delaunay edge, add a quad. */
  Array<int> edge_deg(orig_verts_num, 0);
  for (const int2 &e : input.edge) {
    edge_deg[e[0]]++;
    edge_deg[e[1]]++;
  }

  Vector<int> free_ends;
  for (const int vi : IndexRange(orig_verts_num)) {
    if (edge_deg[vi] == 1) {
      free_ends.append(vi);
    }
  }

  for (int i = 0; i < free_ends.size(); i++) {
    for (int j = i + 1; j < free_ends.size(); j++) {
      const int u = free_ends[i];
      const int v = free_ends[j];
      if (!hull_verts.contains(u) && !hull_verts.contains(v)) {
        if (initial_edges.contains(int2(std::min(u, v), std::max(u, v)))) {
          const int u_dup = u + orig_verts_num;
          const int v_dup = v + orig_verts_num;
          triangles.add_triangle(u, u_dup, v);
          triangles.add_triangle(u, v, v_dup);
        }
      }
    }
  }

  /* 5. Handle free ends on the convex hull.
   * Add an unbounded triangle for the cap edge at each hull free end. */
  for (const int u : free_ends) {
    if (hull_verts.contains(u)) {
      int cap_start = -1;
      int cap_end = -1;
      for (const ContourEdge &ce : contours.edges) {
        if (ce.side == -1 && (ce.start == u || ce.end == u)) {
          cap_start = ce.start;
          cap_end = ce.end;
          break;
        }
      }
      if (cap_start != -1) {
        triangles.add_triangle(cap_start, cap_end, -1);
      }
    }
  }

  /* 6. Re-pair all half-edges. */
  const int total_edges_num = triangles.edges_num();
  Map<int2, int> new_edge_map;
  new_edge_map.reserve(total_edges_num);
  for (const int ei : IndexRange(total_edges_num)) {
    const int u = triangles.start_vert(ei);
    const int v = triangles.end_vert(ei);
    if (u >= 0 && v >= 0) {
      new_edge_map.add(int2(u, v), ei);
    }
  }
  for (const int ei : IndexRange(total_edges_num)) {
    const int u = triangles.start_vert(ei);
    const int v = triangles.end_vert(ei);
    if (u >= 0 && v >= 0) {
      const int *opp = new_edge_map.lookup_ptr(int2(v, u));
      if (opp != nullptr) {
        triangles.set_paired(ei, *opp);
      }
      else {
        triangles.set_paired(ei, -1);
      }
    }
    else {
      triangles.set_paired(ei, -1);
    }
  }

  /* 7. Pair unbounded wings around infinity. */
  pair_unbounded_wings(triangles);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main state
 * \{ */

/**
 * All working state for a straight skeleton computation.
 */
class StraightSkeletonState {
  /** The triangulation, stored as half-edges. */
  TriangleEdges triangles;
  /** Coincidence threshold used throughout the computation. */
  double epsilon{1e-8};
  /** 2D positions of all vertices (original [0, N-1] and duplicate [N, 2N-1]). */
  Array<double2> vert_pos;
  /** Contour sequences (loops of wavefront edges). */
  ContourSequences contours;

 public:
  StraightSkeletonState(const StraightSkeletonInput &input) : epsilon(input.epsilon)
  {
    const int orig_verts_num = int(input.vert.size());
    vert_pos.reinitialize(2 * orig_verts_num);
    for (const int i : IndexRange(orig_verts_num)) {
      vert_pos[i] = input.vert[i];
      vert_pos[i + orig_verts_num] = input.vert[i];
    }
  }

  /** Build the initial kinetic triangulation from \a input.
   * After this call #triangles contains the initial triangulation
   * with unbounded triangles added for exterior boundary edges.
   * \param dbg_level If > 0, dump the state to stdout and write an SVG to /tmp. */
  void build_initial_kinetic_triangulation(const StraightSkeletonInput &input, int dbg_level = 0);
};

void StraightSkeletonState::build_initial_kinetic_triangulation(const StraightSkeletonInput &input,
                                                                int dbg_level)
{
  /* 1. Build initial contour sequences from the input geometry (faces, edges, free-end caps). */
  build_contour_sequences(input, contours);

  /* 2. Compute constrained Delaunay triangulation (CDT_FULL covers the convex hull). */
  meshintersect::CDT_input<double> cdt_in;
  cdt_in.vert = input.vert;
  cdt_in.edge = input.edge;
  cdt_in.face_offsets = input.face_offsets;
  cdt_in.face_vert_indices = input.face_vert_indices;
  cdt_in.epsilon = epsilon;
  cdt_in.needed_ids = CDT_NO_ORIG_IDS;

  const meshintersect::CDT_result<double> cdt = meshintersect::delaunay_2d_calc(cdt_in, CDT_FULL);
  const int cdt_triangles_num = int(cdt.face.size());

  if (cdt_triangles_num == 0) {
    /* Degenerate 1D / collinear input (e.g. single line segment or collinear chains):
     * No 2D CDT faces exist because the input points have zero area in 2D.
     * Every contour edge in the wavefront loops lies on the exterior boundary.
     * Create an unbounded triangle with base v -> u for each contour edge u -> v,
     * then pair adjacent wings around infinity. */
    for (const Vector<int> &loop : contours.loops) {
      for (const int cei : loop) {
        const ContourEdge &ce = contours.edges[cei];
        triangles.add_triangle(ce.start, ce.end, -1);
      }
    }
    pair_unbounded_wings(triangles);
  }
  else {
    /* Load CDT triangles into TriangleEdges. */
    triangles.reserve_triangles(4 * cdt_triangles_num);
    for (const int fi : IndexRange(cdt_triangles_num)) {
      const Vector<int> &f = cdt.face[fi];
      BLI_assert(f.size() == 3);
      triangles.add_triangle(f[0], f[1], f[2]);
    }

    /* Pair up interior half-edges. */
    const int edges_num = 3 * cdt_triangles_num;
    Map<int2, int> edge_map;
    edge_map.reserve(edges_num);
    for (const int ei : IndexRange(edges_num)) {
      const int u = triangles.start_vert(ei);
      const int v = triangles.end_vert(ei);
      edge_map.add(int2(u, v), ei);
    }
    for (const int ei : IndexRange(edges_num)) {
      const int u = triangles.start_vert(ei);
      const int v = triangles.end_vert(ei);
      const int *opp = edge_map.lookup_ptr(int2(v, u));
      if (opp != nullptr) {
        triangles.set_paired(ei, *opp);
      }
    }

    /* Add unbounded triangles for convex hull edges. */
    Vector<int> hull_edges;
    hull_edges.reserve(cdt_triangles_num);
    for (const int ei : IndexRange(edges_num)) {
      if (triangles.paired_edge(ei) == -1) {
        hull_edges.append(ei);
      }
    }
    for (const int ei : hull_edges) {
      const int u = triangles.start_vert(ei);
      const int v = triangles.end_vert(ei);
      const int new_tri = triangles.triangles_num();
      triangles.add_triangle(v, u, -1);
      const int base_edge = 3 * new_tri;
      triangles.set_paired(ei, base_edge);
      triangles.set_paired(base_edge, ei);
    }

    /* Rewire kinetic triangulation to attach triangles to correct duplicate vertices. */
    rewire_kinetic_triangulation(input, triangles, contours);
  }

  if (dbg_level > 0) {
    dump_contours(contours);
    dump_triangles(triangles);
    draw_triangulation_svg(triangles, vert_pos, input, contours);
  }
}

/** \} */

StraightSkeletonOutput straight_skeleton_calc(const StraightSkeletonInput &input)
{
  StraightSkeletonState state(input);
  state.build_initial_kinetic_triangulation(input, /*dbg_level=*/1);
  /* TODO: run event loop. */
  return StraightSkeletonOutput{};
}

} /* namespace blender::straightskeleton */
