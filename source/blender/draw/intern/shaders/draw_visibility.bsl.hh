/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * Compute visibility of each resource bounds for a given view, and (for resources that draw
 * meshlets) the visibility of each of their meshlets against the meshlet's own bounds.
 */

/* TODO(fclem): This could be augmented by a 2 pass occlusion culling system. */

#pragma once

#include "draw_intersect.bsl.hh"

namespace draw {

struct Visibility {
  [[storage(0, read)]] ObjectBounds (&bounds_buf)[];
  [[storage(1, read_write)]] uint (&visibility_buf)[];
  [[storage(2, read)]] DrawMeshlet (&meshlet_buf)[];
  [[storage(3, read)]] MeshletObject (&meshlet_object_buf)[];
  [[storage(4, read)]] ObjectMatrices (&matrix_buf)[];
  [[storage(5, read_write)]] uint (&meshlet_visibility_buf)[];

  [[push_constant]] int resource_len;
  [[push_constant]] int view_len;
  [[push_constant]] int visibility_word_per_draw;
  /** Stride (in meshlet bits) between a resource's meshlet bits; equals the max meshlet count. */
  [[push_constant]] int meshlet_stride;

  void mask_visibility_bit(const uint3 global_id,
                           const uint3 local_id,
                           const uint3 workgroup_id,
                           uint view_id)
  {
    if (view_len > 1) {
      uint index = global_id.x * uint(visibility_word_per_draw) + (view_id / 32u);
      visibility_buf[index] &= ~(1u << (view_id & 31u));
    }
    else {
      atomicAnd(visibility_buf[workgroup_id.x], ~(1u << local_id.x));
    }
  }

  void mask_meshlet_bit(const uint bit_index)
  {
    atomicAnd(meshlet_visibility_buf[bit_index / 32u], ~(1u << (bit_index % 32u)));
  }
};

/**
 * Cull one meshlet for a single view against its own bounds. \a meshlet carries the quantized
 * object-space AABB; \a object carries the box those bounds are quantized against and the
 * resource's model matrix is in \a matrices. Returns true if the meshlet intersects the view.
 */
bool meshlet_visible_in_view(const DrawMeshlet &meshlet,
                             const MeshletObject &object,
                             const ObjectMatrices &matrices,
                             const ViewCulling &culling,
                             uint view_id)
{
  if (culling.get(view_id).bound_sphere.w == -1.0f) {
    /* View disabled. */
    return false;
  }

  float3 local_min, local_max;
  drw_meshlet_bounds_unpack(meshlet.packed_bounds,
                            object.bounds_min,
                            object.bounds_max,
                            local_min,
                            local_max);

  const float4x4 model = matrices.model;

  /* World-space bounding sphere; the radius is scaled by the model's largest column length so
   * the test stays valid under non-uniform scale. */
  float3 local_center = (local_min + local_max) * 0.5f;
  float local_radius = length((local_max - local_min) * 0.5f);
  float3 world_center = (model * float4(local_center, 1.0f)).xyz;
  float model_scale = max(length(model[0].xyz), max(length(model[1].xyz), length(model[2].xyz)));
  Sphere world_sphere = shape_sphere(world_center, local_radius * model_scale);
  if (culling.intersect_view(world_sphere, view_id) == false) {
    return false;
  }

  /* World-space oriented box, tested with the same routine as the per-resource box. */
  float3 origin = (model * float4(local_min, 1.0f)).xyz;
  float3 side_x = (model * float4(local_max.x - local_min.x, 0.0f, 0.0f, 0.0f)).xyz;
  float3 side_y = (model * float4(0.0f, local_max.y - local_min.y, 0.0f, 0.0f)).xyz;
  float3 side_z = (model * float4(0.0f, 0.0f, local_max.z - local_min.z, 0.0f)).xyz;
  IsectBox box = isect_box_setup(origin, side_x, side_y, side_z);
  if (culling.intersect_view(box, view_id) == false) {
    return false;
  }

  return true;
}

[[compute, local_size(DRW_VISIBILITY_GROUP_SIZE)]]
void visibility([[resource_table]] Visibility &srt,
                [[resource_table]] const ViewCulling &culling,
                [[global_invocation_id]] const uint3 global_id,
                [[local_invocation_id]] const uint3 local_id,
                [[work_group_id]] const uint3 workgroup_id)
{
  if (int(global_id.x) >= srt.resource_len) {
    return;
  }

  ObjectBounds bounds = srt.bounds_buf[global_id.x];
  /* no per-resource load here; only split meshes (below) need the `MeshletObject`. */
  bool bounds_valid = drw_bounds_are_valid(bounds);

  IsectBox box;
  Sphere bounding_sphere;
  Sphere inscribed_sphere;
  if (bounds_valid) {
    box = isect_box_setup(bounds.bounding_corners[0].xyz,
                          bounds.bounding_corners[1].xyz,
                          bounds.bounding_corners[2].xyz,
                          bounds.bounding_corners[3].xyz);
    bounding_sphere = shape_sphere(bounds.bounding_sphere.xyz, bounds.bounding_sphere.w);
    inscribed_sphere = shape_sphere(bounds.bounding_sphere.xyz, bounds._inner_sphere_radius);
  }

  for (uint view_id = 0u; view_id < uint(srt.view_len); view_id++) {
    bool cull_resource = false;
    if (culling.get(view_id).bound_sphere.w == -1.0f) {
      /* View disabled. */
      cull_resource = true;
    }
    else if (bounds_valid) {
      if (culling.intersect_view(inscribed_sphere, view_id) == false) {
        if (culling.intersect_view(bounding_sphere, view_id) == false ||
            culling.intersect_view(box, view_id) == false) {
          cull_resource = true;
        }
      }
    }

    /* Per-resource bit, unchanged from the object-level pass. */
    if (cull_resource) {
      srt.mask_visibility_bit(global_id, local_id, workgroup_id, view_id);
    }

    /* Per-meshlet bits, only for split meshes. A full-span meshlet decodes to the resource's
     * own bounds box, so its verdict is the per-resource verdict above and its bit keeps the
     * clear value — no load, no test, no mask. At stride 1 (no split mesh in the frame) this
     * section is dead code: no `MeshletObject` load, no `atomicAnd`. */
    if (srt.meshlet_stride > 1 && bounds_valid) {
      MeshletObject object = srt.meshlet_object_buf[global_id.x];
      if (object.meshlet_count > 1) {
        uint base_bit = global_id.x * uint(srt.meshlet_stride);
        if (cull_resource) {
          /* Every meshlet lies inside the resource's bounds, which miss the frustum. */
          for (uint m = 0; m < object.meshlet_count; m++) {
            srt.mask_meshlet_bit(base_bit + m);
          }
        }
        else {
          /* Resource not culled and the mesh is split: test each meshlet against its own
           * bounds. */
          ObjectMatrices matrices = srt.matrix_buf[global_id.x];
          for (uint m = 0; m < object.meshlet_count; m++) {
            DrawMeshlet meshlet = srt.meshlet_buf[object.meshlet_offset + m];
            /* Empty meshlets draw nothing; leave them visible. */
            if (meshlet.vertex_count != 0 &&
                meshlet_visible_in_view(meshlet, object, matrices, culling, view_id) == false) {
              srt.mask_meshlet_bit(base_bit + m);
            }
          }
        }
      }
    }
  }
}

PipelineCompute visibility_compute(visibility);

}  // namespace draw
