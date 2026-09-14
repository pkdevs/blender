/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Straight Skeleton computation for PSLGs (Planar Straight Line Graphs).
 *
 * The straight skeleton of a PSLG is a geometric structure in which each edge of
 * the skeleton is equidistant from two edges of the input PSLG.  It is used to
 * compute interior offsets (insets) of PSLG: as an inset amount increases, each
 * edge of the PSLG moves inward parallel to itself, and the skeleton records the
 * trace of each vertex under this motion.
 *
 * Input
 * -----
 * The input is a set of vertices and one or more faces (simple polygons) defined over
 * those vertices.  Edges that are not part of a face may also be supplied as
 * additional input.
 *
 * Vertices are given as 2D coordinates (double precision).  The library ignores the
 * z-coordinate; callers that work in 3D should project to the face plane before
 * calling.
 *
 * Faces must be simple (non-self-intersecting) polygons wound counter-clockwise when
 * viewed in the standard orientation (y up).  Clockwise-wound faces will produce an
 * exterior skeleton rather than an interior one.
 *
 * Output
 * ------
 * The output describes the faces of the offset mesh: for every input face there will
 * be one or more output faces.  Each output face corresponds to one edge of the
 * input polygon (a "side strip") or to the central skeleton region.
 *
 * Output-to-input mappings are always filled in:
 *   - #StraightSkeletonOutput::vert_orig[v]  - index of the input vertex that
 *     corrseponds to output vertex v, or -1 if v is a new skeleton vertex.
 *   - #StraightSkeletonOutput::edge_orig[e]  - index of the input edge that
 *     corresponds to output edge e, or -1 if e is a new skeleton edge.
 *   - #StraightSkeletonOutput::face_orig[f]  - index of the input face from which
 *     output face f was derived.
 *
 * Epsilon
 * -------
 * #StraightSkeletonInput::epsilon controls how close two vertices must be to be
 * considered coincident.  If zero is supplied, a default of 1e-8 is used.
 */

#include <limits>

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {
struct HeapNode;
}

namespace blender::straightskeleton {

class StraightSkeletonInput;

/* -------------------------------------------------------------------- */
/** \name Kinetic Triangle Events
 * \{ */

enum class TriangleEventType {
  NONE,
  EDGE_COLLAPSE,
  SPLIT,
  FLIP,
  MULTI_SPLIT,
  UNBOUNDED_COLLAPSE,
};

struct TriangleEvent {
  int triangle_index{-1};
  double time{std::numeric_limits<double>::infinity()};
  TriangleEventType type{TriangleEventType::NONE};
  int edge_index{-1};          /* Half-edge index involved in the event */
  int vert_index{-1};          /* Vertex index involved in the event */
  double2 event_pos{0.0, 0.0}; /* Coordinates where event occurs */
  HeapNode *heap_node{nullptr};
};

const char *triangle_event_type_name(TriangleEventType type);

/** For testing: calculate initial collapse events in the priority queue. */
Array<TriangleEvent> straight_skeleton_initial_events(const StraightSkeletonInput &input);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Skeleton Graph & Kinetic Vertices
 * \{ */

struct SkeletonNode {
  int id{-1};                 /* Node index in SkeletonGraph */
  double2 pos{0.0, 0.0};      /* 2D position (x, y) */
  double time{0.0};           /* Event time / height t (0 for input verts) */
  int orig_vert{-1};          /* Index of coincident input vertex, or -1 for interior nodes */
  Vector<int> incident_edges; /* Incident SkeletonEdge indices */
};

struct SkeletonEdge {
  int id{-1};                    /* Unique edge index in SkeletonGraph */
  int node1{-1};                 /* First endpoint (lower time or start) */
  int node2{-1};                 /* Second endpoint (higher time or end) */
  int orig_edge{-1};             /* Coincident input edge index, or -1 for skeleton arcs */
  int input_face{-1};            /* Incident input face index, or -1 */
  int side{0};                   /* Side 0 or Side 1 */
  double time_start{0.0};        /* Time at node1 */
  double time_end{0.0};          /* Time at node2 */
  bool is_input_boundary{false}; /* True for input edges / face boundaries at t=0 */
};

class SkeletonGraph {
 public:
  Vector<SkeletonNode> nodes;
  Vector<SkeletonEdge> edges;

  int add_node(const double2 &pos, double time, int orig_vert = -1);
  int add_edge(int node1,
               int node2,
               int orig_edge = -1,
               int input_face = -1,
               int side = 0,
               double time_start = 0.0,
               double time_end = 0.0,
               bool is_input_boundary = false);
};

struct KineticVertex {
  int id{-1};                  /* Unique kinetic vertex index */
  int start_node{-1};          /* SkeletonNode index where this vertex was born */
  double start_time{0.0};      /* Creation time (t_birth) */
  double2 start_pos{0.0, 0.0}; /* Position at start_time */
  double2 velocity{0.0, 0.0};  /* Velocity vector v */

  int end_node{-1}; /* SkeletonNode index where this vertex crashes */
  double end_time{std::numeric_limits<double>::infinity()}; /* Collapse time */

  int input_edge{-1}; /* Incident input edge (or -1) */
  int input_face{-1}; /* Incident input face (or -1) */
  int side{0};        /* 0 or 1 */

  bool is_active{true};
  bool is_infinitely_fast{false};

  double2 position_at(const double t) const
  {
    return start_pos + (t - start_time) * velocity;
  }
};

/** For testing: return the initial skeleton graph built from input. */
SkeletonGraph straight_skeleton_initial_graph(const StraightSkeletonInput &input);

struct EventLoopStats {
  int events_processed_num{0};
  int flips_num{0};
  int edge_collapses_num{0};
  int multi_splits_num{0};
  int splits_num{0};
};

/** For testing: run the straight skeleton event loop up to \a max_events. */
EventLoopStats straight_skeleton_run_events_test(const StraightSkeletonInput &input,
                                                 int max_events = 100,
                                                 int dbg_level = 0,
                                                 bool handle_edge_collapses = true);

/** For testing: return active triangles (as triples of vertex indices) after running \a
 * max_events. */
Vector<int3> straight_skeleton_active_triangles_test(const StraightSkeletonInput &input,
                                                     int max_events = 100,
                                                     int dbg_level = 0,
                                                     bool handle_edge_collapses = true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Input
 * \{ */

/**
 * Input to the straight skeleton computation.
 * All indices inside #edge and #face_vert_indices refer to positions in #vert.
 */
class StraightSkeletonInput {
 public:
  /** 2D positions of the input vertices. */
  Span<double2> vert;

  /** Optional additional edges, each given as a pair of vertex indices. */
  Span<int2> edge;

  /** Per-face vertex ranges into #face_vert_indices. */
  OffsetIndices<int> face_offsets;

  /** Concatenated list of vertex indices for all faces.
   * Vertices for face f occupy face_vert_indices[face_offsets[f]]. */
  Span<int> face_vert_indices;

  /** Coincidence threshold.  Pass 0.0 to use the default (1e-8). */
  double epsilon{0.0};

  /** Optional identifying label for debug drawings and logging. */
  const char *label{nullptr};
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Output
 * \{ */

/**
 * Output of the straight skeleton computation.
 *
 * The arrays are owned by the output struct.
 * The meaning is the same as in the input, but in addition there are arrays
 * to give the mapping from output indices to input ones.
 */
class StraightSkeletonOutput {
 public:
  /** 2D positions of the output vertices (original boundary + new skeleton vertices). */
  Array<double2> vert;

  /** Output edges as pairs of indices into #vert. */
  Array<int2> edge;

  /** Storage backing face_offsets. */
  Array<int> face_offsets_data;

  /** Per-face vertex ranges into #face_vert_indices. */
  OffsetIndices<int> face_offsets;

  /** Concatenated list of vertex indices for all output faces. */
  Array<int> face_vert_indices;

  /** For each output vertex, the straight skeleton collapse time / offset distance. */
  Array<double> vert_time;

  /** For each output vertex, the index of the coincident input vertex, or -1. */
  Array<int> vert_orig;

  /** For each output edge, the index of the input edge it lies on, or -1. */
  Array<int> edge_orig;

  /** For each output face, the index of the input face it was derived from. */
  Array<int> face_orig;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entry Point
 * \{ */

/**
 * Compute the straight skeleton of the polygon mesh described by \a input.
 *
 * Returns the output faces that tile the interior of the input polygon(s),
 * together with origin mappings back to the input.
 */
StraightSkeletonOutput straight_skeleton_calc(const StraightSkeletonInput &input);

/**
 * Compute the inward straight-skeleton offset (inset) of polygons.
 *
 * \param vert               2D coordinates of the input vertices.
 * \param polygons           Vertex indices of the boundary polygons. CCW-oriented
 *                           polygons are treated as outer boundaries; CW-oriented
 *                           polygons are treated as interior holes.
 * \param offset             Inward offset distance (non-negative).
 * \param r_new_vert         New vertices created by the operation are appended here.
 *                           Face indices in the return value that are >= `vert.size()`
 *                           address `r_new_vert[index - vert.size()]`.
 * \param r_new_vert_offset  For each new vertex in \a r_new_vert, its offset distance
 *                           (creation time in the straight skeleton advancement) is
 *                           stored here, parallel to \a r_new_vert.
 *
 * \return A list of polygonal faces (each face is a list of vertex indices)
 *         that together tile the interior of the outer boundaries minus their holes,
 *         accounting for the offset.  The faces include the side strips
 *         beside each boundary edge as well as the remaining inner region(s).
 */
Vector<Vector<int>> straight_skeleton_offset_calc(Span<double2> vert,
                                                  const Vector<Vector<int>> &polygons,
                                                  double offset,
                                                  Vector<double2> &r_new_vert,
                                                  Vector<double> &r_new_vert_offset);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug Output Controls
 * \{ */

/**
 * When true, debug drawing functions open a new file (truncating existing content)
 * and write the HTML document header.  When false, they append new sections to the
 * existing file.  Debug drawing functions automatically set this to false after opening
 * a new file.
 */
extern bool debug_new_file;

/** Alias for convenience. */
inline bool &straight_skeleton_debug_new_file = debug_new_file;

/** Filepath for debug HTML output. */
extern const char *debug_html_filepath;

/** Reset the debug file flag so the next debug drawing starts a new file. */
void debug_reset_file();

/** \} */

} /* namespace blender::straightskeleton */
