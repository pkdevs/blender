/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_linear_allocator.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"

#include "GEO_xpbd_constraint_isometric_bending.hh"
#include "GEO_xpbd_constraint_set_params.hh"
#include "GEO_xpbd_updater_gauss_seidel.hh"

namespace blender::xpbd::tests {

/* Irregular flat hinge: edge (x0, x1), wings x2, x3 on opposite sides. */
static const float3 FLAT_HINGE[4] = {
    {0.0f, 0.0f, 0.0f}, {1.0f, 0.1f, 0.0f}, {0.55f, 0.9f, 0.0f}, {0.45f, -0.85f, 0.0f}};

static float degrees_to_radians(const float degrees)
{
  return degrees * float(M_PI) / 180.0f;
}

static float3 rotate_point(const float3 &p,
                           const float3 &origin,
                           const float3 &axis,
                           const float angle)
{
  const float3 v = p - origin;
  const float c = math::cos(angle);
  const float s = math::sin(angle);
  return origin + v * c + math::cross(axis, v) * s + axis * math::dot(axis, v) * (1.0f - c);
}

/** Rotate the wing x3 about the shared edge so the hinge's dihedral becomes \a target_angle. */
static std::array<float3, 4> fold_hinge(const float target_angle)
{
  std::array<float3, 4> xs = {FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]};
  const float3 axis = math::normalize(xs[1] - xs[0]);
  const float current = *dihedral_angle(xs[0], xs[1], xs[2], xs[3]);
  xs[3] = rotate_point(xs[3], xs[0], axis, target_angle - current);
  return xs;
}

/** Owns the data for a single hinge constraint and runs the real solver code on it. */
struct SingleHinge {
  Array<float3> positions;
  Array<float> inv_masses;
  Array<int4> points;
  Array<float4> weights;
  Array<float3> rest_data;
  Array<float> compliances;
  Array<float> lambdas;
  Array<GeometryRef> refs;

  SingleHinge(const Span<float3> initial_positions,
              const float rest_angle,
              const float compliance = 0.0f,
              const Span<float> point_inv_masses = {})
      : positions(initial_positions),
        inv_masses(4, 1.0f),
        points({int4(0, 1, 2, 3)}),
        compliances({compliance}),
        lambdas({0.0f}),
        refs(1)
  {
    if (!point_inv_masses.is_empty()) {
      inv_masses.as_mutable_span().copy_from(point_inv_masses);
    }
    /* Weights and the stiffness scale always come from the flat reference stencil so that flat
     * and rest angle paths are comparable. */
    weights = Array<float4>(
        {*isometric_bending_weights(FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3])});
    const float scale = isometric_bending_stiffness_scale(
        FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]);
    rest_data = Array<float3>(
        {float3(scale, rest_angle, math::distance(FLAT_HINGE[0], FLAT_HINGE[1]))});
    refs[0].positions = positions;
    refs[0].inv_masses = inv_masses;
  }

  void solve(const int iterations, const float delta_time = 1.0f)
  {
    IsometricBendingConstraintSet constraint_set(
        0, points, weights, rest_data, compliances, 1.0f, lambdas);
    const ConstraintSetParams params(refs, delta_time);
    GaussSeidelUpdater updater(refs);
    for ([[maybe_unused]] const int i : IndexRange(iterations)) {
      constraint_set.solve_single(params, updater, 0);
    }
  }

  float angle() const
  {
    return *dihedral_angle(positions[0], positions[1], positions[2], positions[3]);
  }

  float max_coordinate() const
  {
    float result = 0.0f;
    for (const float3 &p : positions) {
      result = math::max(result, math::reduce_max(math::abs(p)));
    }
    return result;
  }

  bool is_finite() const
  {
    for (const float3 &p : positions) {
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        return false;
      }
    }
    return true;
  }
};

TEST(xpbd_isometric_bending, WeightsReference)
{
  const std::optional<float4> weights = isometric_bending_weights(
      FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]);
  ASSERT_TRUE(weights.has_value());
  EXPECT_NEAR((*weights)[0], 1.5212378f, 1e-4f);
  EXPECT_NEAR((*weights)[1], 1.5300058f, 1e-4f);
  EXPECT_NEAR((*weights)[2], -1.5694615f, 1e-4f);
  EXPECT_NEAR((*weights)[3], -1.4817821f, 1e-4f);
  /* The weights sum to zero and annihilate the rest positions. */
  EXPECT_NEAR((*weights)[0] + (*weights)[1] + (*weights)[2] + (*weights)[3], 0.0f, 1e-4f);
  float3 s(0.0f);
  for (const int i : IndexRange(4)) {
    s += (*weights)[i] * FLAT_HINGE[i];
  }
  EXPECT_NEAR(math::length(s), 0.0f, 1e-5f);
  EXPECT_NEAR(isometric_bending_stiffness_scale(
                  FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]),
              1.3196133f,
              1e-4f);
}

TEST(xpbd_isometric_bending, WeightsAffineInvariance)
{
  const float4 weights = *isometric_bending_weights(
      FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]);
  /* Arbitrary affine image (stretch + shear + rotation + translation): still zero. */
  const float3x3 linear(
      float3(1.7f, 0.3f, 0.1f), float3(-0.4f, 0.8f, 0.5f), float3(0.2f, -0.6f, 1.3f));
  const float3 translation(0.3f, -1.2f, 0.7f);
  float3 s(0.0f);
  for (const int i : IndexRange(4)) {
    s += weights[i] * (linear * FLAT_HINGE[i] + translation);
  }
  EXPECT_NEAR(math::length(s), 0.0f, 1e-5f);

  /* A coplanar but non-affine deformation of the stencil has nonzero constraint value. */
  std::array<float3, 4> xs = {FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]};
  xs[2] += float3(0.1f, 0.2f, 0.0f);
  s = float3(0.0f);
  for (const int i : IndexRange(4)) {
    s += weights[i] * xs[i];
  }
  EXPECT_GT(math::length(s), 0.01f);
}

TEST(xpbd_isometric_bending, DihedralAngleBasic)
{
  const std::optional<float> flat_angle = dihedral_angle(
      FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]);
  ASSERT_TRUE(flat_angle.has_value());
  EXPECT_NEAR(*flat_angle, 0.0f, 1e-6f);
  for (const float target : {0.5f, -1.2f, 2.8f}) {
    const std::array<float3, 4> xs = fold_hinge(target);
    EXPECT_NEAR(*dihedral_angle(xs[0], xs[1], xs[2], xs[3]), target, 1e-5f);
  }
}

TEST(xpbd_isometric_bending, DihedralGradientFiniteDifference)
{
  for (const float target : {0.5f, -2.0f, 2.6f}) {
    std::array<float3, 4> xs = fold_hinge(target);
    /* Also test a stretched and sheared variant of each folded state. */
    for (const bool stretched : {false, true}) {
      if (stretched) {
        for (float3 &x : xs) {
          x = float3(1.6f * x.x + 0.2f * x.y, 0.7f * x.y, 1.1f * x.z + 0.1f * x.x);
        }
      }
      float3 grads[4];
      const std::optional<float> angle = dihedral_angle_and_gradient(
          xs[0], xs[1], xs[2], xs[3], grads);
      ASSERT_TRUE(angle.has_value());
      const float eps = 1e-3f;
      for (const int i : IndexRange(4)) {
        for (const int axis : IndexRange(3)) {
          std::array<float3, 4> xp = xs;
          std::array<float3, 4> xm = xs;
          xp[i][axis] += eps;
          xm[i][axis] -= eps;
          const float numeric = (*dihedral_angle(xp[0], xp[1], xp[2], xp[3]) -
                                 *dihedral_angle(xm[0], xm[1], xm[2], xm[3])) /
                                (2.0f * eps);
          EXPECT_NEAR(grads[i][axis], numeric, 2e-2f * math::max(1.0f, math::abs(numeric)));
        }
      }
    }
  }
}

TEST(xpbd_isometric_bending, ProjectionConvergesToRestAngle)
{
  for (const float target : {0.5f, 2.0f, -2.1f}) {
    SingleHinge hinge(Span<float3>(FLAT_HINGE, 4), target);
    hinge.solve(400);
    EXPECT_TRUE(hinge.is_finite());
    EXPECT_NEAR(wrap_angle_to_pi(hinge.angle() - target), 0.0f, 1e-3f) << "target " << target;
  }
  /* Also with nonzero compliance and accumulated multiplier. */
  SingleHinge hinge(Span<float3>(FLAT_HINGE, 4), 2.0f, 1e-3f);
  hinge.solve(2000);
  EXPECT_NEAR(wrap_angle_to_pi(hinge.angle() - 2.0f), 0.0f, 1e-2f);
  EXPECT_TRUE(std::isfinite(hinge.lambdas[0]));
}

TEST(xpbd_isometric_bending, ProjectionWrapShortPath)
{
  const float target = degrees_to_radians(170.0f);
  SingleHinge hinge(fold_hinge(degrees_to_radians(-170.0f)), target);
  hinge.solve(1000);
  EXPECT_NEAR(hinge.angle(), target, 1e-3f);
  /* The short path is 20 degrees; unfolding the long way round would accumulate a much larger
   * multiplier. */
  EXPECT_LT(math::abs(hinge.lambdas[0]), 0.5f);
}

TEST(xpbd_isometric_bending, AntipodeStaysBounded)
{
  const float target = 0.5f;
  /* Near the antipode of the target: the trust region must prevent the catapult and the hinge
   * must escape and converge. */
  SingleHinge near_hinge(fold_hinge(target + float(M_PI) - 0.01f), target);
  near_hinge.solve(2000);
  EXPECT_TRUE(near_hinge.is_finite());
  EXPECT_LT(near_hinge.max_coordinate(), 10.0f);
  EXPECT_NEAR(wrap_angle_to_pi(near_hinge.angle() - target), 0.0f, 1e-2f);

  /* Exactly at the antipode there is no gradient direction. With zero compliance the hinge
   * either stays there or escapes through float rounding and converges; anything in between is
   * not acceptable. */
  SingleHinge exact_hinge(fold_hinge(target + float(M_PI)), target);
  exact_hinge.solve(2000);
  EXPECT_TRUE(exact_hinge.is_finite());
  EXPECT_LT(exact_hinge.max_coordinate(), 10.0f);
  const float residual = math::abs(wrap_angle_to_pi(exact_hinge.angle() - target));
  EXPECT_TRUE(residual > float(M_PI) - 0.01f || residual < 1e-2f)
      << "neither stationary nor converged: residual " << residual;
}

TEST(xpbd_isometric_bending, CompliantAntipodeStaysBounded)
{
  /* With compliance the multiplier can absorb the constraint force after a small bounded step
   * and the hinge stalls inverted. Only boundedness is guaranteed at the antipode, so this must
   * not assert convergence. */
  const float target = 0.5f;
  SingleHinge hinge(fold_hinge(target + float(M_PI)), target, 1e-3f);
  const float rest_edge_length = math::distance(FLAT_HINGE[0], FLAT_HINGE[1]);
  for ([[maybe_unused]] const int step : IndexRange(2000)) {
    const Array<float3> before = hinge.positions;
    hinge.solve(1);
    ASSERT_TRUE(hinge.is_finite());
    for (const int i : IndexRange(4)) {
      EXPECT_LE(math::distance(hinge.positions[i], before[i]), 0.1f * rest_edge_length * 1.01f);
    }
  }
  EXPECT_LT(hinge.max_coordinate(), 10.0f);
  EXPECT_TRUE(std::isfinite(hinge.lambdas[0]));
}

TEST(xpbd_isometric_bending, DegenerateHingesGuarded)
{
  /* Collapsed shared edge. */
  std::array<float3, 4> xs = {FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]};
  xs[1] = xs[0] + float3(1e-8f, 0.0f, 0.0f);
  EXPECT_FALSE(dihedral_angle(xs[0], xs[1], xs[2], xs[3]).has_value());

  /* Collapsed triangle: wing on the shared edge. */
  xs = {FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]};
  xs[2] = math::interpolate(xs[0], xs[1], 0.5f) + float3(0.0f, 1e-8f, 0.0f);
  EXPECT_FALSE(dihedral_angle(xs[0], xs[1], xs[2], xs[3]).has_value());

  /* The solver must leave a degenerate hinge untouched. */
  SingleHinge hinge(Span<float3>(xs.data(), 4), 0.5f);
  const Array<float3> before = hinge.positions;
  hinge.solve(10);
  for (const int i : IndexRange(4)) {
    EXPECT_EQ(hinge.positions[i], before[i]);
  }
  EXPECT_EQ(hinge.lambdas[0], 0.0f);
}

TEST(xpbd_isometric_bending, FlatAndRestAngleModelsAgreeUnderIsometry)
{
  const float4 weights = *isometric_bending_weights(
      FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]);
  const float scale = isometric_bending_stiffness_scale(
      FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]);
  for (const float angle_deg : {5.0f, 30.0f, 90.0f, 150.0f}) {
    const std::array<float3, 4> xs = fold_hinge(degrees_to_radians(angle_deg));
    float3 s(0.0f);
    for (const int i : IndexRange(4)) {
      s += weights[i] * xs[i];
    }
    const float c_flat = math::length(s);
    const float angle = *dihedral_angle(xs[0], xs[1], xs[2], xs[3]);
    const float c_rest_angle = math::abs(2.0f * scale * math::sin(0.5f * angle));
    EXPECT_NEAR(c_flat, c_rest_angle, 1e-4f * math::max(1.0f, c_flat));
  }
}

TEST(xpbd_isometric_bending, PathSwitchGapBoundedAcrossScales)
{
  for (const float scene_scale : {0.01f, 1.0f, 100.0f}) {
    std::array<float3, 4> rest;
    for (const int i : IndexRange(4)) {
      rest[i] = scene_scale * FLAT_HINGE[i];
    }
    const float4 weights = *isometric_bending_weights(rest[0], rest[1], rest[2], rest[3]);
    const float stiffness = isometric_bending_stiffness_scale(rest[0], rest[1], rest[2], rest[3]);
    std::array<float3, 4> xs = fold_hinge(degrees_to_radians(3.0f));
    for (float3 &x : xs) {
      x *= scene_scale;
    }
    float3 s(0.0f);
    for (const int i : IndexRange(4)) {
      s += weights[i] * xs[i];
    }
    const float c_flat = math::length(s);
    for (const float threshold : {1e-3f, 1e-2f}) {
      const float angle = *dihedral_angle(xs[0], xs[1], xs[2], xs[3]);
      const float c_rest_angle = math::abs(2.0f * stiffness *
                                           math::sin(0.5f * wrap_angle_to_pi(angle - threshold)));
      EXPECT_LT(math::abs(c_flat - c_rest_angle),
                2.0f * stiffness * threshold + 1e-4f * stiffness);
    }
  }
}

TEST(xpbd_isometric_bending, PinnedPointsDoNotMove)
{
  const std::array<float, 4> all_pinned = {0.0f, 0.0f, 0.0f, 0.0f};
  SingleHinge hinge(fold_hinge(1.0f), 0.5f, 0.0f, all_pinned);
  const Array<float3> before = hinge.positions;
  hinge.solve(50);
  for (const int i : IndexRange(4)) {
    EXPECT_EQ(hinge.positions[i], before[i]);
  }

  const std::array<float, 4> partial = {0.0f, 1.0f, 1.0f, 1.0f};
  SingleHinge partial_hinge(fold_hinge(1.0f), 0.5f, 0.0f, partial);
  const float3 pinned_before = partial_hinge.positions[0];
  partial_hinge.solve(200);
  EXPECT_TRUE(partial_hinge.is_finite());
  EXPECT_EQ(partial_hinge.positions[0], pinned_before);
  EXPECT_NEAR(wrap_angle_to_pi(partial_hinge.angle() - 0.5f), 0.0f, 1e-3f);
}

TEST(xpbd_isometric_bending, ClampAccountsForInverseMass)
{
  /* A very light point must not be catapulted: the trust region has to include the inverse
   * mass factor of the position update. */
  const std::array<float, 4> inv_masses = {100.0f, 1.0f, 0.0f, 0.0f};
  SingleHinge hinge(fold_hinge(0.5f + float(M_PI) - 0.1f), 0.5f, 0.0f, inv_masses);
  const float rest_edge_length = math::distance(FLAT_HINGE[0], FLAT_HINGE[1]);
  for ([[maybe_unused]] const int step : IndexRange(50)) {
    const Array<float3> before = hinge.positions;
    hinge.solve(1);
    ASSERT_TRUE(hinge.is_finite());
    for (const int i : IndexRange(4)) {
      EXPECT_LE(math::distance(hinge.positions[i], before[i]), 0.1f * rest_edge_length * 1.01f);
    }
  }
}

TEST(xpbd_isometric_bending, ClampAppliesForTinyInverseMass)
{
  /* Very heavy points are the mirror case: delta lambda grows inversely with the masses, so
   * the clamp must not have an absolute activation threshold. */
  const std::array<float, 4> inv_masses = {1e-12f, 1e-12f, 1e-12f, 1e-12f};
  const float target = 0.5f;
  SingleHinge hinge(fold_hinge(degrees_to_radians(170.0f)), target, 0.0f, inv_masses);
  const float rest_edge_length = math::distance(FLAT_HINGE[0], FLAT_HINGE[1]);
  const float initial_residual = math::abs(wrap_angle_to_pi(hinge.angle() - target));
  for ([[maybe_unused]] const int step : IndexRange(50)) {
    const Array<float3> before = hinge.positions;
    hinge.solve(1);
    ASSERT_TRUE(hinge.is_finite());
    for (const int i : IndexRange(4)) {
      EXPECT_LE(math::distance(hinge.positions[i], before[i]), 0.1f * rest_edge_length * 1.01f);
    }
  }
  /* The clamp must bound the steps, not freeze the hinge. */
  EXPECT_LT(math::abs(wrap_angle_to_pi(hinge.angle() - target)), initial_residual);
}

TEST(xpbd_isometric_bending, ProjectionConvergesAcrossSceneScales)
{
  for (const float scene_scale : {1e-6f, 0.01f, 100.0f}) {
    std::array<float3, 4> xs = fold_hinge(0.1f);
    for (float3 &x : xs) {
      x *= scene_scale;
    }
    /* A geometrically valid hinge must be accepted at any scale: the degeneracy guards are
     * relative to the stencil size, so acceptance must not depend on the scene scale. */
    ASSERT_TRUE(isometric_bending_weights(xs[0], xs[1], xs[2], xs[3]).has_value())
        << "scene scale " << scene_scale;
    ASSERT_TRUE(dihedral_angle(xs[0], xs[1], xs[2], xs[3]).has_value())
        << "scene scale " << scene_scale;
    SingleHinge hinge(Span<float3>(xs.data(), 4), 2.0f);
    /* Scale dependent data has to be rebuilt for the scaled stencil. */
    const float scale = isometric_bending_stiffness_scale(xs[0], xs[1], xs[2], xs[3]);
    hinge.rest_data[0] = float3(scale, 2.0f, math::distance(xs[0], xs[1]));
    hinge.solve(1000);
    EXPECT_TRUE(hinge.is_finite());
    EXPECT_NEAR(wrap_angle_to_pi(hinge.angle() - 2.0f), 0.0f, 1e-3f)
        << "scene scale " << scene_scale;
  }
}

TEST(xpbd_isometric_bending, DegenerateRestStencilsGuarded)
{
  /* Gather relies on these functions rejecting degenerate REST geometry so that no stencil with
   * non finite precomputed data is ever created. */
  std::array<float3, 4> rest = {FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]};
  rest[2] = math::interpolate(rest[0], rest[1], 0.5f);
  EXPECT_FALSE(isometric_bending_weights(rest[0], rest[1], rest[2], rest[3]).has_value());
  EXPECT_FALSE(dihedral_angle(rest[0], rest[1], rest[2], rest[3]).has_value());
  rest = {FLAT_HINGE[0], FLAT_HINGE[0], FLAT_HINGE[2], FLAT_HINGE[3]};
  EXPECT_FALSE(isometric_bending_weights(rest[0], rest[1], rest[2], rest[3]).has_value());
  EXPECT_FALSE(dihedral_angle(rest[0], rest[1], rest[2], rest[3]).has_value());

  /* The relative guards must keep rejecting degenerate stencils at a tiny scene scale, and must
   * reject a stencil collapsed to a single point. */
  rest = {FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]};
  rest[2] = math::interpolate(rest[0], rest[1], 0.5f);
  for (float3 &p : rest) {
    p *= 1e-6f;
  }
  EXPECT_FALSE(isometric_bending_weights(rest[0], rest[1], rest[2], rest[3]).has_value());
  EXPECT_FALSE(dihedral_angle(rest[0], rest[1], rest[2], rest[3]).has_value());
  rest = {FLAT_HINGE[0], FLAT_HINGE[0], FLAT_HINGE[0], FLAT_HINGE[0]};
  EXPECT_FALSE(isometric_bending_weights(rest[0], rest[1], rest[2], rest[3]).has_value());
  EXPECT_FALSE(dihedral_angle(rest[0], rest[1], rest[2], rest[3]).has_value());
}

TEST(xpbd_isometric_bending, StencilOrderCovariance)
{
  /* The constraint value must be invariant under any consistent relabeling or mirroring of a
   * hinge, since the gather stores whichever labeling the mesh happens to produce. */
  const std::array<float3, 4> rest = {FLAT_HINGE[0], FLAT_HINGE[1], FLAT_HINGE[2], FLAT_HINGE[3]};
  const std::array<float3, 4> bent = fold_hinge(1.1f);
  const auto constraint_value = [](const std::array<float3, 4> &r,
                                   const std::array<float3, 4> &x) {
    const float scale = isometric_bending_stiffness_scale(r[0], r[1], r[2], r[3]);
    const float rest_angle = *dihedral_angle(r[0], r[1], r[2], r[3]);
    const float angle = *dihedral_angle(x[0], x[1], x[2], x[3]);
    return math::abs(2.0f * scale * math::sin(0.5f * wrap_angle_to_pi(angle - rest_angle)));
  };
  const float reference = constraint_value(rest, bent);
  const auto relabel = [](const std::array<float3, 4> &x, const std::array<int, 4> &order) {
    return std::array<float3, 4>{x[order[0]], x[order[1]], x[order[2]], x[order[3]]};
  };
  for (const std::array<int, 4> &order : {std::array<int, 4>{1, 0, 2, 3},
                                          std::array<int, 4>{0, 1, 3, 2},
                                          std::array<int, 4>{1, 0, 3, 2}})
  {
    EXPECT_NEAR(constraint_value(relabel(rest, order), relabel(bent, order)), reference, 1e-4f);
  }
  const auto mirror = [](std::array<float3, 4> x) {
    for (float3 &p : x) {
      p.z = -p.z;
    }
    return x;
  };
  EXPECT_NEAR(constraint_value(mirror(rest), mirror(bent)), reference, 1e-4f);
}

TEST(xpbd_isometric_bending, PathSwitchProductionBranch)
{
  /* The threshold is a model switch, not a continuous blend: the two paths' corrections are
   * not identical, but they must be finite and directionally consistent. */
  const float threshold = IsometricBendingConstraintSet::FLAT_REST_ANGLE_THRESHOLD;
  const std::array<float3, 4> start = fold_hinge(degrees_to_radians(3.0f));
  SingleHinge below(Span<float3>(start.data(), 4), 0.99f * threshold);
  SingleHinge above(Span<float3>(start.data(), 4), 1.01f * threshold);
  below.solve(1);
  above.solve(1);
  ASSERT_TRUE(below.is_finite());
  ASSERT_TRUE(above.is_finite());
  float direction_dot = 0.0f;
  for (const int i : IndexRange(4)) {
    direction_dot += math::dot(below.positions[i] - start[i], above.positions[i] - start[i]);
  }
  EXPECT_GT(direction_dot, 0.0f);
}

TEST(xpbd_isometric_bending, QuaternaryColoringHasNoConflicts)
{
  /* Stencils of a 2 x 4 vertex strip: neighbors share up to three points. */
  const Array<int4> stencils = {
      int4(0, 3, 1, 2), int4(2, 3, 0, 5), int4(2, 5, 3, 4), int4(4, 5, 2, 7), int4(4, 7, 5, 6)};
  LinearAllocator<> allocator;
  const ConstraintColoring coloring = color_constraints__quaternary(stencils, allocator);
  int total = 0;
  for (const IndexMask &mask : coloring.colors) {
    Set<int> used_points;
    mask.foreach_index([&](const int64_t constraint_i) {
      total++;
      for (const int axis : IndexRange(4)) {
        EXPECT_TRUE(used_points.add(stencils[constraint_i][axis]))
            << "point " << stencils[constraint_i][axis] << " used twice in one color";
      }
    });
  }
  EXPECT_EQ(total, stencils.size());
}

}  // namespace blender::xpbd::tests
