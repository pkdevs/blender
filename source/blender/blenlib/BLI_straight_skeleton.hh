/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Straight Skeleton computation for 2D polygons with holes.
 *
 * The straight skeleton of a polygon is a geometric structure where each edge of
 * the skeleton is equidistant from two boundary edges of the input polygon.
 * As an inset amount increases, the boundary of the polygon moves inward parallel
 * to itself (the outer polygon moves inward, while hole polygons move outward into
 * the interior domain), and the skeleton records the trace of vertices under this motion.
 *
 * Input
 * -----
 * The input is a set of 2D vertices, and either:
 *   - A single outer polygon oriented counter-clockwise (CCW), and any number of hole
 *     polygons each oriented clockwise (CW); OR
 *   - An open polyline (#StraightSkeletonInput::edge_chain) of at least 2 vertices.
 *
 * Vertices are given as 2D coordinates (double precision). Callers working in 3D
 * should project to the polygon plane before calling.
 *
 * Offset and Segments
 * -------------------
 * #StraightSkeletonInput::offset controls the inward/outward offset distance:
 *   - When left at infinity (the default for closed polygons), the complete straight
 *     skeleton (full roof tiling) is returned.
 *   - When set to a finite positive value d, a closed polygon is inset up to distance d,
 *     clipped at height t = d (clamped to maximum collapse time). When an edge chain is
 *     supplied, it is offset by distance d into a surrounding offset band.
 *   - When set to a finite negative value d, a closed polygon is outset (exterior offset
 *     advancing on the CW side of the outer polygon, and on the CCW side of inner holes)
 *     by distance |d|. Negative offset is a no-op for an edge chain.
 *
 * #StraightSkeletonInput::segments divides the effective offset range into that many
 * equal-width bands. Band 0 is the outermost ring (adjacent to the input boundary);
 * band k-1 is the innermost ring. #StraightSkeletonOutput::face_band records which
 * band each output face belongs to.
 *
 * Output
 * ------
 * The output describes the faces of the straight skeleton roof (or offset bands):
 *   - #StraightSkeletonOutput::vert          - 2D coordinates of output vertices.
 *   - #StraightSkeletonOutput::edge          - pairs of indices into #vert.
 *   - #StraightSkeletonOutput::face_offsets  - ranges into #face_vert_indices.
 *   - #StraightSkeletonOutput::vert_orig[v]  - index of the input vertex that
 *     corresponds to output vertex v, or -1 if v is a new skeleton vertex.
 *   - #StraightSkeletonOutput::edge_orig[e]  - index of the input boundary edge that
 *     corresponds to output edge e, or -1 if e is a skeleton edge.
 *   - #StraightSkeletonOutput::face_orig[f]  - index of the input face (0 for outer polygon,
 *     or hole index + 1) from which output face f was derived.
 *   - #StraightSkeletonOutput::face_band[f]  - zero-based band index of output face f.
 *
 * Epsilon
 * -------
 * #StraightSkeletonInput::epsilon controls how close two vertices must be to be
 * considered coincident. If zero is supplied, a default of 1e-8 is used.
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

/**
 * Input to the straight skeleton computation.
 * Specifies 1 outer polygon (oriented CCW) and any number of hole polygons (each oriented CW).
 * All indices inside #outer_polygon and #hole_vert_indices refer to positions in #vert.
 */
class StraightSkeletonInput {
 public:
  /** 2D positions of the input vertices. */
  Span<double2> vert;

  /** Vertex indices in #vert of the outer boundary polygon, oriented CCW. */
  Span<int> outer_polygon;

  /** Per-hole vertex ranges into #hole_vert_indices. */
  OffsetIndices<int> hole_offsets;

  /** Concatenated list of vertex indices in #vert for all hole polygons (each oriented CW). */
  Span<int> hole_vert_indices;

  /**
   * Vertex indices in #vert of an open polyline (edge chain) to offset.
   * Either (#outer_polygon with optional holes) OR #edge_chain must be provided, but not both.
   * When #edge_chain is provided, #offset must be a finite positive distance.
   */
  Span<int> edge_chain;

  /** Coincidence threshold. Pass 0.0 to use the default (1e-8). */
  double epsilon{0.0};

  /**
   * Inward or outward offset distance for polygon offsetting.
   *
   * - `infinity` (default): the full straight skeleton is computed.
   * - A finite positive value `d`: the polygon or chain is inset until d is reached.
   * - A finite negative value `d`: the polygon is outset until |d| is reached.
   */
  double offset{std::numeric_limits<double>::infinity()};

  /** Number of equal-width offset bands to subdivide the effective offset into. */
  int segments{1};

  /* The following are used only for debugging and testing. */

  /** Optional identifying label for debug drawings and logging. */
  const char *label{nullptr};

  /** If true, runs extensive validation checks (graph planarity, no edge crossings, face
   * simplicity, and area partition). */
  bool full_verification{false};
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Output
 * \{ */

/**
 * Output of the straight skeleton computation.
 *
 * The arrays are owned by the output struct.
 */
class StraightSkeletonOutput {
 public:
  /** 2D positions of the output vertices (original boundary + new skeleton vertices). */
  Array<double2> vert;

  /** Output edges as pairs of indices into #vert. */
  Array<int2> edge;

  /** Storage backing face_offsets(). */
  Array<int> face_offsets_data;

  /** Per-face vertex ranges into #face_vert_indices. */
  OffsetIndices<int> face_offsets() const
  {
    return OffsetIndices<int>(face_offsets_data.as_span());
  }

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

  /** For each output face, the zero-based band index it belongs to. Zero is outermost. */
  Array<int> face_band;

  /* The following fields are for testing; users can ignore them. */

  /** True if the event loop was terminated because max_events was reached. */
  bool hit_max_events{false};

  /** Total number of events processed during the simulation. */
  int events_processed_num{0};

  /** If full_verification was requested, true if all validation checks passed. */
  bool is_valid{true};

  /** Detailed diagnostic message if validation failed. */
  std::string error_message;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entry Point
 * \{ */

/**
 * Compute the straight skeleton of the polygon with holes described by \a input,
 * optionally clipped and subdivided into offset bands.
 */
StraightSkeletonOutput straight_skeleton_calc(const StraightSkeletonInput &input);

/** \} */

} /* namespace blender::straightskeleton */
