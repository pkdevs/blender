/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_straight_skeleton.hh"

namespace blender::straightskeleton {

/**
 * Compute the straight skeleton using the kinetic Delaunay triangulation algorithm
 * (Palfrader, Held, Huber 2012).
 */
StraightSkeletonOutput straight_skeleton_calc_kinetic(const StraightSkeletonInput &input);

/**
 * Compute the straight skeleton using the motorcycle graph algorithm
 * (Eppstein & Erickson 1999, Huber & Held 2011).
 */
StraightSkeletonOutput straight_skeleton_calc_motorcycle(const StraightSkeletonInput &input);

/**
 * Compute the straight skeleton using the step-by-step wavefront simulation algorithm
 * with kdtree spatial acceleration.
 */
StraightSkeletonOutput straight_skeleton_calc_simulation(const StraightSkeletonInput &input);

}  // namespace blender::straightskeleton
