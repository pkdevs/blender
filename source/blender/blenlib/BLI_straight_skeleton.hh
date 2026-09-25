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
 * The input is a set of 2D vertices, a single outer polygon oriented counter-clockwise (CCW),
 * and any number of hole polygons each oriented clockwise (CW).
 *
 * Vertices are given as 2D coordinates (double precision). Callers working in 3D
 * should project to the polygon plane before calling.
 *
 * Offset and Segments
 * -------------------
 * #StraightSkeletonInput::offset controls the inward offset distance. When left at
 * its default of infinity, the complete straight skeleton (full roof tiling) is returned.
 * When set to a finite value d, the roof is clipped at height t = d; if d exceeds the
 * maximum collapse time it is silently clamped so that no bands are empty.
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

  /** Coincidence threshold. Pass 0.0 to use the default (1e-8). */
  double epsilon{0.0};

  /**
   * Inward offset distance for polygon offsetting.
   *
   * - `infinity` (default): the full straight skeleton is computed. The algorithm
   *   determines the maximum collapse time automatically and uses it as the effective
   *   offset for the purposes of band subdivision (see #segments).
   * - A finite non-negative value `d`: the roof is clipped at height
   *   `t = min(d, max_collapse_time)`. Silently clamping to the maximum collapse
   *   time ensures all bands have uniform width and none are empty.
   */
  double offset{std::numeric_limits<double>::infinity()};

  /**
   * Number of equal-width offset bands to subdivide the effective offset into.
   *
   * - `1` (default): a single output at the effective offset. When #offset is
   *   infinite this produces the complete roof tiling as one set of faces (band 0).
   * - `k > 1`: the range `[0, effective_offset]` is divided into `k` bands of equal
   *   width (`effective_offset / k` each) by instantiating intermediate wavefronts at
   *   `t_i = effective_offset * i / k` for `i = 1 ... k-1`.
   *   Each band `b` (0-indexed, outermost first) spans `[t_b, t_{b+1}]` and produces
   *   its own side-strip faces plus the inner polygon at `t_{b+1}`.
   *   When #offset is infinite, #segments still applies using the computed
   *   `max_collapse_time` as the effective 100% mark.
   */
  int segments{1};

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

  /**
   * For each output face, the zero-based band index it belongs to.
   *
   * Band 0 is the outermost ring (side-strip faces adjacent to the input boundary at t = 0).
   * Band `k-1` is the innermost ring (includes the inner polygon at the effective offset).
   * When #StraightSkeletonInput::segments is 1 every entry is 0.
   */
  Array<int> face_band;

  /* The following fields are provided for unit testing; users can ignore them. */

  /** True if the event loop was terminated because max_events was reached. */
  bool hit_max_events{false};

  /** Total number of events processed during the simulation. */
  int events_processed_num{0};

  /** Total number of flip events processed. */
  int flips_num{0};

  /** If full_verification was requested, true if all validation checks passed. */
  bool is_valid{true};

  /** Detailed diagnostic message if validation failed. */
  std::string error_message;

  StraightSkeletonOutput() = default;

  StraightSkeletonOutput(const StraightSkeletonOutput &other)
  {
    *this = other;
  }

  StraightSkeletonOutput(StraightSkeletonOutput &&other) noexcept
  {
    *this = std::move(other);
  }

  StraightSkeletonOutput &operator=(const StraightSkeletonOutput &other)
  {
    if (this != &other) {
      vert = other.vert;
      edge = other.edge;
      face_offsets_data = other.face_offsets_data;
      face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
      face_vert_indices = other.face_vert_indices;
      vert_time = other.vert_time;
      vert_orig = other.vert_orig;
      edge_orig = other.edge_orig;
      face_orig = other.face_orig;
      face_band = other.face_band;
      hit_max_events = other.hit_max_events;
      events_processed_num = other.events_processed_num;
      flips_num = other.flips_num;
      is_valid = other.is_valid;
      error_message = other.error_message;
    }
    return *this;
  }

  StraightSkeletonOutput &operator=(StraightSkeletonOutput &&other) noexcept
  {
    if (this != &other) {
      vert = std::move(other.vert);
      edge = std::move(other.edge);
      face_offsets_data = std::move(other.face_offsets_data);
      face_offsets = OffsetIndices<int>(face_offsets_data.as_span());
      face_vert_indices = std::move(other.face_vert_indices);
      vert_time = std::move(other.vert_time);
      vert_orig = std::move(other.vert_orig);
      edge_orig = std::move(other.edge_orig);
      face_orig = std::move(other.face_orig);
      face_band = std::move(other.face_band);
      hit_max_events = other.hit_max_events;
      events_processed_num = other.events_processed_num;
      flips_num = other.flips_num;
      is_valid = other.is_valid;
      error_message = std::move(other.error_message);
      other.face_offsets = OffsetIndices<int>();
    }
    return *this;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entry Point
 * \{ */

/**
 * Compute the straight skeleton of the polygon with holes described by \a input,
 * optionally clipped and subdivided into offset bands.
 *
 * When `input.offset` is infinity (the default) and `input.segments` is 1,
 * the complete roof tiling is returned -- one face per input boundary edge plus
 * the inner skeleton faces.
 *
 * When `input.offset` is finite, or `input.segments > 1`, the output is
 * divided into `input.segments` equal-width bands spanning
 * `[0, effective_offset]`, where `effective_offset = min(input.offset,
 * max_collapse_time)`. Each band produces side-strip faces and an inner
 * polygon. #StraightSkeletonOutput::face_band identifies the band (0 =
 * outermost) for each output face.
 */
StraightSkeletonOutput straight_skeleton_calc(const StraightSkeletonInput &input);

/** \} */

} /* namespace blender::straightskeleton */
