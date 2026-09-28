/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * Copy mesh data from vertices, edges, or faces to face corners on the GPU, so that only the
 * smaller source domain has to be uploaded from the host. The data is copied as opaque 32-bit
 * words, so any vertex format with a stride that is a multiple of 4 bytes is supported without
 * type specific shaders.
 */

#pragma once

#include "draw_defines.hh"

#include "gpu_shader_compat.hh"

namespace draw::mesh {

struct GatherResources {
  [[storage(0, read)]] const uint (&src)[];
  /** The source element for each face corner, e.g. #Mesh::corner_verts(). */
  [[storage(1, read)]] const int (&indices)[];
  [[storage(2, write)]] uint (&dst)[];

  [[push_constant]] const int indices_num;
  [[push_constant]] const int words_per_element;
};

/** `dst[i] = src[indices[i]]`. */
[[compute, local_size(DRW_MESH_TO_CORNER_GROUP_SIZE)]]
void gather_main([[resource_table]] GatherResources &srt,
                 [[global_invocation_id]] const uint3 global_id,
                 [[num_work_groups]] const uint3 num_work_groups)
{
  const uint words_num = uint(srt.words_per_element);
  const uint stride = num_work_groups.x * DRW_MESH_TO_CORNER_GROUP_SIZE;
  for (uint i = global_id.x; i < uint(srt.indices_num); i += stride) {
    const uint src_start = uint(srt.indices[i]) * words_num;
    const uint dst_start = i * words_num;
    for (uint word = 0u; word < words_num; word++) {
      srt.dst[dst_start + word] = srt.src[src_start + word];
    }
  }
}

struct ScatterFacesResources {
  [[storage(0, read)]] const uint (&src)[];
  /** The start of each face's range of corners, e.g. #Mesh::face_offsets(). */
  [[storage(1, read)]] const int (&face_offsets)[];
  [[storage(2, write)]] uint (&dst)[];

  [[push_constant]] const int faces_num;
  [[push_constant]] const int words_per_element;
};

/** Copy each face's value to all of its corners. One thread processes one face. */
[[compute, local_size(DRW_MESH_TO_CORNER_GROUP_SIZE)]]
void scatter_faces_main([[resource_table]] ScatterFacesResources &srt,
                        [[global_invocation_id]] const uint3 global_id,
                        [[num_work_groups]] const uint3 num_work_groups)
{
  const uint words_num = uint(srt.words_per_element);
  const uint stride = num_work_groups.x * DRW_MESH_TO_CORNER_GROUP_SIZE;
  for (uint face = global_id.x; face < uint(srt.faces_num); face += stride) {
    const uint src_start = face * words_num;
    const uint corner_start = uint(srt.face_offsets[face]);
    const uint corner_end = uint(srt.face_offsets[face + 1]);
    for (uint corner = corner_start; corner < corner_end; corner++) {
      const uint dst_start = corner * words_num;
      for (uint word = 0u; word < words_num; word++) {
        srt.dst[dst_start + word] = srt.src[src_start + word];
      }
    }
  }
}

PipelineCompute gather(gather_main);
PipelineCompute scatter_faces(scatter_faces_main);

}  // namespace draw::mesh
