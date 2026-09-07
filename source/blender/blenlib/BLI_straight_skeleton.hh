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

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

namespace blender::straightskeleton {

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

  /** Per-face vertex ranges into #face_vert_indices. */
  OffsetIndices<int> face_offsets;

  /** Concatenated list of vertex indices for all output faces. */
  Array<int> face_vert_indices;

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

/** \} */

}  // namespace blender::straightskeleton
