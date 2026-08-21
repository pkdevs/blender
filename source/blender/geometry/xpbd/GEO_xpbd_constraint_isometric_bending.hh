/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "GEO_xpbd_constraint_coloring_utils.hh"
#include "GEO_xpbd_constraint_set_templated.hh"

namespace blender::xpbd {

/**
 * Weights of an isometric bending stencil, from "A Quadratic Bending Model for Inextensible
 * Surfaces" (Bergou et al. 2006). Points 0 and 1 span the edge shared by two triangles, points 2
 * and 3 are the corners opposite of it. The stencil's bending energy is 1/2 * |sum_i K_i * x_i|^2
 * with these weights and is zero for every affine image of the rest stencil, which decouples
 * bending from stretch and shear. The constraint function is C = |sum_i K_i * x_i| so that the
 * implicit XPBD energy C^2 / (2 * compliance) is that bending energy, which keeps the stiffness
 * of a compliance value stable under mesh refinement. Returns nothing for degenerate stencils.
 */
inline std::optional<float4> isometric_bending_weights(const float3 &x0,
                                                       const float3 &x1,
                                                       const float3 &x2,
                                                       const float3 &x3)
{
  const float3 e0 = x1 - x0;
  const float3 e1 = x2 - x0;
  const float3 e2 = x3 - x0;
  const float3 e3 = x2 - x1;
  const float3 e4 = x3 - x1;

  const float area0 = 0.5f * math::length(math::cross(e0, e1));
  const float area1 = 0.5f * math::length(math::cross(e0, e2));
  /* Relative to the stencil size so acceptance does not depend on the scene scale; <= also
   * rejects a stencil collapsed to a single point. */
  const float size_sq = math::max(
      math::max(math::length_squared(e0), math::length_squared(e1)),
      math::max(math::length_squared(e2),
                math::max(math::length_squared(e3), math::length_squared(e4))));
  if (area0 <= 1e-12f * size_sq || area1 <= 1e-12f * size_sq) {
    return std::nullopt;
  }

  const auto cot = [](const float3 &a, const float3 &b) {
    return math::dot(a, b) / math::length(math::cross(a, b));
  };
  const float c01 = cot(e0, e1);
  const float c02 = cot(e0, e2);
  const float c03 = cot(-e0, e3);
  const float c04 = cot(-e0, e4);

  const float scale = math::sqrt(3.0f / (2.0f * (area0 + area1)));
  return scale * float4(c03 + c04, c01 + c02, -c01 - c03, -c02 - c04);
}

/**
 * Stiffness scale k of a stencil such that the rest angle constraint below matches the bending
 * energy of the weight based constraint: 2 * k * sin(angle / 2) equals |sum_i K_i * x_i| under
 * isometric deformation of a flat rest stencil. Call with rest positions of a non degenerate
 * stencil.
 */
inline float isometric_bending_stiffness_scale(const float3 &x0,
                                               const float3 &x1,
                                               const float3 &x2,
                                               const float3 &x3)
{
  const float3 e0 = x1 - x0;
  const float area0 = 0.5f * math::length(math::cross(e0, x2 - x0));
  const float area1 = 0.5f * math::length(math::cross(e0, x3 - x0));
  return math::sqrt(3.0f / (2.0f * (area0 + area1))) * math::length(e0);
}

/** Wrap an angle to the range [-pi, pi). */
inline float wrap_angle_to_pi(const float angle)
{
  return math::floored_mod(angle + float(M_PI), 2.0f * float(M_PI)) - float(M_PI);
}

/**
 * Signed dihedral angle of a hinge and its position gradient. Zero for a planar stencil, sign by
 * the right hand rule around the edge from x0 to x1. The gradient is the classic per-vertex form
 * (Bridson, "Simulation of Clothing with Folds and Wrinkles"), negated to match the signed
 * angle. Returns nothing when the shared edge or a triangle collapses, tested relative to the
 * stencil size so the result does not depend on the scene scale.
 */
inline std::optional<float> dihedral_angle_and_gradient(
    const float3 &x0, const float3 &x1, const float3 &x2, const float3 &x3, float3 r_grads[4])
{
  const float3 e = x1 - x0;
  const float3 w2 = x2 - x0;
  const float3 w3 = x3 - x0;
  const float e_len_sq = math::length_squared(e);
  /* <= also rejects a stencil collapsed to a single point. */
  const float size_sq = math::max(e_len_sq,
                                  math::max(math::length_squared(w2), math::length_squared(w3)));
  if (e_len_sq <= 1e-12f * size_sq) {
    return std::nullopt;
  }
  const float3 n0 = math::cross(e, w2);
  const float3 n1 = math::cross(w3, e);
  const float n0_len_sq = math::length_squared(n0);
  const float n1_len_sq = math::length_squared(n1);
  /* |n| = 2 * area ~ edge length * distance of the opposite corner from the edge. */
  if (n0_len_sq < 1e-12f * math::square(e_len_sq) || n1_len_sq < 1e-12f * math::square(e_len_sq)) {
    return std::nullopt;
  }

  const float e_len = math::sqrt(e_len_sq);
  const float3 n0_unit = n0 / math::sqrt(n0_len_sq);
  const float3 n1_unit = n1 / math::sqrt(n1_len_sq);
  const float angle = math::atan2(math::dot(math::cross(n0_unit, n1_unit), e / e_len),
                                  math::dot(n0_unit, n1_unit));

  const float3 d2 = (e_len / n0_len_sq) * n0;
  const float3 d3 = (e_len / n1_len_sq) * n1;
  const float3 d0 = math::dot(x2 - x1, e) / e_len / n0_len_sq * n0 +
                    math::dot(x3 - x1, e) / e_len / n1_len_sq * n1;
  const float3 d1 = math::dot(x0 - x2, e) / e_len / n0_len_sq * n0 +
                    math::dot(x0 - x3, e) / e_len / n1_len_sq * n1;
  r_grads[0] = -d0;
  r_grads[1] = -d1;
  r_grads[2] = -d2;
  r_grads[3] = -d3;
  return angle;
}

/** Signed dihedral angle of a hinge, see #dihedral_angle_and_gradient. */
inline std::optional<float> dihedral_angle(const float3 &x0,
                                           const float3 &x1,
                                           const float3 &x2,
                                           const float3 &x3)
{
  float3 grads[4];
  return dihedral_angle_and_gradient(x0, x1, x2, x3, grads);
}

class IsometricBendingConstraintSet
    : public TemplatedConstraintSet<IsometricBendingConstraintSet> {
 private:
  /** Indexed by constraint index. */
  Span<int4> point_indices_;
  Span<float4> weights_;
  /**
   * Per constraint: x = stiffness scale k, y = rest dihedral angle, z = rest edge length.
   * Nearly flat rest shapes (see #FLAT_REST_ANGLE_THRESHOLD) use the weight based constraint
   * C = |sum_i K_i * x_i|, which is cheaper and has a constant gradient magnitude. Curved rest
   * shapes use C = 2 * k * sin((angle - rest_angle) / 2), which is zero at the rest shape
   * instead of flattening it. The two are energy equivalent under isometric deformation, so a
   * compliance value means the same stiffness on either path.
   */
  Span<float3> rest_data_;
  Span<float> compliances_;
  /* Scale factor for residual error. */
  float error_scale_;
  MutableSpan<float> lambdas_;

 public:
  static constexpr StringRefNull debug_name = "Isometric Bending";

  /**
   * Rest angles below this take the flat path. This is a model switch, not a continuous blend:
   * the constraint magnitude gap is bounded by roughly the stiffness scale times this value,
   * but the gradients of the two paths differ.
   */
  static constexpr float FLAT_REST_ANGLE_THRESHOLD = 1e-3f;

  IsometricBendingConstraintSet(const int geo_i,
                                const Span<int4> point_indices,
                                const Span<float4> weights,
                                const Span<float3> rest_data,
                                const Span<float> compliances,
                                const float error_scale,
                                MutableSpan<float> lambdas)
      : TemplatedConstraintSet<IsometricBendingConstraintSet>(point_indices.size(), {geo_i}),
        point_indices_(point_indices),
        weights_(weights),
        rest_data_(rest_data),
        compliances_(compliances),
        error_scale_(error_scale),
        lambdas_(lambdas)
  {
  }

  void reset_force(const int constraint_i) const
  {
    lambdas_[constraint_i] = 0.0f;
  }

  template<typename UpdaterT>
  void solve_single(const ConstraintSetParams &params,
                    UpdaterT &updater,
                    const int constraint_i) const
  {
    if (math::abs(rest_data_[constraint_i].y) < FLAT_REST_ANGLE_THRESHOLD) {
      this->solve_single_flat(params, updater, constraint_i);
    }
    else {
      this->solve_single_rest_angle(params, updater, constraint_i);
    }
  }

  ConstraintColoring color_constraints(LinearAllocator<> &memory) const override
  {
    return color_constraints__quaternary(point_indices_, memory);
  }

 private:
  template<typename UpdaterT>
  void solve_single_flat(const ConstraintSetParams &params,
                         UpdaterT &updater,
                         const int constraint_i) const
  {
    const int geo_i = affected_geo_indices_[0];
    const int4 &points = point_indices_[constraint_i];
    const float4 &weights = weights_[constraint_i];

    float3 s(0.0f);
    float weights_sq_sum = 0.0f;
    for (const int i : IndexRange(4)) {
      s += weights[i] * params.position(geo_i, points[i]);
      weights_sq_sum += params.inv_mass(geo_i, points[i]) * math::square(weights[i]);
    }
    if (weights_sq_sum == 0.0f) {
      /* All points are pinned. */
      return;
    }

    /* C = |s| with dC/dx_i = K_i * s / |s|. For a planar stencil the direction degenerates to
     * zero: no position change, while the multiplier still decays. */
    float c;
    const float3 s_dir = math::normalize_and_get_length(s, c);
    const float compliance_term = compliances_[constraint_i] * params.compliance_term_factor;
    const float denom = weights_sq_sum + compliance_term;
    const float error = c + compliance_term * lambdas_[constraint_i];
    const float delta_lambda = -error / denom;
    lambdas_[constraint_i] += delta_lambda;
    const float3 correction = delta_lambda * s_dir;
    for (const int i : IndexRange(4)) {
      updater.update_position(
          geo_i, points[i], params.inv_mass(geo_i, points[i]) * weights[i] * correction);
    }
    updater.add_residual_error(geo_i, math::square(error) * error_scale_);
  }

  template<typename UpdaterT>
  void solve_single_rest_angle(const ConstraintSetParams &params,
                               UpdaterT &updater,
                               const int constraint_i) const
  {
    const int geo_i = affected_geo_indices_[0];
    const int4 &points = point_indices_[constraint_i];
    const float3 &x0 = params.position(geo_i, points[0]);
    const float3 &x1 = params.position(geo_i, points[1]);
    const float3 &x2 = params.position(geo_i, points[2]);
    const float3 &x3 = params.position(geo_i, points[3]);

    float3 grads[4];
    const std::optional<float> angle = dihedral_angle_and_gradient(x0, x1, x2, x3, grads);
    if (!angle) {
      return;
    }
    const float3 &rest = rest_data_[constraint_i];
    const float delta = wrap_angle_to_pi(*angle - rest.y);
    const float c = 2.0f * rest.x * math::sin(0.5f * delta);
    /* At the antipode (delta = +-pi) the constraint is maximally violated but the gradient
     * vanishes, comparable to a shell pushed exactly inside out. Corrections stay bounded by the
     * trust region below, but convergence is not guaranteed: with compliance the multiplier can
     * absorb the constraint force and the hinge stalls in the inverted state. */
    const float grad_scale = rest.x * math::cos(0.5f * delta);

    float weighted_grads_sq_sum = 0.0f;
    /* How far the most affected point moves per unit of delta lambda. */
    float displacement_per_lambda = 0.0f;
    for (const int i : IndexRange(4)) {
      grads[i] *= grad_scale;
      const float inv_mass = params.inv_mass(geo_i, points[i]);
      const float grad_length = math::length(grads[i]);
      weighted_grads_sq_sum += inv_mass * math::square(grad_length);
      displacement_per_lambda = math::max(displacement_per_lambda, inv_mass * grad_length);
    }
    if (weighted_grads_sq_sum == 0.0f) {
      /* All points pinned, or the gradient vanished at the antipode. Returning here also keeps
       * the denominator and the displacement below positive without absolute epsilons. */
      return;
    }
    const float compliance_term = compliances_[constraint_i] * params.compliance_term_factor;
    const float denom = weighted_grads_sq_sum + compliance_term;
    const float error = c + compliance_term * lambdas_[constraint_i];
    float delta_lambda = -error / denom;
    /* Trust region: linearized far from its solution the constraint can request corrections
     * orders of magnitude larger than the stencil, so limit every point's move, inverse mass
     * included, to a fraction of the rest edge length. Delta lambda itself is clamped so the
     * multiplier stays consistent with the applied correction, and unconditionally, because it
     * grows inversely with the masses so an absolute gate would fail exactly when needed. */
    const float limit = 0.1f * rest.z / displacement_per_lambda;
    delta_lambda = math::clamp(delta_lambda, -limit, limit);
    lambdas_[constraint_i] += delta_lambda;
    for (const int i : IndexRange(4)) {
      updater.update_position(
          geo_i, points[i], params.inv_mass(geo_i, points[i]) * delta_lambda * grads[i]);
    }
    updater.add_residual_error(geo_i, math::square(error) * error_scale_);
  }
};

}  // namespace blender::xpbd
