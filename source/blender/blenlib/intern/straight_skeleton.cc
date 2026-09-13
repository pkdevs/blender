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

#include "BLI_delaunay_2d.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_straight_skeleton.hh"

namespace blender::straightskeleton {

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

  void reserve_triangles(int nfaces)
  {
    start_.reserve(3 * nfaces);
    paired_.reserve(3 * nfaces);
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

  int num_triangles() const
  {
    return int(start_.size()) / 3;
  }

  int num_edges() const
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
/** \name Debug output
 * \{ */

/** Print a description of \a tri to stdout. */
static void dump_triangles(const TriangleEdges &tri)
{
  const int ntri = tri.num_triangles();
  std::cout << "TriangleEdges: " << ntri << " triangles, " << tri.num_edges() << " half-edges\n";
  for (const int ti : IndexRange(ntri)) {
    const int e0 = 3 * ti;
    const int e1 = e0 + 1;
    const int e2 = e0 + 2;
    const int v0 = tri.start_vert(e0);
    const int v1 = tri.start_vert(e1);
    const int v2 = tri.start_vert(e2);
    const bool is_unbounded = tri.is_triangle_unbounded(ti);
    const bool is_deleted = (v0 == -1);
    std::cout << "  tri[" << ti << "]"
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
static void draw_triangulation_svg(const TriangleEdges &tri,
                                   const Span<double2> vert_pos,
                                   const StraightSkeletonInput &input,
                                   const char *path)
{
  const int nverts = int(vert_pos.size());
  const int ntri = tri.num_triangles();
  const int nedges = tri.num_edges();

  /* Canonicalize all input constraint edges (both from explicit edges and face boundaries). */
  Set<int2> input_edges;
  for (const int2 &e : input.edge) {
    input_edges.add(int2(std::min(e[0], e[1]), std::max(e[0], e[1])));
  }
  const Span<int> fvi = input.face_vert_indices;
  for (const int fi : input.face_offsets.index_range()) {
    const IndexRange r = input.face_offsets[fi];
    const int sz = int(r.size());
    for (const int k : IndexRange(sz)) {
      const int a = fvi[r[k]];
      const int b = fvi[r[(k + 1) % sz]];
      input_edges.add(int2(std::min(a, b), std::max(a, b)));
    }
  }

  /* ---- bounding box ---- */
  double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30;
  for (const double2 &v : vert_pos) {
    xmin = std::min(xmin, v.x);
    xmax = std::max(xmax, v.x);
    ymin = std::min(ymin, v.y);
    ymax = std::max(ymax, v.y);
  }
  const double range = std::max(xmax - xmin, ymax - ymin);
  const double pad = range * 0.2 + 0.01;
  xmin -= pad;
  xmax += pad;
  ymin -= pad;
  ymax += pad;

  const double svgw = 700.0;
  const double svgh = 700.0;
  const double sx = svgw / (xmax - xmin);
  const double sy = svgh / (ymax - ymin);

  /* Map math coordinates to SVG coordinates (SVG y-axis is flipped). */
  auto tx = [&](double x) { return (x - xmin) * sx; };
  auto ty = [&](double y) { return svgh - (y - ymin) * sy; };

  /* ---- hull adjacency maps ---- */
  /* For each hull vertex u: next_hull[u] = v means the CCW polygon edge goes u->v. */
  Map<int, int> next_hull; /* next vertex CCW around hull */
  Map<int, int> prev_hull; /* previous vertex CCW around hull */
  /* For each hull vertex u: outgoing_wing[u] = half-edge index of u->(-1). */
  Map<int, int> outgoing_wing;
  /* For each hull vertex v: incoming_wing[v] = half-edge index of (-1)->v. */
  Map<int, int> incoming_wing;

  for (const int ti : IndexRange(ntri)) {
    if (!tri.is_triangle_unbounded(ti)) {
      continue;
    }
    const int e0 = 3 * ti;
    const int v = tri.start_vert(e0);     /* base goes v -> u */
    const int u = tri.start_vert(e0 + 1); /* u is the interior hull edge start */
    /* Interior hull edge was u->v, so CCW: after u comes v. */
    next_hull.add_overwrite(u, v);
    prev_hull.add_overwrite(v, u);
    outgoing_wing.add_overwrite(u, e0 + 1); /* u->(-1) */
    incoming_wing.add_overwrite(v, e0 + 2); /* (-1)->v */
  }

  /* ---- stub direction at hull vertex u ---- */
  /* Returns the exterior angle-bisector direction in SVG pixels. */
  auto stub_dir_svg = [&](int u) -> std::pair<double, double> {
    const int *nxt = next_hull.lookup_ptr(u);
    const int *prv = prev_hull.lookup_ptr(u);
    double2 sum{0.0, 0.0};
    if (nxt && *nxt >= 0 && *nxt < nverts) {
      const double2 d = vert_pos[*nxt] - vert_pos[u];
      const double len = std::sqrt(d.x * d.x + d.y * d.y);
      if (len > 1e-14) {
        sum.x += d.x / len;
        sum.y += d.y / len;
      }
    }
    if (prv && *prv >= 0 && *prv < nverts) {
      const double2 d = vert_pos[*prv] - vert_pos[u];
      const double len = std::sqrt(d.x * d.x + d.y * d.y);
      if (len > 1e-14) {
        sum.x += d.x / len;
        sum.y += d.y / len;
      }
    }
    /* Outward bisector = negate the inward bisector (sum of unit vectors toward neighbors). */
    double bx = -sum.x;
    double by = -sum.y;
    const double blen = std::sqrt(bx * bx + by * by);
    if (blen < 1e-14) {
      /* Degenerate: just go perpendicular to an adjacent edge. */
      if (nxt && *nxt >= 0 && *nxt < nverts) {
        const double2 d = vert_pos[*nxt] - vert_pos[u];
        bx = d.y;
        by = -d.x;
      }
    }
    else {
      bx /= blen;
      by /= blen;
    }
    /* Convert math direction (bx,by) to SVG pixels: SVG y is negated. */
    return {bx * sx, -by * sy};
  };

  /* Ray length for wing stubs, in SVG pixels. */
  const double ray_len = range * 0.15 * std::min(sx, sy);

  /* ---- label offset: half-edge labels are placed using the SVG right-perpendicular
   *      of the directed edge, at a fixed pixel distance.
   *
   *  For a CCW-wound interior triangle (appears CW in SVG), the right perp in SVG
   *  space points INTO the triangle interior, so each label sits clearly on its own
   *  side of the edge.  For unbounded base edges (reversed edge), the right perp
   *  points outward — also correct.
   * ---- */
  const double label_off_px = 14.0; /* pixels from edge to half-edge index */

  /* Centroid of a bounded triangle (average of real vertices). */
  auto tri_centroid = [&](int ti) -> double2 {
    const int e0 = 3 * ti;
    double2 c{0.0, 0.0};
    int count = 0;
    for (const int k : IndexRange(3)) {
      const int v = tri.start_vert(e0 + k);
      if (v >= 0 && v < nverts) {
        c += vert_pos[v];
        count++;
      }
    }
    return (count > 0) ? c / double(count) : double2{0.0, 0.0};
  };

  /* ---- open SVG ---- */
  std::ofstream f(path);
  f << "<!DOCTYPE html>\n<html><body>\n";
  f << "<svg width=\"" << svgw << "\" height=\"" << svgh
    << "\" style=\"border:1px solid #ccc; background:#f8f8f8\">\n";
  f << "<style>text { font-family: monospace; }</style>\n";
  f << "<text x=\"15\" y=\"20\" font-size=\"11\" fill=\"#000088\" font-weight=\"bold\">― Input "
       "edges (bold)</text>\n";
  f << "<text x=\"15\" y=\"35\" font-size=\"11\" fill=\"#6688aa\">╌ Delaunay edges "
       "(dashed)</text>\n";

  /* ---- fill bounded triangles ---- */
  for (const int ti : IndexRange(ntri)) {
    if (tri.is_triangle_unbounded(ti) || tri.is_triangle_deleted(ti)) {
      continue;
    }
    const int e0 = 3 * ti;
    const int v0 = tri.start_vert(e0);
    const int v1 = tri.start_vert(e0 + 1);
    const int v2 = tri.start_vert(e0 + 2);
    if (v0 < 0 || v0 >= nverts || v1 < 0 || v1 >= nverts || v2 < 0 || v2 >= nverts) {
      continue;
    }
    f << "<polygon points=\"" << tx(vert_pos[v0].x) << "," << ty(vert_pos[v0].y) << " "
      << tx(vert_pos[v1].x) << "," << ty(vert_pos[v1].y) << " " << tx(vert_pos[v2].x) << ","
      << ty(vert_pos[v2].y) << "\" fill=\"#ddeeff\" stroke=\"none\"/>\n";
  }

  /* ---- draw edges and half-edge labels ---- */
  for (const int ei : IndexRange(nedges)) {
    const int u = tri.start_vert(ei);
    const int v = tri.end_vert(ei);

    /* Skip wing edges (start or end is the ghost vertex). */
    if (u < 0 || u >= nverts || v < 0 || v >= nverts) {
      continue;
    }

    /* Draw the geometric edge only once (lower-indexed half-edge of the pair). */
    const int paired = tri.paired_edge(ei);
    if (paired == -1 || ei < paired) {
      const bool is_input = input_edges.contains(int2(std::min(u, v), std::max(u, v)));
      if (is_input) {
        /* Input constraint edge: bold solid line. */
        f << "<line x1=\"" << tx(vert_pos[u].x) << "\" y1=\"" << ty(vert_pos[u].y) << "\" x2=\""
          << tx(vert_pos[v].x) << "\" y2=\"" << ty(vert_pos[v].y)
          << "\" stroke=\"#000088\" stroke-width=\"2.5\"/>\n";
      }
      else {
        /* Non-input edge added for Delaunay triangulation: dashed line. */
        f << "<line x1=\"" << tx(vert_pos[u].x) << "\" y1=\"" << ty(vert_pos[u].y) << "\" x2=\""
          << tx(vert_pos[v].x) << "\" y2=\"" << ty(vert_pos[v].y)
          << "\" stroke=\"#6688aa\" stroke-width=\"1.2\" stroke-dasharray=\"5,4\"/>\n";
      }
    }

    /* Half-edge label: fixed perpendicular offset (SVG right-perp of the directed edge).
     * SVG right-perp of (dx_s, dy_s) is (dy_s, -dx_s); for CCW-in-math / CW-in-SVG
     * triangles this points into the triangle interior. */
    const double u_sx = tx(vert_pos[u].x);
    const double u_sy = ty(vert_pos[u].y);
    const double v_sx = tx(vert_pos[v].x);
    const double v_sy = ty(vert_pos[v].y);
    const double dx_s = v_sx - u_sx;
    const double dy_s = v_sy - u_sy;
    const double len_s = std::sqrt(dx_s * dx_s + dy_s * dy_s);
    if (len_s < 1e-6) {
      continue;
    }
    /* Normalized SVG right-perpendicular: (dy_s, -dx_s) / len_s */
    const double px = dy_s / len_s;
    const double py = -dx_s / len_s;
    const double mid_sx = 0.5 * (u_sx + v_sx);
    const double mid_sy = 0.5 * (u_sy + v_sy);
    const double lx = mid_sx + px * label_off_px;
    const double ly = mid_sy + py * label_off_px;
    f << "<text x=\"" << lx << "\" y=\"" << ly
      << "\" font-size=\"10\" fill=\"#880000\" text-anchor=\"middle\">" << ei << "</text>\n";
  }

  /* ---- draw wing stubs (angle bisector direction) and label all wing edges ---- */
  for (const int ti : IndexRange(ntri)) {
    if (!tri.is_triangle_unbounded(ti)) {
      continue;
    }
    const int e0 = 3 * ti;
    const int vi = tri.start_vert(e0);     /* base goes vi -> ui */
    const int ui = tri.start_vert(e0 + 1); /* ui is source of outgoing wing */

    /* Stub at ui (outgoing wing ui->(-1)), labeled with e0+1. */
    if (ui >= 0 && ui < nverts) {
      const auto [dx, dy] = stub_dir_svg(ui);
      const double dlen = std::sqrt(dx * dx + dy * dy);
      if (dlen > 1e-14) {
        const double ndx = dx / dlen;
        const double ndy = dy / dlen;
        const double x1 = tx(vert_pos[ui].x);
        const double y1 = ty(vert_pos[ui].y);
        const double x2 = x1 + ndx * ray_len;
        const double y2 = y1 + ndy * ray_len;
        f << "<line x1=\"" << x1 << "\" y1=\"" << y1 << "\" x2=\"" << x2 << "\" y2=\"" << y2
          << "\" stroke=\"#999999\" stroke-width=\"1\" stroke-dasharray=\"5,3\"/>\n";
        /* Label outgoing wing (e0+1) at 60% along the stub,
         * offset 8px to the right of the stub (perp = (ndy, -ndx)). */
        f << "<text x=\"" << (x1 + ndx * ray_len * 0.6 + ndy * 8.0) << "\" y=\""
          << (y1 + ndy * ray_len * 0.6 - ndx * 8.0)
          << "\" font-size=\"10\" fill=\"#aa6600\" text-anchor=\"middle\">" << (e0 + 1)
          << "</text>\n";
      }
    }

    /* Incoming wing (-1)->vi arrives at vi; stub from vi in ITS exterior bisector direction,
     * labeled with e0+2. Draw at 40% length so it doesn't fully overlap the outgoing stub. */
    if (vi >= 0 && vi < nverts) {
      const auto [dx, dy] = stub_dir_svg(vi);
      const double dlen = std::sqrt(dx * dx + dy * dy);
      if (dlen > 1e-14) {
        const double ndx = dx / dlen;
        const double ndy = dy / dlen;
        const double x1 = tx(vert_pos[vi].x);
        const double y1 = ty(vert_pos[vi].y);
        /* Draw at 40% length (shorter) and label at 25%. */
        f << "<line x1=\"" << x1 << "\" y1=\"" << y1 << "\" x2=\"" << (x1 + ndx * ray_len * 0.4)
          << "\" y2=\"" << (y1 + ndy * ray_len * 0.4)
          << "\" stroke=\"#bbbbbb\" stroke-width=\"1\" stroke-dasharray=\"3,3\"/>\n";
        /* Label incoming wing (e0+2) at 25% along the stub,
         * offset 8px to the left of the stub (perp = (-ndy, ndx)). */
        f << "<text x=\"" << (x1 + ndx * ray_len * 0.25 - ndy * 8.0) << "\" y=\""
          << (y1 + ndy * ray_len * 0.25 + ndx * 8.0)
          << "\" font-size=\"10\" fill=\"#aa8800\" text-anchor=\"middle\">" << (e0 + 2)
          << "</text>\n";
      }
    }
  }

  /* ---- triangle labels ---- */
  for (const int ti : IndexRange(ntri)) {
    if (tri.is_triangle_deleted(ti)) {
      continue;
    }
    if (tri.is_triangle_unbounded(ti)) {
      /* Unbounded label: outside the base edge midpoint (SVG right-perp direction outward). */
      const int e0 = 3 * ti;
      const int vi = tri.start_vert(e0);
      const int ui = tri.start_vert(e0 + 1);
      if (vi < 0 || vi >= nverts || ui < 0 || ui >= nverts) {
        continue;
      }
      const double vi_sx = tx(vert_pos[vi].x);
      const double vi_sy = ty(vert_pos[vi].y);
      const double ui_sx = tx(vert_pos[ui].x);
      const double ui_sy = ty(vert_pos[ui].y);
      const double dx_s = ui_sx - vi_sx;
      const double dy_s = ui_sy - vi_sy;
      const double len_s = std::sqrt(dx_s * dx_s + dy_s * dy_s);
      if (len_s < 1e-6) {
        continue;
      }
      const double mid_sx = 0.5 * (vi_sx + ui_sx);
      const double mid_sy = 0.5 * (vi_sy + ui_sy);
      /* SVG right-perp of vi->ui points outward for unbounded triangles. */
      const double px = dy_s / len_s;
      const double py = -dx_s / len_s;
      const double lx = mid_sx + px * 30.0;
      const double ly = mid_sy + py * 30.0;
      f << "<text x=\"" << lx << "\" y=\"" << ly
        << "\" font-size=\"11\" fill=\"#886600\" text-anchor=\"middle\" "
        << "font-weight=\"bold\">t" << ti << "</text>\n";
    }
    else {
      /* Bounded label at centroid. */
      const double2 cen = tri_centroid(ti);
      f << "<text x=\"" << tx(cen.x) << "\" y=\"" << ty(cen.y)
        << "\" font-size=\"11\" fill=\"#004488\" text-anchor=\"middle\" "
        << "font-weight=\"bold\">t" << ti << "</text>\n";
    }
  }

  /* ---- vertex circles and labels ---- */
  for (const int vi : IndexRange(nverts)) {
    const double cx = tx(vert_pos[vi].x);
    const double cy = ty(vert_pos[vi].y);
    f << "<circle cx=\"" << cx << "\" cy=\"" << cy
      << "\" r=\"5\" fill=\"#224488\" stroke=\"white\" stroke-width=\"1\"/>\n";
    f << "<text x=\"" << (cx + 7) << "\" y=\"" << (cy - 5)
      << "\" font-size=\"12\" fill=\"#112233\" font-weight=\"bold\">" << vi << "</text>\n";
  }

  f << "</svg>\n</body></html>\n";
  f.close();
  std::cout << "SVG written to " << path << "\n";
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
  /** 2D positions of the input vertices. */
  Span<double2> vert_pos;

 public:
  StraightSkeletonState(const StraightSkeletonInput &input)
      : epsilon(input.epsilon), vert_pos(input.vert)
  {
  }

  /** Build the initial kinetic triangulation from \a input.
   * After this call #triangles contains the CDT of the input geometry
   * with unbounded triangles added for convex-hull edges.
   * \param dbg_level If > 0, dump the state to stdout and write an SVG to /tmp. */
  void build_initial_kinetic_triangulation(const StraightSkeletonInput &input, int dbg_level = 0);
};

void StraightSkeletonState::build_initial_kinetic_triangulation(const StraightSkeletonInput &input,
                                                                int dbg_level)
{
  /* Pack input into a CDT_input and compute the constrained Delaunay triangulation.
   * CDT_FULL triangulates the entire convex hull of the input geometry. */
  meshintersect::CDT_input<double> cdt_in;
  cdt_in.vert = input.vert;
  cdt_in.edge = input.edge;
  cdt_in.face_offsets = input.face_offsets;
  cdt_in.face_vert_indices = input.face_vert_indices;
  cdt_in.epsilon = epsilon;
  cdt_in.needed_ids = CDT_NO_ORIG_IDS;

  const meshintersect::CDT_result<double> cdt = meshintersect::delaunay_2d_calc(cdt_in, CDT_FULL);

  /* Load CDT triangles into TriangleEdges.  Each CDT output face is a triangle.
   * Reserve 4x to allow for convex-hull unbounded triangles and future flip triangles. */
  const int cdt_ntri = int(cdt.face.size());
  triangles.reserve_triangles(4 * cdt_ntri);
  for (const int fi : IndexRange(cdt_ntri)) {
    const Vector<int> &f = cdt.face[fi];
    BLI_assert(f.size() == 3);
    triangles.add_triangle(f[0], f[1], f[2]);
  }

  /* Pair up half-edges: for each directed edge (u->v) find the opposite (v->u)
   * by building a map from (start, end) to edge index. */
  const int nedges = 3 * cdt_ntri;
  Map<int2, int> edge_map;
  edge_map.reserve(nedges);
  for (const int ei : IndexRange(nedges)) {
    const int u = triangles.start_vert(ei);
    const int v = triangles.end_vert(ei);
    edge_map.add(int2(u, v), ei);
  }
  for (const int ei : IndexRange(nedges)) {
    const int u = triangles.start_vert(ei);
    const int v = triangles.end_vert(ei);
    const int *opp = edge_map.lookup_ptr(int2(v, u));
    if (opp != nullptr) {
      triangles.set_paired(ei, *opp);
    }
  }

  /* Add an unbounded triangle for every convex-hull edge (any half-edge that is still
   * unpaired after the interior pairing pass).
   *
   * Convention: the unbounded triangle for interior hull half-edge ei (u->v) is stored as
   * vertices (v, u, -1), giving three half-edges:
   *   base:  v->u  (index 3*new_tri+0) -- paired with ei.
   *   right: u->-1 (index 3*new_tri+1) -- wing into infinity, no pair.
   *   left:  -1->v (index 3*new_tri+2) -- wing into infinity, no pair.
   *
   * Collect unpaired edges first so that adding new triangles does not invalidate
   * the index range being iterated. */
  Vector<int> hull_edges;
  hull_edges.reserve(cdt_ntri);
  for (const int ei : IndexRange(nedges)) {
    if (triangles.paired_edge(ei) == -1) {
      hull_edges.append(ei);
    }
  }
  for (const int ei : hull_edges) {
    const int u = triangles.start_vert(ei);
    const int v = triangles.end_vert(ei);
    const int new_tri = triangles.num_triangles();
    /* Store as (v, u, -1): base edge v->u is the correct geometric opposite of ei (u->v). */
    triangles.add_triangle(v, u, -1);
    const int base_edge = 3 * new_tri; /* half-edge v->u */
    triangles.set_paired(ei, base_edge);
    triangles.set_paired(base_edge, ei);
  }

  if (dbg_level > 0) {
    dump_triangles(triangles);
    draw_triangulation_svg(triangles, vert_pos, input, "/tmp/skel_tri_debug.html");
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

}  // namespace blender::straightskeleton
