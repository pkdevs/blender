/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup geo
 */

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"

#include "GEO_mesh_primitive_grid.hh"

namespace blender::geometry {

BLI_NOINLINE static void calculate_positions(const int verts_x,
                                             const int verts_y,
                                             const float size_x,
                                             const float size_y,
                                             MutableSpan<float3> positions)
{
  const int edges_x = verts_x - 1;
  const int edges_y = verts_y - 1;
  const float dx = edges_x == 0 ? 0.0f : size_x / edges_x;
  const float dy = edges_y == 0 ? 0.0f : size_y / edges_y;
  const float x_shift = edges_x / 2.0f;
  const float y_shift = edges_y / 2.0f;
  const int64_t x_grain = std::max<int64_t>(1, 4096 / std::max(verts_y, 1));

  threading::parallel_for(IndexRange(verts_x), x_grain, [&](const IndexRange x_range) {
    for (const int x : x_range) {
      const float x_pos = (x - x_shift) * dx;
      const int column_offset = x * verts_y;
      for (const int y : IndexRange(verts_y)) {
        positions[column_offset + y] = float3(x_pos, (y - y_shift) * dy, 0.0f);
      }
    }
  });
}

BLI_NOINLINE static void calculate_y_edges(const int verts_x,
                                           const int verts_y,
                                           MutableSpan<int2> edges)
{
  const int edges_y = verts_y - 1;
  const int64_t x_grain = std::max<int64_t>(1, 4096 / std::max(edges_y, 1));

  threading::parallel_for(IndexRange(verts_x), x_grain, [&](const IndexRange x_range) {
    for (const int x : x_range) {
      const int y_vert_offset = x * verts_y;
      const int y_edge_offset = x * edges_y;
      for (const int y : IndexRange(edges_y)) {
        const int vert = y_vert_offset + y;
        edges[y_edge_offset + y] = int2(vert, vert + 1);
      }
    }
  });
}

BLI_NOINLINE static void calculate_x_edges(const int verts_x,
                                           const int verts_y,
                                           MutableSpan<int2> edges)
{
  const int edges_x = verts_x - 1;
  const int edges_y = verts_y - 1;
  const int x_edges_start = verts_x * edges_y;
  const int64_t y_grain = std::max<int64_t>(1, 4096 / std::max(edges_x, 1));

  threading::parallel_for(IndexRange(verts_y), y_grain, [&](const IndexRange y_range) {
    for (const int y : y_range) {
      const int x_edge_offset = x_edges_start + y * edges_x;
      for (const int x : IndexRange(edges_x)) {
        const int vert = x * verts_y + y;
        edges[x_edge_offset + x] = int2(vert, vert + verts_y);
      }
    }
  });
}

BLI_NOINLINE static void calculate_corners(const int verts_x,
                                           const int verts_y,
                                           MutableSpan<int> corner_verts,
                                           MutableSpan<int> corner_edges)
{
  const int edges_x = verts_x - 1;
  const int edges_y = verts_y - 1;
  if (edges_x < 1 || edges_y < 1) {
    return;
  }

  const int y_edges_start = 0;
  const int x_edges_start = verts_x * edges_y;
  const int64_t x_grain = std::max<int64_t>(1, 4096 / std::max(edges_y, 1));

  threading::parallel_for(IndexRange(edges_x), x_grain, [&](const IndexRange x_range) {
    for (const int x : x_range) {
      const int y_offset = x * edges_y;
      for (const int y : IndexRange(edges_y)) {
        const int face = y_offset + y;
        const int corner = face * 4;
        const int vert = x * verts_y + y;

        corner_verts[corner] = vert;
        corner_edges[corner] = x_edges_start + edges_x * y + x;

        corner_verts[corner + 1] = vert + verts_y;
        corner_edges[corner + 1] = y_edges_start + edges_y * (x + 1) + y;

        corner_verts[corner + 2] = vert + verts_y + 1;
        corner_edges[corner + 2] = x_edges_start + edges_x * (y + 1) + x;

        corner_verts[corner + 3] = vert + 1;
        corner_edges[corner + 3] = y_edges_start + edges_y * x + y;
      }
    }
  });
}

BLI_NOINLINE static void calculate_uvs(const int verts_x,
                                       const int verts_y,
                                       const float size_x,
                                       const float size_y,
                                       MutableSpan<float2> uvs)
{
  const int edges_x = verts_x - 1;
  const int edges_y = verts_y - 1;
  const float u_scale = (size_x == 0.0f) ? 0.0f : 1.0f / float(edges_x);
  const float v_scale = (size_y == 0.0f) ? 0.0f : 1.0f / float(edges_y);
  const int64_t x_grain = std::max<int64_t>(1, 4096 / std::max(edges_y, 1));

  threading::parallel_for(IndexRange(edges_x), x_grain, [&](const IndexRange x_range) {
    for (const int x : x_range) {
      const float u = float(x) * u_scale;
      const float u_next = float(x + 1) * u_scale;
      const int y_offset = x * edges_y;
      for (const int y : IndexRange(edges_y)) {
        const int corner = (y_offset + y) * 4;
        const float v = float(y) * v_scale;
        const float v_next = float(y + 1) * v_scale;
        uvs[corner] = float2(u, v);
        uvs[corner + 1] = float2(u_next, v);
        uvs[corner + 2] = float2(u_next, v_next);
        uvs[corner + 3] = float2(u, v_next);
      }
    }
  });
}

Mesh *create_grid_mesh(const int verts_x,
                       const int verts_y,
                       const float size_x,
                       const float size_y,
                       const std::optional<StringRef> uv_map_id)
{
  BLI_assert(verts_x > 0 && verts_y > 0);
  const int edges_x = verts_x - 1;
  const int edges_y = verts_y - 1;
  const int verts_num = verts_x * verts_y;
  const int edges_num = edges_x * verts_y + edges_y * verts_x;
  const int faces_num = edges_x * edges_y;
  const int corners_num = faces_num * 4;

  Mesh *mesh = BKE_mesh_new_nomain(verts_num, edges_num, faces_num, corners_num);
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  MutableSpan<int2> edges = mesh->edges_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  MutableSpan<int> corner_edges = mesh->corner_edges_for_write();
  bke::mesh_smooth_set(*mesh, false);

  bke::SpanAttributeWriter<float2> uv_attribute;
  if (uv_map_id && faces_num != 0) {
    uv_attribute = mesh->attributes_for_write().lookup_or_add_for_write_only_span<float2>(
        *uv_map_id, bke::AttrDomain::Corner);
  }

  const bool use_threading = verts_num > 4096;
  const int64_t bytes_touched = positions.size_in_bytes() + edges.size_in_bytes() +
                                corner_verts.size_in_bytes() + corner_edges.size_in_bytes() +
                                (uv_attribute ? uv_attribute.span.size_in_bytes() : 0);

  threading::memory_bandwidth_bound_task(bytes_touched, [&]() {
    threading::parallel_invoke(
        use_threading,
        [&]() { offset_indices::fill_constant_group_size(4, 0, mesh->face_offsets_for_write()); },
        [&]() { calculate_positions(verts_x, verts_y, size_x, size_y, positions); },
        [&]() { calculate_y_edges(verts_x, verts_y, edges); },
        [&]() { calculate_x_edges(verts_x, verts_y, edges); },
        [&]() { calculate_corners(verts_x, verts_y, corner_verts, corner_edges); },
        [&]() {
          if (uv_attribute) {
            calculate_uvs(verts_x, verts_y, size_x, size_y, uv_attribute.span);
          }
        });
  });

  if (uv_attribute) {
    uv_attribute.finish();
  }

  if (verts_x > 1 || verts_y > 1) {
    mesh->tag_loose_verts_none();
  }
  if (verts_x > 1 && verts_y > 1) {
    mesh->tag_loose_edges_none();
  }
  mesh->tag_overlapping_none();

  const float3 bounds = float3(size_x * 0.5f, size_y * 0.5f, 0.0f);
  mesh->bounds_set_eager({-bounds, bounds});

  return mesh;
}

}  // namespace blender::geometry
