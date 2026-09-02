/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"

#include "BKE_mesh_frequency_smooth.hh"

namespace blender::bke {

struct AxisAccum {
  double value[3] = {0.0, 0.0, 0.0};
};

template<typename Fn>
static void for_each_vert(const Span<int> verts,
                          const int verts_num,
                          const int64_t grain_size,
                          const Fn &fn)
{
  if (verts.is_empty()) {
    threading::parallel_for(IndexRange(verts_num), grain_size, [&](const IndexRange range) {
      for (const int i : range) {
        fn(int(i));
      }
    });
  }
  else {
    threading::parallel_for(verts.index_range(), grain_size, [&](const IndexRange range) {
      for (const int64_t index : range) {
        fn(verts[index]);
      }
    });
  }
}

static AxisAccum axis_dot(const Span<double3> a,
                          const Span<double3> b,
                          const Span<int> verts,
                          const int verts_num)
{
  const IndexRange range = verts.is_empty() ? IndexRange(verts_num) : verts.index_range();
  return threading::parallel_reduce(
      range,
      4096,
      AxisAccum{},
      [&](const IndexRange subrange, AxisAccum sum) {
        if (verts.is_empty()) {
          for (const int64_t i : subrange) {
            for (const int axis : IndexRange(3)) {
              sum.value[axis] += a[i][axis] * b[i][axis];
            }
          }
        }
        else {
          for (const int64_t index : subrange) {
            const int i = verts[index];
            for (const int axis : IndexRange(3)) {
              sum.value[axis] += a[i][axis] * b[i][axis];
            }
          }
        }
        return sum;
      },
      [](AxisAccum a, const AxisAccum &b) {
        a.value[0] += b.value[0];
        a.value[1] += b.value[1];
        a.value[2] += b.value[2];
        return a;
      });
}

static void apply_frequency_operator(const OffsetIndices<int> adjacency,
                                     const Span<int> neighbors,
                                     const Span<double> weights,
                                     const Span<double> lt_weights,
                                     const Span<double> inverse_weight_sums,
                                     const Span<int> laplacian_verts,
                                     const Span<int> unknown_verts,
                                     const int verts_num,
                                     const double laplacian_weight,
                                     const Span<double3> src,
                                     MutableSpan<double3> laplacian,
                                     MutableSpan<double3> dst)
{
  for_each_vert(laplacian_verts, verts_num, 1024, [&](const int i) {
    const double inverse_weight_sum = inverse_weight_sums[i];
    if (inverse_weight_sum == 0.0) {
      laplacian[i] = double3(0.0);
      return;
    }

    double3 neighbor_sum(0.0);
    for (const int slot : adjacency[i]) {
      neighbor_sum += src[neighbors[slot]] * weights[slot];
    }
    laplacian[i] = src[i] - neighbor_sum * inverse_weight_sum;
  });

  /* Edge weights are symmetric, so `L^T y` is also a gather. Isolated vertices have `L_ii = 0`,
   * which is encoded by storing a zero Laplacian value for them.
   *
   * Accumulate in double: for large lambda, `src + lambda * L^T L src` loses the identity term
   * in float32, which is exactly the low-cutoff regime the unit tests cover. */
  for_each_vert(unknown_verts, verts_num, 1024, [&](const int i) {
    double3 transposed_laplacian = laplacian[i];
    for (const int slot : adjacency[i]) {
      transposed_laplacian -= laplacian[neighbors[slot]] * lt_weights[slot];
    }
    dst[i] = src[i] + transposed_laplacian * laplacian_weight;
  });
}

static bool frequency_pcg_solve(const OffsetIndices<int> adjacency,
                                const Span<int> neighbors,
                                const Span<double> weights,
                                const Span<double> lt_weights,
                                const Span<double> inverse_weight_sums,
                                const Span<double> inverse_diagonal,
                                const Span<int> laplacian_verts,
                                const Span<int> unknown_verts,
                                const int verts_num,
                                const double laplacian_weight,
                                const bool axis_enabled[3],
                                const Span<double3> rhs,
                                MutableSpan<double3> positions,
                                MutableSpan<double3> laplacian,
                                MutableSpan<double3> residual,
                                MutableSpan<double3> direction,
                                MutableSpan<double3> matrix_result)
{
  apply_frequency_operator(adjacency,
                           neighbors,
                           weights,
                           lt_weights,
                           inverse_weight_sums,
                           laplacian_verts,
                           unknown_verts,
                           verts_num,
                           laplacian_weight,
                           positions,
                           laplacian,
                           matrix_result);

  for_each_vert(unknown_verts, verts_num, 4096, [&](const int i) {
    for (const int axis : IndexRange(3)) {
      residual[i][axis] = axis_enabled[axis] ? rhs[i][axis] - matrix_result[i][axis] : 0.0;
      matrix_result[i][axis] = residual[i][axis] * inverse_diagonal[i];
      direction[i][axis] = matrix_result[i][axis];
    }
  });

  const AxisAccum rhs_dot = axis_dot(rhs, rhs, unknown_verts, verts_num);
  AxisAccum residual_dot = axis_dot(residual, residual, unknown_verts, verts_num);
  AxisAccum old_preconditioned_dot = axis_dot(residual, matrix_result, unknown_verts, verts_num);

  /* Stop on ||r|| / ||b||, not ||r|| / ||r0||. For large lambda, ||r0|| ~= ||A x0|| >> ||b||, so
   * reducing the initial residual by 1e-6 still leaves a large absolute equation error. */
  constexpr double squared_relative_tolerance = 1.0e-12;
  double residual_threshold[3];
  bool converged[3];
  for (const int axis : IndexRange(3)) {
    if (!axis_enabled[axis] || !std::isfinite(rhs_dot.value[axis]) || rhs_dot.value[axis] <= 0.0)
    {
      residual_threshold[axis] = 0.0;
      converged[axis] = true;
      continue;
    }
    residual_threshold[axis] = rhs_dot.value[axis] * squared_relative_tolerance;
    converged[axis] = residual_dot.value[axis] <= residual_threshold[axis];
  }
  if (converged[0] && converged[1] && converged[2]) {
    return true;
  }

  /* Jacobi-preconditioned CG needs O(sqrt(kappa) log(1/eps)) steps, kappa <= 1+4*lambda.
   * Do not cap at n_unknown: float/double PCG loses orthogonality, and small meshes with a
   * tiny cutoff need more iterations than vertices. */
  const double condition_bound = 1.0 + 4.0 * laplacian_weight;
  const int64_t iteration_bound = int64_t(std::ceil(std::sqrt(condition_bound) * 20.0));
  const int max_solver_iterations = int(std::min<int64_t>(100000, std::max<int64_t>(64, iteration_bound)));
  constexpr int restart_every = 256;

  auto finite_or_fail = [](const double value) { return std::isfinite(value); };

  for (int iteration = 0; iteration < max_solver_iterations; iteration++) {
    apply_frequency_operator(adjacency,
                             neighbors,
                             weights,
                             lt_weights,
                             inverse_weight_sums,
                             laplacian_verts,
                             unknown_verts,
                             verts_num,
                             laplacian_weight,
                             direction,
                             laplacian,
                             matrix_result);
    const AxisAccum direction_dot = axis_dot(direction, matrix_result, unknown_verts, verts_num);

    double step[3] = {0.0, 0.0, 0.0};
    for (const int axis : IndexRange(3)) {
      if (converged[axis]) {
        continue;
      }
      const double denominator = direction_dot.value[axis];
      if (!(denominator > 0.0) || !finite_or_fail(denominator)) {
        /* Search direction is numerically A-orthogonal; treat as converged for this axis. */
        converged[axis] = true;
        continue;
      }
      step[axis] = old_preconditioned_dot.value[axis] / denominator;
      if (!finite_or_fail(step[axis])) {
        return false;
      }
    }
    if (converged[0] && converged[1] && converged[2]) {
      return true;
    }

    for_each_vert(unknown_verts, verts_num, 4096, [&](const int i) {
      for (const int axis : IndexRange(3)) {
        if (converged[axis]) {
          continue;
        }
        positions[i][axis] += step[axis] * direction[i][axis];
        residual[i][axis] -= step[axis] * matrix_result[i][axis];
      }
    });

    residual_dot = axis_dot(residual, residual, unknown_verts, verts_num);
    for (const int axis : IndexRange(3)) {
      if (converged[axis]) {
        continue;
      }
      if (!finite_or_fail(residual_dot.value[axis])) {
        return false;
      }
      converged[axis] = residual_dot.value[axis] <= residual_threshold[axis];
    }
    if (converged[0] && converged[1] && converged[2]) {
      return true;
    }

    const bool restart = ((iteration + 1) % restart_every) == 0;
    if (restart) {
      apply_frequency_operator(adjacency,
                               neighbors,
                               weights,
                               lt_weights,
                               inverse_weight_sums,
                               laplacian_verts,
                               unknown_verts,
                               verts_num,
                               laplacian_weight,
                               positions,
                               laplacian,
                               matrix_result);
      for_each_vert(unknown_verts, verts_num, 4096, [&](const int i) {
        for (const int axis : IndexRange(3)) {
          residual[i][axis] = axis_enabled[axis] && !converged[axis] ?
                                  rhs[i][axis] - matrix_result[i][axis] :
                                  0.0;
        }
      });
      residual_dot = axis_dot(residual, residual, unknown_verts, verts_num);
      for (const int axis : IndexRange(3)) {
        if (converged[axis]) {
          continue;
        }
        if (!finite_or_fail(residual_dot.value[axis])) {
          return false;
        }
        converged[axis] = residual_dot.value[axis] <= residual_threshold[axis];
      }
      if (converged[0] && converged[1] && converged[2]) {
        return true;
      }
    }

    for_each_vert(unknown_verts, verts_num, 4096, [&](const int i) {
      for (const int axis : IndexRange(3)) {
        matrix_result[i][axis] = converged[axis] ? 0.0 : residual[i][axis] * inverse_diagonal[i];
      }
    });
    const AxisAccum new_preconditioned_dot = axis_dot(
        residual, matrix_result, unknown_verts, verts_num);

    double beta[3] = {0.0, 0.0, 0.0};
    for (const int axis : IndexRange(3)) {
      if (converged[axis]) {
        continue;
      }
      const double denominator = old_preconditioned_dot.value[axis];
      if (!(denominator > 0.0) || !finite_or_fail(denominator)) {
        converged[axis] = true;
        continue;
      }
      beta[axis] = restart ? 0.0 : new_preconditioned_dot.value[axis] / denominator;
      if (!finite_or_fail(beta[axis])) {
        return false;
      }
    }

    for_each_vert(unknown_verts, verts_num, 4096, [&](const int i) {
      for (const int axis : IndexRange(3)) {
        if (converged[axis]) {
          direction[i][axis] = 0.0;
          continue;
        }
        direction[i][axis] = matrix_result[i][axis] + beta[axis] * direction[i][axis];
      }
    });
    old_preconditioned_dot = new_preconditioned_dot;
  }

  /* Keep the last iterate instead of discarding the solve. The caller still applies it. */
  return true;
}

bool mesh_frequency_smooth_filter(const Span<int2> edges,
                                  const GroupedSpan<int> vert_to_edge_map,
                                  const Span<float> edge_weights,
                                  const Span<bool> pinned,
                                  const float cutoff,
                                  const int iterations,
                                  const bool axis_enabled[3],
                                  MutableSpan<float3> positions)
{
  const int verts_num = int(positions.size());
  if (verts_num == 0 || iterations == 0) {
    return true;
  }
  if (!axis_enabled[0] && !axis_enabled[1] && !axis_enabled[2]) {
    return true;
  }

  const bool has_pin = !pinned.is_empty();
  int n_unknown = verts_num;
  if (has_pin) {
    n_unknown = 0;
    for (const int i : positions.index_range()) {
      if (!pinned[i]) {
        n_unknown++;
      }
    }
  }
  if (n_unknown == 0) {
    return true;
  }

  const bool use_all = n_unknown == verts_num;
  Array<int> unknown_storage;
  if (!use_all) {
    unknown_storage.reinitialize(n_unknown);
    int unknown_index = 0;
    for (const int i : positions.index_range()) {
      if (!pinned[i]) {
        unknown_storage[unknown_index++] = i;
      }
    }
  }
  const Span<int> unknown_verts = unknown_storage.as_span();

  Array<int> laplacian_storage;
  if (!use_all) {
    Array<char> need_laplacian(verts_num, 0);
    for (const int i : unknown_verts) {
      need_laplacian[i] = 1;
      for (const int e : vert_to_edge_map[i]) {
        const int2 edge = edges[e];
        need_laplacian[(edge[0] == i) ? edge[1] : edge[0]] = 1;
      }
    }
    int laplacian_num = 0;
    for (const char flag : need_laplacian) {
      laplacian_num += flag;
    }
    laplacian_storage.reinitialize(laplacian_num);
    int laplacian_index = 0;
    for (const int i : positions.index_range()) {
      if (need_laplacian[i]) {
        laplacian_storage[laplacian_index++] = i;
      }
    }
  }
  const Span<int> laplacian_verts = laplacian_storage.as_span();

  const bool weighted = !edge_weights.is_empty();
  Array<double> inverse_weight_sums(verts_num, 0.0);
  for_each_vert(laplacian_verts, verts_num, 1024, [&](const int i) {
    double weight_sum = 0.0;
    for (const int e : vert_to_edge_map[i]) {
      weight_sum += weighted ? double(edge_weights[e]) : 1.0;
    }
    if (weight_sum > 0.0) {
      inverse_weight_sums[i] = 1.0 / weight_sum;
    }
  });

  Array<int> adj_offsets(verts_num + 1, 0);
  for_each_vert(laplacian_verts, verts_num, 1024, [&](const int i) {
    adj_offsets[i] = int(vert_to_edge_map[i].size());
  });
  const OffsetIndices<int> adjacency = offset_indices::accumulate_counts_to_offsets(adj_offsets);

  Array<int> neighbors(adjacency.total_size());
  Array<double> csr_weights(adjacency.total_size());
  Array<double> lt_weights(adjacency.total_size());
  for_each_vert(laplacian_verts, verts_num, 1024, [&](const int i) {
    int slot = int(adjacency[i].start());
    for (const int e : vert_to_edge_map[i]) {
      const int2 edge = edges[e];
      const int other = (edge[0] == i) ? edge[1] : edge[0];
      const double weight = weighted ? double(edge_weights[e]) : 1.0;
      neighbors[slot] = other;
      csr_weights[slot] = weight;
      lt_weights[slot] = weight * inverse_weight_sums[other];
      slot++;
    }
  });

  const double safe_cutoff = double(std::max(cutoff, 1e-6f));
  const double laplacian_weight = 1.0 / (safe_cutoff * safe_cutoff);

  Array<double> inverse_diagonal(verts_num, 0.0);
  for_each_vert(unknown_verts, verts_num, 1024, [&](const int i) {
    double normal_diagonal = (inverse_weight_sums[i] != 0.0) ? 1.0 : 0.0;
    for (const int slot : adjacency[i]) {
      const double coefficient = lt_weights[slot];
      normal_diagonal += coefficient * coefficient;
    }
    inverse_diagonal[i] = 1.0 / (1.0 + laplacian_weight * normal_diagonal);
  });

  Array<double3> solved(verts_num);
  threading::parallel_for(IndexRange(verts_num), 4096, [&](const IndexRange range) {
    for (const int64_t i : range) {
      solved[i] = double3(double(positions[i][0]), double(positions[i][1]), double(positions[i][2]));
    }
  });
  Array<double3> rhs(verts_num);
  Array<double3> laplacian(verts_num, double3(0.0));
  Array<double3> residual(verts_num, double3(0.0));
  Array<double3> direction(verts_num, double3(0.0));
  Array<double3> matrix_result(verts_num, double3(0.0));

  for (int iteration = 0; iteration < iterations; iteration++) {
    rhs.as_mutable_span().copy_from(solved.as_span());
    if (!frequency_pcg_solve(adjacency,
                             neighbors,
                             csr_weights,
                             lt_weights,
                             inverse_weight_sums,
                             inverse_diagonal,
                             laplacian_verts,
                             unknown_verts,
                             verts_num,
                             laplacian_weight,
                             axis_enabled,
                             rhs,
                             solved,
                             laplacian,
                             residual,
                             direction,
                             matrix_result))
    {
      return false;
    }
  }

  for_each_vert(unknown_verts, verts_num, 4096, [&](const int i) {
    positions[i] = float3(float(solved[i][0]), float(solved[i][1]), float(solved[i][2]));
  });
  return true;
}

}  // namespace blender::bke
