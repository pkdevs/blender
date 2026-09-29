/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#pragma once

namespace blender {

struct PointCloud;
namespace gpu {
class Batch;
class VertBuf;
}  // namespace gpu
struct GPUMaterial;

namespace draw {

struct Mesh;

gpu::VertBuf *pointcloud_position_and_radius_get(PointCloud *pointcloud);
draw::Mesh **pointcloud_surface_shaded_get(PointCloud *pointcloud,
                                           GPUMaterial **gpu_materials,
                                           int mat_len);
draw::Mesh *pointcloud_surface_get(PointCloud *pointcloud);

}  // namespace draw
}  // namespace blender
