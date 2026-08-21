/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_math_vector.hh"

#include "GEO_xpbd_constraint_damping_linear.hh"
#include "GEO_xpbd_constraint_set_params.hh"
#include "GEO_xpbd_updater_velocity.hh"

namespace blender::xpbd::tests {

TEST(xpbd_linear_damping, SkipsZeroInverseMass)
{
  Array<float3> positions(2, float3(0.0f));
  Array<float3> velocities(2, float3(1.0f, 0.0f, 0.0f));
  Array<float> inv_masses({0.0f, 1.0f});
  Array<GeometryRef> refs(1);
  refs[0].positions = positions;
  refs[0].velocities = velocities;
  refs[0].inv_masses = inv_masses;

  Array<float> dampings(2, 10.0f);
  Array<float> lambdas(2, 0.0f);
  LinearDampingConstraintSet constraint_set(0, IndexRange(2), dampings, lambdas);
  const ConstraintSetParams params(refs, 0.05f);
  VelocityUpdater updater(refs);
  constraint_set.solve_single(params, updater, 0);
  constraint_set.solve_single(params, updater, 1);

  /* Damping is a drag force and cannot change the velocity of a point with zero inverse mass;
   * a kinematic hard pinned point in particular keeps the pin target's motion. */
  EXPECT_EQ(velocities[0], float3(1.0f, 0.0f, 0.0f));
  /* The movable point is damped by delta_time * damping. */
  EXPECT_NEAR(velocities[1].x, 0.5f, 1e-6f);
  EXPECT_NEAR(velocities[1].y, 0.0f, 1e-6f);
  EXPECT_NEAR(velocities[1].z, 0.0f, 1e-6f);
}

}  // namespace blender::xpbd::tests
