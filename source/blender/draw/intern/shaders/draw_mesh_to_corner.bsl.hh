/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * Copy mesh data from vertices, edges, or faces to face corners on the GPU, so that only the
 * smaller source domain has to be uploaded from the host. The data is copied as opaque 32-bit
 * words, so any vertex format with a stride of 4, 8, 12, or 16 bytes is supported without type
 * specific shaders.
 */

#pragma once

#include "draw_defines.hh"

#include "gpu_shader_compat.hh"

namespace draw::mesh {

/**
 * Elements of each supported size. Use integers instead of floats so bit patterns of other types
 * that happen to be denormal floats (e.g. packed normals) are never flushed to zero.
 */
struct Bytes4 {
  uint x;
};
struct Bytes8 {
  uint x, y;
};
struct Bytes12 {
  uint x, y, z;
};
struct Bytes16 {
  uint x, y, z, w;
};

/**
 * Both shaders process a batch of `count` elements starting at `start`, to stay within the
 * potentially limited GPU work group count.
 */

template<typename T> struct GatherResources {
  [[storage(0, read)]] const T (&src)[];
  /** The source element for each face corner, e.g. #Mesh::corner_verts(). */
  [[storage(1, read)]] const int (&indices)[];
  [[storage(2, write)]] T (&dst)[];

  [[push_constant]] const int start;
  [[push_constant]] const int count;
};

template struct GatherResources<Bytes4>;
template struct GatherResources<Bytes8>;
template struct GatherResources<Bytes12>;
template struct GatherResources<Bytes16>;

/** `dst[i] = src[indices[i]]`. One thread processes one face corner. */
template<typename T>
[[compute, local_size(DRW_MESH_TO_CORNER_GROUP_SIZE)]]
void gather_main([[resource_table]] GatherResources<T> &srt,
                 [[global_invocation_id]] const uint3 global_id)
{
  if (global_id.x >= uint(srt.count)) {
    return;
  }
  const uint i = uint(srt.start) + global_id.x;
  srt.dst[i] = srt.src[srt.indices[i]];
}

template void gather_main<Bytes4>(GatherResources<Bytes4> &, const uint3);
template void gather_main<Bytes8>(GatherResources<Bytes8> &, const uint3);
template void gather_main<Bytes12>(GatherResources<Bytes12> &, const uint3);
template void gather_main<Bytes16>(GatherResources<Bytes16> &, const uint3);

template<typename T> struct ScatterFacesResources {
  [[storage(0, read)]] const T (&src)[];
  /** The start of each face's range of corners, e.g. #Mesh::face_offsets(). */
  [[storage(1, read)]] const uint (&face_offsets)[];
  [[storage(2, write)]] T (&dst)[];

  [[push_constant]] const int start;
  [[push_constant]] const int count;
};

template struct ScatterFacesResources<Bytes4>;
template struct ScatterFacesResources<Bytes8>;
template struct ScatterFacesResources<Bytes12>;
template struct ScatterFacesResources<Bytes16>;

/** Copy each face's value to all of its corners. One thread processes one face. */
template<typename T>
[[compute, local_size(DRW_MESH_TO_CORNER_GROUP_SIZE)]]
void scatter_faces_main([[resource_table]] ScatterFacesResources<T> &srt,
                        [[global_invocation_id]] const uint3 global_id)
{
  if (global_id.x >= uint(srt.count)) {
    return;
  }
  const uint face = uint(srt.start) + global_id.x;
  const T value = srt.src[face];
  for (uint corner = srt.face_offsets[face]; corner < srt.face_offsets[face + 1]; corner++) {
    srt.dst[corner] = value;
  }
}

template void scatter_faces_main<Bytes4>(ScatterFacesResources<Bytes4> &, const uint3);
template void scatter_faces_main<Bytes8>(ScatterFacesResources<Bytes8> &, const uint3);
template void scatter_faces_main<Bytes12>(ScatterFacesResources<Bytes12> &, const uint3);
template void scatter_faces_main<Bytes16>(ScatterFacesResources<Bytes16> &, const uint3);

PipelineCompute gather_4(gather_main<Bytes4>);
PipelineCompute gather_8(gather_main<Bytes8>);
PipelineCompute gather_12(gather_main<Bytes12>);
PipelineCompute gather_16(gather_main<Bytes16>);
PipelineCompute scatter_faces_4(scatter_faces_main<Bytes4>);
PipelineCompute scatter_faces_8(scatter_faces_main<Bytes8>);
PipelineCompute scatter_faces_12(scatter_faces_main<Bytes12>);
PipelineCompute scatter_faces_16(scatter_faces_main<Bytes16>);

}  // namespace draw::mesh
