/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Straight Skeleton computation for 2D Planar Straight Line Graphs (PSLGs).
 *
 * The straight skeleton of a PSLG is a geometric structure where each edge of
 * the skeleton is equidistant from two boundary edges of the input geometry.
 * As an inset amount increases, the boundary edges move parallel to themselves
 * at unit speed, and the skeleton records the trace of vertices under this motion.
 *
 * Input
 * -----
 * The input is a Planar Straight Line Graph (PSLG) consisting of:
 *   - 2D vertices
 *   - Closed faces
 *   - Optional wire edges
 *
 * Wavefront propagation direction is controlled by direction:
 *   - Inside  - propagates into interior domains (solid faces).
 *   - Outside - propagates into exterior domains (bounding box & holes).
 *   - Both    - propagates into both inside and outside domains.
 *
 * Vertices are given as 2D coordinates (double precision).
 *
 * Offset and Segments
 * -------------------
 * offset controls the offset distance:
 *   - When left at infinity (the default), the complete straight skeleton is computed.
 *   - When set to a finite positive value d, wavefronts are stopped at distance d.
 *
 * segments divides the effective offset range into that many
 * equal-width bands. Band 0 is the outermost ring (adjacent to the input boundary);
 * band k-1 is the innermost ring.
 *
 * Output
 * ------
 * The output describes the faces of the straight skeleton roof (or offset bands):
 *   - vert          - 2D coordinates of output vertices.
 *   - edge          - pairs of indices into #vert.
 *   - face_offsets  - ranges into #face_vert_indices.
 *   - vert_orig[v]  - index of the input vertex that
 *     corresponds to output vertex v, or -1 if v is a new skeleton vertex.
 *   - edge_orig[e]  - index of the input boundary edge that
 *     corresponds to output edge e, or -1 if e is a skeleton edge.
 *   - face_orig[f]  - index of the input face in #face_offsets
 *     from which output face f was derived, or -1 for bounding box faces.
 *   - face_band[f]  - zero-based band index of output face f.
 *
 * Miscellaneous
 * -------
 * `epsilon` controls how close two vertices must be to be
 * considered coincident. If zero is supplied, a default of 1e-6 is used.
 * `bbox_expansion_pct` controls how far the bounding box is expanded
 * when outside propagation is enabled.
 */

#include <limits>
#include <string>

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

namespace blender::straightskeleton {

/* -------------------------------------------------------------------- */
/** \name Input
 * \{ */

/** Controls the domain into which straight skeleton wavefronts propagate. */
enum class PropagationDirection : uint8_t {
  Inside = 0,
  Outside = 1,
  Both = 2,
};

class StraightSkeletonInput {
 public:
  Span<double2> vert;
  Span<int2> edge;

  OffsetIndices<int> face_offsets;
  Span<int> face_vert_indices;

  PropagationDirection direction{PropagationDirection::Inside};

  double bbox_expansion_pct{10.0};
  double epsilon{0.0};
  double offset{std::numeric_limits<double>::infinity()};
  int segments{1};

  /* These are used for testing; users can ignore them. */
  const char *label{nullptr};
  bool full_verification{false};
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Output
 * \{ */

class StraightSkeletonOutput {
 public:
  Array<double2> vert;
  Array<int2> edge;

  Array<int> face_offsets_data;

  OffsetIndices<int> face_offsets() const
  {
    return OffsetIndices<int>(face_offsets_data.as_span());
  }

  Array<int> face_vert_indices;
  Array<double> vert_time;
  Array<int> vert_orig;
  Array<int> edge_orig;
  Array<int> face_orig;
  Array<int> face_band;

  /* The following fields are for testing; users can ignore them. */
  bool hit_max_events{false};
  int events_processed_num{0};
  bool is_valid{true};
  std::string error_message;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entry Point
 * \{ */

/**
 * Compute the straight skeleton of the Planar Straight Line Graph (PSLG) described by \a input,
 * optionally clipped and subdivided into offset bands.
 */
StraightSkeletonOutput straight_skeleton_calc(const StraightSkeletonInput &input);

/** \} */

} /* namespace blender::straightskeleton */
