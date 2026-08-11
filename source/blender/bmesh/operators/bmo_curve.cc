/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 */
#include "BLI_math_vector.hh"
#include "BLI_math_vector_c.hh"

#include "BLI_length_parameterize.hh"
#include "BLI_map.hh"
#include "BLI_math_solvers.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */

namespace blender {

constexpr float CURVE_EPSILON = 1e-8f;

/**
 * A chain of vertices collected from a walk along connected edges.
 */
struct CurveChainData {
  /** Ordered vertices along the chain path. */
  Vector<BMVert *> verts;
  /** True if the path forms a closed chain. */
  bool is_closed = false;
};

/**
 * Coefficients for the cubic spline curve equation.
 */
struct SplineCoeffs {
  /** Value at the start of the segment. */
  float a;
  /** First-order coefficient. */
  float b;
  /** Second-order coefficient. */
  float c;
  /** Third-order coefficient. */
  float d;
  /** Parameter value at the start of the segment. */
  float x;
};

/**
 * Compute cubic spline coefficients for one coordinate axis.
 * Uses `BLI_tridiagonal_solve` for open chains and
 * `BLI_tridiagonal_solve_cyclic` for closed loops.
 */
static void calculate_splines_axis(Span<float> distances,
                                   Span<float> coords,
                                   const bool is_closed,
                                   Array<SplineCoeffs> &r_coeffs)
{
  const int verts_num = coords.size();
  if (verts_num < 2) {
    return;
  }
  const int segments_num = is_closed ? verts_num : verts_num - 1;
  Array<float> segment_length(segments_num);

  for (const int i : IndexRange(segments_num)) {
    segment_length[i] = distances[i + 1] - distances[i];
    if (!(segment_length[i] > 0.0f)) {
      segment_length[i] = CURVE_EPSILON;
    }
  }

  /* Stores second derivative coefficients. For a natural cubic spline, the boundary
   * condition defines the first and last points as zero. */
  Array<float> c_vals(verts_num, 0.0f);

  if (is_closed) {
    Array<float> lower_diag(verts_num);
    Array<float> diag(verts_num);
    Array<float> upper_diag(verts_num);
    Array<float> rhs(verts_num);
    for (const int i : IndexRange(verts_num)) {
      const int i_prev = math::mod_periodic(i - 1, verts_num);
      const int i_next = math::mod_periodic(i + 1, verts_num);
      lower_diag[i] = segment_length[i_prev];
      diag[i] = 2.0f * (segment_length[i_prev] + segment_length[i]);
      upper_diag[i] = segment_length[i];
      rhs[i] = 3.0f * (((coords[i_next] - coords[i]) / segment_length[i]) -
                       ((coords[i] - coords[i_prev]) / segment_length[i_prev]));
    }
    BLI_tridiagonal_solve_cyclic(
        lower_diag.data(), diag.data(), upper_diag.data(), rhs.data(), c_vals.data(), verts_num);
  }
  else {
    const int interior = verts_num - 2;
    if (interior > 0) {
      Array<float> lower_diag(interior);
      Array<float> diag(interior);
      Array<float> upper_diag(interior);
      Array<float> rhs(interior);

      for (const int i_curr : IndexRange(interior)) {
        const int i_next = i_curr + 1;
        lower_diag[i_curr] = segment_length[i_curr];
        diag[i_curr] = 2.0f * (segment_length[i_curr] + segment_length[i_next]);
        upper_diag[i_curr] = segment_length[i_next];
        rhs[i_curr] = 3.0f * (((coords[i_next + 1] - coords[i_next]) / segment_length[i_next]) -
                              ((coords[i_next] - coords[i_curr]) / segment_length[i_curr]));
      }
      BLI_tridiagonal_solve(lower_diag.data(),
                            diag.data(),
                            upper_diag.data(),
                            rhs.data(),
                            c_vals.data() + 1,
                            interior);
    }
  }

  r_coeffs = Array<SplineCoeffs>(segments_num);
  /* Build polynomial coefficients for each segment. */
  for (const int i : IndexRange(segments_num)) {
    const int i_next = is_closed ? math::mod_periodic(i + 1, verts_num) : i + 1;
    const float coeff_a = coords[i];
    const float coeff_b = ((coords[i_next] - coords[i]) / segment_length[i]) -
                          (segment_length[i] * (c_vals[i_next] + 2.0f * c_vals[i])) / 3.0f;
    const float coeff_c = c_vals[i];
    const float coeff_d = (c_vals[i_next] - c_vals[i]) / (3.0f * segment_length[i]);
    r_coeffs[i] = {coeff_a, coeff_b, coeff_c, coeff_d, distances[i]};
  }
}

static void trace_curve_loop(BMVert *start_vert,
                             BMEdge *start_edge,
                             Set<BMEdge *> &visited_edges,
                             Vector<BMVert *> &r_loop,
                             bool &r_circular)
{
  BMVert *curr_vert = start_vert;
  BMEdge *curr_edge = start_edge;

  visited_edges.add(curr_edge);

  while (true) {
    BMVert *next_vert = BM_edge_other_vert(curr_edge, curr_vert);
    r_loop.append(next_vert);

    if (next_vert == start_vert) {
      r_circular = true;
      r_loop.pop_last();
      break;
    }

    int edge_count = 0;
    BMIter iter;
    BMEdge *e;
    BMEdge *next_edge = nullptr;

    BM_ITER_ELEM (e, &iter, next_vert, BM_EDGES_OF_VERT) {
      if (!BM_elem_flag_test(e, BM_ELEM_HIDDEN)) {
        edge_count++;
      }
    }

    if (edge_count < 3 || edge_count > 4) {
      break;
    }
    BM_ITER_ELEM (e, &iter, next_vert, BM_EDGES_OF_VERT) {
      if (e == curr_edge || BM_elem_flag_test(e, BM_ELEM_HIDDEN)) {
        continue;
      }

      if (!BM_edge_share_quad_check(curr_edge, e)) {
        next_edge = e;
        break;
      }
    }

    if (!next_edge) {
      break;
    }
    curr_edge = next_edge;
    curr_vert = next_vert;
    visited_edges.add(curr_edge);
  }
}

static void get_curve_input_chains(BMesh *bm, Vector<CurveChainData> &r_chains)
{
  Vector<BMVert *> selected_verts;
  BMIter iter;
  BMVert *v;
  BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
    if (BM_elem_flag_test(v, BM_ELEM_SELECT) && !BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
      selected_verts.append(v);
    }
  }

  Set<BMEdge *> visited_edges;

  for (BMVert *start_vert : selected_verts) {
    BMIter v_edge_iter;
    BMEdge *start_edge;
    BM_ITER_ELEM (start_edge, &v_edge_iter, start_vert, BM_EDGES_OF_VERT) {
      if (BM_elem_flag_test(start_edge, BM_ELEM_HIDDEN)) {
        continue;
      }
      if (visited_edges.contains(start_edge)) {
        continue;
      }

      CurveChainData chain;
      chain.is_closed = false;

      Vector<BMVert *> forward_trace;
      bool is_forward_circular = false;
      trace_curve_loop(
          start_edge->v1, start_edge, visited_edges, forward_trace, is_forward_circular);

      if (is_forward_circular) {
        chain.is_closed = true;
        chain.verts.append(start_edge->v1);
        chain.verts.extend(forward_trace);
      }
      else {
        Vector<BMVert *> backward_trace;
        bool is_backward_circular = false;
        trace_curve_loop(
            start_edge->v2, start_edge, visited_edges, backward_trace, is_backward_circular);

        std::ranges::reverse(backward_trace);
        chain.verts.extend(backward_trace);
        chain.verts.extend(forward_trace);
      }

      if (chain.verts.size() < 3) {
        continue;
      }

      int num_selected = 0;
      for (BMVert *chain_vert : chain.verts) {
        if (BM_elem_flag_test(chain_vert, BM_ELEM_SELECT)) {
          num_selected++;
          if (num_selected >= 2) {
            r_chains.append(std::move(chain));
            break;
          }
        }
      }
    }
  }
}

static void find_longest_unselected_gap(Span<BMVert *> verts,
                                        int &r_longest_gap,
                                        int &r_best_gap_start)
{
  const int verts_num = verts.size();
  Vector<int> selected_indices;

  for (const int i : verts.index_range()) {
    if (BM_elem_flag_test(verts[i], BM_ELEM_SELECT)) {
      selected_indices.append(i);
    }
  }

  if (selected_indices.is_empty()) {
    r_longest_gap = verts_num;
    r_best_gap_start = 0;
    return;
  }

  int longest_gap = -1;
  int best_gap_start = -1;

  for (const int i : selected_indices.index_range()) {
    int curr_index = selected_indices[i];
    int next_index = selected_indices[math::mod_periodic(i + 1, int(selected_indices.size()))];
    int gap_size = math::mod_periodic(next_index - curr_index - 1, verts_num);

    if (gap_size > longest_gap) {
      longest_gap = gap_size;
      best_gap_start = math::mod_periodic(curr_index + 1, verts_num);
    }
  }

  r_longest_gap = longest_gap;
  r_best_gap_start = best_gap_start;
}

static void trim_chains_to_boundaries(Vector<CurveChainData> &chains)
{
  for (CurveChainData &chain : chains) {
    if (chain.is_closed) {
      int longest_gap, best_gap_start;
      find_longest_unselected_gap(chain.verts, longest_gap, best_gap_start);

      if (longest_gap > 0) {
        int shift = math::mod_periodic(best_gap_start + longest_gap, int(chain.verts.size()));
        std::rotate(chain.verts.begin(), chain.verts.begin() + shift, chain.verts.end());
      }
      chain.is_closed = false;
    }

    int first_selected = -1;
    int last_selected = -1;
    for (const int i : chain.verts.index_range()) {
      if (BM_elem_flag_test(chain.verts[i], BM_ELEM_SELECT)) {
        if (first_selected == -1) {
          first_selected = i;
        }
        last_selected = i;
      }
    }

    if (first_selected == -1) {
      chain.verts.clear();
      continue;
    }

    if (first_selected > 0 || last_selected < chain.verts.size() - 1) {
      Vector<BMVert *> trimmed;
      for (int i = first_selected; i <= last_selected; i++) {
        trimmed.append(chain.verts[i]);
      }
      chain.verts = std::move(trimmed);
    }
  }
}

static float3 project_point_onto_line(const float3 &line_a, const float3 &line_b, const float3 &p)
{
  const float3 line_dir = line_b - line_a;
  return line_a + math::project(p - line_a, line_dir);
}

static void project_knots(Span<BMVert *> verts,
                          Span<int> knot_indices,
                          const bool is_closed,
                          MutableSpan<float3> r_knot_positions)
{
  const int knots_num = knot_indices.size();

  for (const int ki : IndexRange(knots_num)) {
    const int vert_index = knot_indices[ki];
    const float3 knot_co(verts[vert_index]->co);

    if (!is_closed && (ki == 0 || ki == knots_num - 1)) {
      r_knot_positions[ki] = knot_co;
      continue;
    }

    if (!BM_elem_flag_test(verts[vert_index], BM_ELEM_SELECT)) {
      r_knot_positions[ki] = knot_co;
      continue;
    }

    float3 left_co = knot_co;
    bool found_left = false;
    {
      const int verts_num = verts.size();
      for (const int step : IndexRange(1, verts_num - 1)) {
        int index = vert_index - step;
        if (is_closed) {
          index = math::mod_periodic(index, verts_num);
        }
        else if (index < 0) {
          break;
        }
        if (!knot_indices.contains(index)) {
          left_co = float3(verts[index]->co);
          found_left = true;
          break;
        }
      }
    }

    float3 right_co = knot_co;
    bool found_right = false;
    {
      const int verts_num = verts.size();
      for (const int step : IndexRange(1, verts_num - 1)) {
        int index = vert_index + step;
        if (is_closed) {
          index = math::mod_periodic(index, verts_num);
        }
        else if (index >= verts_num) {
          break;
        }
        if (!knot_indices.contains(index)) {
          right_co = float3(verts[index]->co);
          found_right = true;
          break;
        }
      }
    }

    if (found_left && found_right && math::distance_squared(left_co, right_co) > CURVE_EPSILON) {
      r_knot_positions[ki] = project_point_onto_line(left_co, right_co, knot_co);
    }
    else {
      r_knot_positions[ki] = knot_co;
    }
  }
}

static void calculate_curve_distances(Span<BMVert *> verts,
                                      Span<int> knot_indices,
                                      Span<float3> knot_positions,
                                      const bool is_closed,
                                      const bool regular,
                                      Array<float> &r_knot_distances,
                                      Array<float> &r_point_distances,
                                      Array<int> &r_point_vert_indices)
{
  const int verts_num = verts.size();
  Set<int> knot_set;
  for (const int ki : knot_indices) {
    knot_set.add(ki);
  }

  Array<float3> positions(verts_num);
  for (const int i : IndexRange(verts_num)) {
    if (knot_set.contains(i)) {
      for (const int ki : knot_indices.index_range()) {
        if (knot_indices[ki] == i) {
          positions[i] = knot_positions[ki];
          break;
        }
      }
    }
    else {
      positions[i] = float3(verts[i]->co);
    }
  }

  const int dist_size = verts_num + (is_closed ? 1 : 0);
  Array<float> cumulative(dist_size);
  cumulative[0] = 0.0f;
  length_parameterize::accumulate_lengths<float3>(
      positions, is_closed, cumulative.as_mutable_span().drop_front(1));

  r_knot_distances = Array<float>(knot_indices.size() + (is_closed ? 1 : 0));
  r_point_distances = Array<float>(verts_num - knot_indices.size());
  r_point_vert_indices = Array<int>(verts_num - knot_indices.size());

  int knot_index = 0;
  int point_index = 0;
  for (const int i : IndexRange(verts_num)) {
    if (knot_set.contains(i)) {
      r_knot_distances[knot_index++] = cumulative[i];
    }
    else {
      r_point_distances[point_index] = cumulative[i];
      r_point_vert_indices[point_index] = i;
      point_index++;
    }
  }

  if (is_closed && !r_knot_distances.is_empty()) {
    const float total_length = cumulative.last();
    r_knot_distances[knot_index++] = total_length;
  }

  if (regular && r_knot_distances.size() >= 2) {
    const float total = cumulative.last();
    if (total > CURVE_EPSILON) {
      const float spacing = total / float(is_closed ? verts_num : verts_num - 1);
      for (const int i : r_point_vert_indices.index_range()) {
        const int vert_index = r_point_vert_indices[i];
        r_point_distances[i] = float(vert_index) * spacing;
      }
      for (const int ki : knot_indices.index_range()) {
        r_knot_distances[ki] = float(knot_indices[ki]) * spacing;
      }
      if (is_closed) {
        r_knot_distances[knot_indices.size()] = total;
      }
    }
  }
}

static void calculate_curve_splines(Span<float3> knot_positions,
                                    Span<float> knot_distances,
                                    const bool is_closed,
                                    std::array<Array<SplineCoeffs>, 3> &r_coeffs)
{
  const int knots_num = knot_positions.size();
  Array<float> coords_x(knots_num);
  Array<float> coords_y(knots_num);
  Array<float> coords_z(knots_num);

  for (const int i : IndexRange(knots_num)) {
    coords_x[i] = knot_positions[i].x;
    coords_y[i] = knot_positions[i].y;
    coords_z[i] = knot_positions[i].z;
  }

  calculate_splines_axis(knot_distances, coords_x, is_closed, r_coeffs[0]);
  calculate_splines_axis(knot_distances, coords_y, is_closed, r_coeffs[1]);
  calculate_splines_axis(knot_distances, coords_z, is_closed, r_coeffs[2]);
}

static void execute_curve_chain(Span<BMVert *> in_verts,
                                const bool is_closed,
                                const int interpolation,
                                const int restriction,
                                const bool regular,
                                const float factor,
                                const bool lock_x,
                                const bool lock_y,
                                const bool lock_z,
                                const bool do_project_knots,
                                Map<BMVert *, float3> &r_moves)
{
  int verts_num = in_verts.size();
  const int original_verts_num = verts_num;
  bool local_is_closed = is_closed;
  int shift = 0;

  if (local_is_closed) {
    int longest_gap, best_gap_start;
    find_longest_unselected_gap(in_verts, longest_gap, best_gap_start);

    int offset = original_verts_num / 4;

    if (longest_gap > 2 * offset) {
      local_is_closed = false;

      int trim_start = math::mod_periodic(best_gap_start + longest_gap - offset,
                                          original_verts_num);
      int trim_end = math::mod_periodic(best_gap_start - 1 + offset, original_verts_num);

      shift = trim_start;
      verts_num = math::mod_periodic(trim_end - trim_start, original_verts_num) + 1;
    }
    else {
      for (const int i : in_verts.index_range()) {
        if (BM_elem_flag_test(in_verts[i], BM_ELEM_SELECT)) {
          shift = i;
          break;
        }
      }
    }
  }

  Array<BMVert *> verts(verts_num);
  for (const int i : IndexRange(verts_num)) {
    verts[i] = in_verts[(i + shift) % original_verts_num];
  }

  Vector<int> knot_indices;
  for (const int i : IndexRange(verts_num)) {
    if (BM_elem_flag_test(verts[i], BM_ELEM_SELECT)) {
      knot_indices.append(i);
    }
  }

  if (!local_is_closed) {
    if (knot_indices.is_empty() || knot_indices.first() != 0) {
      knot_indices.insert(0, 0);
    }
    if (knot_indices.last() != verts_num - 1) {
      knot_indices.append(verts_num - 1);
    }
  }

  if (knot_indices.size() < 2) {
    return;
  }

  Array<float3> knot_positions(knot_indices.size());
  if (do_project_knots) {
    project_knots(verts, knot_indices, local_is_closed, knot_positions);
  }
  else {
    for (const int ki : knot_indices.index_range()) {
      knot_positions[ki] = float3(verts[knot_indices[ki]]->co);
    }
  }

  Array<float3> original_knot_pos(knot_indices.size());
  for (const int ki : knot_indices.index_range()) {
    original_knot_pos[ki] = float3(verts[knot_indices[ki]]->co);
  }

  Array<float> knot_distances;
  Array<float> point_distances;
  Array<int> point_vert_indices;
  calculate_curve_distances(verts,
                            knot_indices,
                            original_knot_pos,
                            local_is_closed,
                            regular,
                            knot_distances,
                            point_distances,
                            point_vert_indices);

  if (point_vert_indices.is_empty()) {
    return;
  }

  const int points_num = point_vert_indices.size();
  Array<float3> new_positions(points_num);
  Array<int> segment_indices(points_num);
  Array<float> factors(points_num);

  length_parameterize::sample_at_lengths(
      knot_distances.as_span().drop_front(1), point_distances, segment_indices, factors);

  if (interpolation == CURVE_INTERP_CUBIC) {
    const int spline_knots_num = local_is_closed ? int(knot_indices.size()) + 1 :
                                                   int(knot_indices.size());
    Array<float3> spline_knot_pos(spline_knots_num);
    for (const int ki : knot_indices.index_range()) {
      spline_knot_pos[ki] = original_knot_pos[ki];
    }
    if (local_is_closed) {
      spline_knot_pos[knot_indices.size()] = original_knot_pos[0];
    }

    std::array<Array<SplineCoeffs>, 3> axis_coeffs;
    calculate_curve_splines(spline_knot_pos, knot_distances, false, axis_coeffs);

    for (const int i : IndexRange(points_num)) {
      const int seg = segment_indices[i];
      const float dt = point_distances[i] - axis_coeffs[0][seg].x;

      const SplineCoeffs &cx = axis_coeffs[0][seg];
      const SplineCoeffs &cy = axis_coeffs[1][seg];
      const SplineCoeffs &cz = axis_coeffs[2][seg];

      new_positions[i] = float3(cx.a + dt * (cx.b + dt * (cx.c + dt * cx.d)),
                                cy.a + dt * (cy.b + dt * (cy.c + dt * cy.d)),
                                cz.a + dt * (cz.b + dt * (cz.c + dt * cz.d)));
    }
  }
  else if (interpolation == CURVE_INTERP_LINEAR) {
    length_parameterize::interpolate<float3>(
        original_knot_pos, segment_indices, factors, new_positions);
  }

  for (const int i : point_vert_indices.index_range()) {
    const int vert_index = point_vert_indices[i];
    BMVert *vert = verts[vert_index];
    float3 new_pos = new_positions[i];

    const float3 old_pos(vert->co);
    const float3 delta = new_pos - old_pos;

    if (restriction != CURVE_RESTRICT_NONE) {
      if (math::length_squared(delta) > CURVE_EPSILON) {
        const float3 normal(vert->no);
        const float dot_val = math::dot(delta, normal);

        if (restriction == CURVE_RESTRICT_EXTRUDE) {
          if (dot_val < -CURVE_EPSILON) {
            continue;
          }
        }
        else if (restriction == CURVE_RESTRICT_INDENT) {
          if (dot_val > CURVE_EPSILON) {
            continue;
          }
        }
      }
    }

    if (lock_x) {
      new_pos.x = old_pos.x;
    }
    if (lock_y) {
      new_pos.y = old_pos.y;
    }
    if (lock_z) {
      new_pos.z = old_pos.z;
    }

    const float3 final_pos = math::interpolate(old_pos, new_pos, factor);
    r_moves.add_overwrite(vert, final_pos);
  }
}

void bmo_curve_exec(BMesh *bm, BMOperator *op)
{
  const float factor = BMO_slot_float_get(op->slots_in, "factor");
  const int restriction = BMO_slot_int_get(op->slots_in, "restriction");
  const bool boundaries = BMO_slot_bool_get(op->slots_in, "boundaries");
  const bool regular = BMO_slot_bool_get(op->slots_in, "regular");
  const bool lock_x = BMO_slot_bool_get(op->slots_in, "lock_x");
  const bool lock_y = BMO_slot_bool_get(op->slots_in, "lock_y");
  const bool lock_z = BMO_slot_bool_get(op->slots_in, "lock_z");
  const int interpolation = BMO_slot_int_get(op->slots_in, "interpolation");

  BMO_slot_buffer_hflag_enable(bm, op->slots_in, "geom", BM_VERT, BM_ELEM_SELECT, false);

  Vector<CurveChainData> chains;
  get_curve_input_chains(bm, chains);

  if (boundaries) {
    trim_chains_to_boundaries(chains);
  }

  Map<BMVert *, float3> moves;

  for (const CurveChainData &chain : chains) {
    execute_curve_chain(chain.verts,
                        chain.is_closed,
                        interpolation,
                        restriction,
                        regular,
                        factor,
                        lock_x,
                        lock_y,
                        lock_z,
                        true,
                        moves);
  }

  for (auto item : moves.items()) {
    copy_v3_v3(item.key->co, item.value);
  }
}

}  // namespace blender
