/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_coloring.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_index_mask.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_offset_indices.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_task_size_hints.hh"

namespace blender {

Vector<IndexMask> color_verts_by_groups(Span<int> vert_id, LinearAllocator<> &memory)
{
  // Array<int> vert_colors(vert_id.size());
  // Vector<IndexMask> colors(color_verts_by_groups(vert_id, vert_colors.as_mutable_span()));
  // IndexMask::from_groups<int>(
  //     edges.index_range(), memory, [&](const int i) { return vert_colors[i]; }, colors);
  // return colors;
  return {};
}

Vector<IndexMask> color_verts_by_groups(GroupedSpan<int> vert_to_ids, LinearAllocator<> &memory)
{
  // Array<int> vert_colors(vert_to_ids.size());
  // Vector<IndexMask> colors(color_verts_by_groups(vert_to_ids, vert_colors.as_mutable_span()));
  // IndexMask::from_groups<int>(
  //     edges.index_range(), memory, [&](const int i) { return vert_colors[i]; }, colors);
  // return colors;
  return {};
}

Vector<IndexMask> color_verts_by_groups(GroupedSpan<int> vert_to_ids,
                                        int max_id_value,
                                        LinearAllocator<> &memory)
{
  // Array<int> vert_colors(vert_to_ids.size());
  // Vector<IndexMask> colors(color_verts_by_groups(vert_to_ids, max_id_value,
  // vert_colors.as_mutable_span())); IndexMask::from_groups<int>(
  //     edges.index_range(), memory, [&](const int i) { return vert_colors[i]; }, colors);
  // return colors;
  return {};
}

int color_verts_by_groups(Span<int> vert_id, MutableSpan<int> r_color)
{
  return {};
}

int color_verts_by_groups(GroupedSpan<int> vert_to_ids, MutableSpan<int> r_color)
{
  return {};
}

int color_verts_by_groups(GroupedSpan<int> vert_to_ids, int max_id_value, MutableSpan<int> r_color)
{
  return {};
}

static auto div_round_up(const std::integral auto value, const std::integral auto divisor)
{
  return (value + divisor - 1) / divisor;
}

static void foreach_isolated_edges_set_imp(const int total_verts,
                                           const Span<int2> edges,
                                           const int grain_size,
                                           const auto &func)
{
  const int chunk_size = 5000;
  const int chunks_num = div_round_up(total_verts, chunk_size);

  /* Row major chunks table. */

  const auto index_to_chunk = [&](const int chunk_i) {
    return int2(chunk_i % chunks_num, chunk_i / chunks_num);
  };
  const auto chunk_to_index = [&](const int2 chunk) { return chunk.y * chunks_num + chunk.x; };

  LinearAllocator<> memory;
  Array<IndexMask> chunk_partition(chunks_num * chunks_num);
  IndexMask::from_groups<int>(
      edges.index_range(),
      memory,
      [&](const int edge_i) {
        const OrderedEdge ordered(edges[edge_i]);
        return chunk_to_index(int2(ordered.v_low, ordered.v_high) / chunk_size);
      },
      chunk_partition);

  /* Diagonal is free to process. */
  threading::parallel_for(
      IndexRange(chunks_num),
      grain_size,
      [&](const IndexRange range) {
        for (const int i : range) {
          func(chunk_partition[chunk_to_index(int2(i))]);
        }
      },
      threading::individual_task_sizes(
          [&](const int i) { return chunk_partition[chunk_to_index(int2(i))].size(); }));

  Array<bool> chunk_processed(chunks_num * chunks_num, false);
  for (const int i : IndexRange(chunks_num)) {
    chunk_processed[chunk_to_index(int2(i))] = true;
  }

  for (const int i : chunk_processed.index_range()) {
    chunk_processed[i] |= chunk_partition[i].is_empty();
  }

  const auto fill_row = [&](MutableSpan<bool> values, const bool value, const int index) {
    values.slice(chunks_num * index, chunks_num).fill(value);
  };

  const auto fill_col = [&](MutableSpan<bool> values, const bool value, const int index) {
    for (const int i : IndexRange(chunks_num)) {
      values[chunk_to_index(int2(index, i))] = value;
    }
  };

  for ([[maybe_unused]] const int pass_i : IndexRange(chunks_num * chunks_num)) {
    Vector<int2> chunks_for_pass;
    Array<bool> to_process = chunk_processed;

    const int pass_first_chunk_index = [&]() {
      int pass_first_chunk_index = 0;
      while (pass_first_chunk_index + 1 < to_process.size() && to_process[pass_first_chunk_index])
      {
        pass_first_chunk_index++;
      }
      return pass_first_chunk_index;
    }();

    for ([[maybe_unused]] const int chunk_i : IndexRange(chunks_num - 1)) {
      const int i_to_process =
          to_process.as_span().drop_front(pass_first_chunk_index).first_index_try(false);
      if (i_to_process == -1) {
        break;
      }
      const int index_to_process = pass_first_chunk_index + i_to_process;

      BLI_assert(!to_process[index_to_process]);
      BLI_assert(!to_process.as_span().take_front(index_to_process).contains(false));

      const int2 chunk = index_to_chunk(index_to_process);
      chunks_for_pass.append(chunk);

      fill_row(to_process, true, chunk.x);
      fill_row(to_process, true, chunk.y);

      fill_col(to_process, true, chunk.x);
      fill_col(to_process, true, chunk.y);
    }

    if (chunks_for_pass.is_empty()) {
      break;
    }

    BLI_assert([&]() {
      VectorSet<int> axes;
      for (const int2 chunk : chunks_for_pass) {
        if (!axes.add(chunk.x)) {
          return false;
        }
        if (!axes.add(chunk.y)) {
          return false;
        }
      }
      return true;
    }());

    BLI_assert(([&]() {
      Array<Set<int>> verts_for_chunks(chunks_for_pass.size());
      for (const int i : chunks_for_pass.index_range()) {
        const int2 chunk_a = chunks_for_pass[i];
        chunk_partition[chunk_to_index(chunk_a)].foreach_index([&](const int edge_i) {
          verts_for_chunks[i].add(edges[edge_i].x);
          verts_for_chunks[i].add(edges[edge_i].y);
        });

        const int2 chunk_b = {chunk_a.y, chunk_a.x};
        chunk_partition[chunk_to_index(chunk_b)].foreach_index([&](const int edge_i) {
          verts_for_chunks[i].add(edges[edge_i].x);
          verts_for_chunks[i].add(edges[edge_i].y);
        });
      }

      Set<int> all_verts;
      for (const Set<int> &verts : verts_for_chunks) {
        for (const int vert : verts) {
          if (!all_verts.add(vert)) {
            return false;
          }
        }
      }

      return true;
    }()));

    threading::parallel_for(
        chunks_for_pass.index_range(),
        grain_size,
        [&](const IndexRange range) {
          for (const int i : range) {
            const int2 chunk_a = chunks_for_pass[i];
            const int2 chunk_b = chunk_a.yx();
            func(chunk_partition[chunk_to_index(chunk_a)]);
            func(chunk_partition[chunk_to_index(chunk_b)]);
          }
        },
        threading::individual_task_sizes([&](const int i) {
          const int2 chunk_a = chunks_for_pass[i];
          const int2 chunk_b = chunk_a.yx();
          return chunk_partition[chunk_to_index(chunk_a)].size() +
                 chunk_partition[chunk_to_index(chunk_b)].size();
        }));

    for (const int2 chunk_a : chunks_for_pass) {
      const int2 chunk_b = chunk_a.yx();
      BLI_assert(!chunk_processed[chunk_to_index(chunk_a)]);
      BLI_assert(!chunk_processed[chunk_to_index(chunk_b)] ||
                 chunk_partition[chunk_to_index(chunk_b)].is_empty());

      chunk_processed[chunk_to_index(chunk_a)] = true;
      chunk_processed[chunk_to_index(chunk_b)] = true;
    }
  }

  BLI_assert(!chunk_processed.as_span().contains(false));
}

static void foreach_isolated_edges_set(const int total_verts,
                                       const Span<int2> edges,
                                       const int grain_size,
                                       const auto &func)
{
  [[maybe_unused]] std::atomic<int> edge_count{0};
  foreach_isolated_edges_set_imp(total_verts, edges, grain_size, [&](const IndexMask &mask) {
#ifndef NDEBUG
    edge_count += mask.size();
#endif
    func(mask);
  });
  BLI_assert(edge_count == edges.size());
}

static GroupedSpan<int> build_vert_to_edge_map(const Span<int2> edges,
                                               const int verts_num,
                                               Array<int> &r_offsets,
                                               Array<int> &r_indices)
{
  const Span<int> edge_verts = edges.cast<int>();

  r_offsets.reinitialize(verts_num + 1);
  r_offsets.as_mutable_span().fill(0);
  offset_indices::build_reverse_offsets(edge_verts, r_offsets);

  const OffsetIndices<int> offsets(r_offsets);
  r_indices.reinitialize(offsets.total_size());

  offset_indices::reverse_indices_in_groups(edge_verts, offsets, r_indices, 2);
  return {offsets, r_indices};
}

Vector<IndexMask> color_edges_by_verts(const int total_verts,
                                       const Span<int2> edges,
                                       LinearAllocator<> &memory)
{
  Array<int> offsets;
  Array<int> indices;
  const GroupedSpan<int> vert_to_edges = build_vert_to_edge_map(
      edges, total_verts, offsets, indices);
  return color_edges_by_verts(total_verts, edges, vert_to_edges, memory);
}

int color_edges_by_verts(const int total_verts, const Span<int2> edges, MutableSpan<int> r_color)
{
  Array<int> offsets;
  Array<int> indices;
  const GroupedSpan<int> vert_to_edges = build_vert_to_edge_map(
      edges, total_verts, offsets, indices);
  return color_edges_by_verts(total_verts, edges, vert_to_edges, r_color);
}

Vector<IndexMask> color_edges_by_verts(const int total_verts,
                                       const Span<int2> edges,
                                       const GroupedSpan<int> vert_to_edges,
                                       LinearAllocator<> &memory)
{
  Array<int> edge_colors(edges.size());
  Vector<IndexMask> colors(
      color_edges_by_verts(total_verts, edges, vert_to_edges, edge_colors.as_mutable_span()));
  IndexMask::from_groups<int>(
      edges.index_range(), memory, [&](const int i) { return edge_colors[i]; }, colors);
  return colors;
}

int color_edges_by_verts(const int total_verts,
                         const Span<int2> edges,
                         const GroupedSpan<int> vert_to_edges,
                         MutableSpan<int> r_color)
{
  static constexpr int not_a_color = -1;
  r_color.fill(not_a_color);

  threading::EnumerableThreadSpecific<int> colors_num;

  foreach_isolated_edges_set(total_verts, edges, 1000, [&](const IndexMask &edges_mask) {
    Vector<bool, 16> color_is_used;

    edges_mask.foreach_index([&](const int edge_i) {
      color_is_used.as_mutable_span().fill(false);
      for (const int vert : {edges[edge_i][0], edges[edge_i][1]}) {
        for (const int other_edge_i : vert_to_edges[vert]) {
          if (edge_i == other_edge_i) {
            continue;
          }
          if (r_color[other_edge_i] == not_a_color) {
            continue;
          }

          color_is_used.resize(std::max<int>(color_is_used.size(), r_color[other_edge_i] + 1),
                               false);
          color_is_used[r_color[other_edge_i]] = true;
        }
      }
      const int best_color = color_is_used.as_span().first_index_try(false);

      if (best_color == -1) {
        r_color[edge_i] = color_is_used.size();
        color_is_used.resize(color_is_used.size() + 1);
      }
      else {
        r_color[edge_i] = best_color;
      }
    });

    colors_num.local() = std::max<int>(colors_num.local(), color_is_used.size());
  });

  int colors_num_value = 0;
  for (const int value : colors_num) {
    colors_num_value = std::max(value, colors_num_value);
  }

  return colors_num_value;
}

bool is_valid_edges_by_verts_coloring(const int total_verts,
                                      const Span<int2> edges,
                                      const Span<IndexMask> colors)
{
  for (const IndexMask &color : colors) {
    Array<int> vert_uses(total_verts, 0);
    color.foreach_index([&](const int edge_i) {
      vert_uses[edges[edge_i].x]++;
      vert_uses[edges[edge_i].y]++;
    });

    if (!std::all_of(vert_uses.begin(), vert_uses.end(), [&](const int count) {
          return ELEM(count, 0, 1);
        }))
    {
      return false;
    }
  }
  return true;
}

}  // namespace blender
