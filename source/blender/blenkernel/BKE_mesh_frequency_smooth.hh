/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

namespace blender::bke {

/**
 * Implicit frequency-aware filter that solves `(I + lambda * L^T L) x = x0`
 * with `lambda = 1/cutoff^2`.
 *
 * `L` is the row-normalized graph Laplacian. This is the same system previously solved with Eigen
 * SparseLU on the stacked least-squares matrix `[I; L/cutoff]`. The operator is SPD, so it is
 * solved in double with Jacobi-preconditioned conjugate gradient and TBB-parallel sparse matvecs.
 * The iterate is kept if the iteration cap is hit; only non-finite states are reported as failure.
 *
 * \param edge_weights: Optional per-edge weights. Empty uses uniform weights.
 * \param pinned: Optional per-vertex pin mask. Empty pins nothing.
 * \param axis_enabled: Per-axis solve mask (X, Y, Z). Disabled axes keep their input values.
 * \return false if the linear system fails numerically.
 */
bool mesh_frequency_smooth_filter(Span<int2> edges,
                                  GroupedSpan<int> vert_to_edge_map,
                                  Span<float> edge_weights,
                                  Span<bool> pinned,
                                  float cutoff,
                                  int iterations,
                                  const bool axis_enabled[3],
                                  MutableSpan<float3> positions);

}  // namespace blender::bke
