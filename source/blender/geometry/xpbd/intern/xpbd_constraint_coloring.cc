/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_timeit.hh"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_set.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_task_size_hints.hh"

#include "BKE_mesh_mapping.hh"

#include "GEO_xpbd_constraint_coloring_utils.hh"

namespace blender::xpbd {

template<typename PointID, typename GetConstraintPointIdsFn>
inline int color_constraints(GetConstraintPointIdsFn &&get_constraint_point_ids_fn,
                             MutableSpan<int> r_colors)
{
  const int constraints_num = r_colors.size();
  MultiValueMap<PointID, int> constraints_by_point;
  for (const int constraint_i : IndexRange(constraints_num)) {
    for (const PointID &point_id : get_constraint_point_ids_fn(constraint_i)) {
      constraints_by_point.add(point_id, constraint_i);
    }
  }
  int colors_num = 0;
  for (const int constraint_i : IndexRange(constraints_num)) {
    Vector<int> used_colors;
    for (const PointID &point_id : get_constraint_point_ids_fn(constraint_i)) {
      for (const int other_constraint_i : constraints_by_point.lookup(point_id)) {
        if (other_constraint_i >= constraint_i) {
          continue;
        }
        used_colors.append_non_duplicates(r_colors[other_constraint_i]);
      }
    }
    int best_color = 0;
    while (used_colors.contains(best_color)) {
      best_color++;
    }
    r_colors[constraint_i] = best_color;
    colors_num = std::max(colors_num, best_color + 1);
  }
  return colors_num;
}

template<typename PointID, typename GetConstraintPointIdsFn>
inline ConstraintColoring generic_constraint_coloring(
    GetConstraintPointIdsFn &&get_constraint_points_fn,
    const int constraints_num,
    LinearAllocator<> &memory)
{
  if (constraints_num == 0) {
    return {};
  }
  Array<int> colors(constraints_num);
  const int colors_num = color_constraints<PointID>(get_constraint_points_fn, colors);
  Array<Vector<int>> color_indices(colors_num);
  for (const int constraint_i : IndexRange(constraints_num)) {
    color_indices[colors[constraint_i]].append(constraint_i);
  }
  ConstraintColoring coloring;
  for (const int color_i : IndexRange(colors_num)) {
    const IndexMask mask = IndexMask::from_indices<int>(color_indices[color_i], memory);
    coloring.colors.append(mask);
  }
  return coloring;
}

ConstraintColoring color_constraints__unary(const Span<int> affected_points,
                                            LinearAllocator<> &memory)
{
  SCOPED_TIMER(__func__);
  return generic_constraint_coloring<int>(
      [&](const int constraint_i) { return Span<int>(&affected_points[constraint_i], 1); },
      affected_points.size(),
      memory);
}

template<typename T, typename Values, typename TConvert>
static int64_t max_element_of(const Values elements,
                              const int64_t grain_size,
                              const TConvert &convert)
{
  return threading::parallel_reduce(
             elements.index_range().drop_front(1),
             grain_size,
             std::make_pair(convert(elements.first()), 0),
             [&](const IndexRange range, auto value) {
               for (const int index : range) {
                 auto item_value = convert(elements[index]);
                 if (item_value > value.first) {
                   value.first = std::move(item_value);
                   value.second = index;
                 }
                 else if (item_value == value.first) {
                   value.second = std::min(value.second, index);
                 }
               }
               return value;
             },
             [&](const auto &a, const auto &b) {
               if (a.first < b.first) {
                 return b;
               }
               else if (a.first == b.first) {
                 return std::make_pair(a.first, std::min(a.second, b.second));
               }
               return a;
             })
      .second;
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

  IndexMaskMemory memory;
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
      while (pass_first_chunk_index + 1 < to_process.size() && to_process[pass_first_chunk_index]) {
        pass_first_chunk_index++;
      }
      return pass_first_chunk_index;
    }();
    
    for ([[maybe_unused]] const int chunk_i : IndexRange(chunks_num - 1)) {
      const int i_to_process = to_process.as_span().drop_front(pass_first_chunk_index).first_index_try(false);
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
            const int2 chunk_b = {chunk_a.y, chunk_a.x};
            func(chunk_partition[chunk_to_index(chunk_a)]);
            func(chunk_partition[chunk_to_index(chunk_b)]);
          }
        },
        threading::individual_task_sizes([&](const int i) {
          const int2 chunk_a = chunks_for_pass[i];
          const int2 chunk_b = {chunk_a.y, chunk_a.x};
          return chunk_partition[chunk_to_index(chunk_a)].size() +
                 chunk_partition[chunk_to_index(chunk_b)].size();
        }));

    for (const int2 chunk_a : chunks_for_pass) {
      const int2 chunk_b = {chunk_a.y, chunk_a.x};
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

static bool is_valid_binary_coloring(const int total_verts,
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

ConstraintColoring color_constraints__binary(const Span<int2> edges, IndexMaskMemory &memory)
{
  ConstraintColoring coloring;
  {
    SCOPED_TIMER(__func__);
    const Span<int> verts = edges.cast<int>();
    const int max_vert_index = max_element_of<int>(verts, 2048, [&](const int i) { return i; });
    const int total_verts = verts[max_vert_index] + 1;

    Array<int> offsets;
    Array<int> indices;
    const GroupedSpan<int> vert_to_edges = bke::mesh::build_vert_to_edge_map(
        edges, total_verts, offsets, indices);

    static constexpr int not_a_color = -1;
    Array<int> colors(edges.size(), not_a_color);

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
            if (colors[other_edge_i] == not_a_color) {
              continue;
            }

            color_is_used.resize(std::max<int>(color_is_used.size(), colors[other_edge_i] + 1), false);
            color_is_used[colors[other_edge_i]] = true;
          }
        }
        const int best_color = color_is_used.as_span().first_index_try(false);

        if (best_color == -1) {
          colors[edge_i] = color_is_used.size();
          color_is_used.resize(color_is_used.size() + 1);
        }
        else {
          colors[edge_i] = best_color;
        }
      });

      colors_num.local() = std::max<int>(colors_num.local(), color_is_used.size());
    });

    int colors_num_value = 0;
    for (const int value : colors_num) {
      colors_num_value = std::max(value, colors_num_value);
    }

    coloring.colors.reinitialize(colors_num_value);
    IndexMask::from_groups<int>(
        edges.index_range(), memory, [&](const int i) { return colors[i]; }, coloring.colors);

    coloring.colors.remove_if([](const IndexMask &value) { return value.is_empty(); });
    BLI_assert(is_valid_binary_coloring(total_verts, edges, coloring.colors));
  }

  printf("coloring.colors: %d;\n", int(coloring.colors.size()));

  return coloring;
}

ConstraintColoring color_constraints__n_ary(const GroupedSpan<int> affected_points,
                                            LinearAllocator<> &memory)
{
  SCOPED_TIMER(__func__);
  return generic_constraint_coloring<int>(
      [&](const int constraint_i) { return affected_points[constraint_i]; },
      affected_points.size(),
      memory);
}

ConstraintColoring color_constraints__all_independent(const int constraints_num)
{
  SCOPED_TIMER(__func__);
  return ConstraintColoring{{IndexMask(constraints_num)}};
}

}  // namespace blender::xpbd
