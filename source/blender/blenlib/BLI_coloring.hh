/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_index_mask.hh"
#include "BLI_offset_indices.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_span.hh"

namespace blender {

/**
 * All verts of same group id must be know as of different colors.
 * \return Total number of colors.
 * \param max_id_value: Mean it is possible to use plain array of that size instead of hash set. No internal check of size is made.
 */
int color_verts_by_groups(Span<int> vert_id, MutableSpan<int> r_color);
Vector<IndexMask> color_verts_by_groups(Span<int> vert_id, LinearAllocator<> &memory);

int color_verts_by_groups(GroupedSpan<int> vert_to_ids, MutableSpan<int> r_color);
Vector<IndexMask> color_verts_by_groups(GroupedSpan<int> vert_to_ids, LinearAllocator<> &memory);

int color_verts_by_groups(GroupedSpan<int> vert_to_ids, int max_id_value, MutableSpan<int> r_color);
Vector<IndexMask> color_verts_by_groups(GroupedSpan<int> vert_to_ids, int max_id_value, LinearAllocator<> &memory);

/**
 * Edge must link verties of different colors.
 * \return Total number of colors.
 * \param max_id_value: Mean it is possible to use plain array of that size instead of hash set. No internal check of size is made.
 */
int color_edges_by_verts(int total_verts, Span<int2> edges, MutableSpan<int> r_color);
Vector<IndexMask> color_edges_by_verts(int total_verts, Span<int2> edges, LinearAllocator<> &memory);

int color_edges_by_verts(int total_verts, Span<int2> edges, GroupedSpan<int> vert_to_edges, MutableSpan<int> r_color);
Vector<IndexMask> color_edges_by_verts(int total_verts, Span<int2> edges, GroupedSpan<int> vert_to_edges, LinearAllocator<> &memory);

bool is_valid_verts_by_groups_coloring(Span<int> vert_id, Span<IndexMask> colors);
bool is_valid_verts_by_groups_coloring(GroupedSpan<int> vert_to_ids, Span<IndexMask> colors);
bool is_valid_edges_by_verts_coloring(int total_verts, Span<int2> edges, Span<IndexMask> colors);

}  // namespace blender
