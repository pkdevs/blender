/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Redistributes interior vertices of selected edge loops along linear
 * or Hermite spline interpolation path between loop endpoints.
 */

#include "BLI_array.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_vector.hh"

#include "BKE_curves.hh"
#include "BLI_length_parameterize.hh"

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */

namespace blender {

struct EdgeFlowLoop {
  Vector<BMVert *> verts;
  bool is_cyclic;
};

static void walk_edge_loop(BMEdge *start_edge, Vector<BMEdge *> &r_edges) {
  Vector<BMEdge *> side[2];
  int dir = 0;

  BMIter l_iter;
  BMLoop *l;
  BM_ITER_ELEM (l, &l_iter, start_edge, BM_LOOPS_OF_EDGE) {
    const int start_valence = BM_vert_edge_count(l->v);
    if (start_valence <= 4) {
      while (true) {
        const int valence = BM_vert_edge_count(l->v);
        if (valence != start_valence || valence != 4) {
          break;
        }

        /* Opposite edge across the next quad in this direction. */
        l = l->prev->radial_prev->prev;
        if (l->e == start_edge || side[0].contains(l->e) || side[1].contains(l->e)) {
          break;
        }

        if (!BM_elem_flag_test(l->e, BM_ELEM_TAG)) {
          break;
        }

        side[dir].append(l->e);
      }
    }
    if (++dir == 2) {
      break;
    }
  }

  /* Walker order: reverse(side[1]) ++ start_edge ++ side[0] */
  for (int i = side[1].size() - 1; i >= 0; i--) {
    r_edges.append(side[1][i]);
  }

  r_edges.append(start_edge);

  for (BMEdge *e : side[0]) {
    r_edges.append(e);
  }
}

static void walk_ngon(BMEdge *start_edge, Vector<BMEdge *> &r_edges) {
  BMLoop *start_loop = nullptr;
  int max_valence = 0;

  /* Pick loop on largest n-gon bordering start edge. */
  BMIter l_iter;
  BMLoop *l;
  BM_ITER_ELEM (l, &l_iter, start_edge, BM_LOOPS_OF_EDGE) {
    const int valence = l->f->len;
    if (valence > 4 && valence > max_valence) {
      max_valence = valence;
      start_loop = l;
    }
  }

  BLI_assert(start_loop != nullptr);
  Vector<BMEdge *> forward;
  Vector<BMEdge *> backward;

  /* Forward walk around face while vert is non-junction. */
  l = start_loop->next;
  while (BM_vert_edge_count(l->v) < 4 && l->e != start_edge && !forward.contains(l->e)) {
    if (!BM_elem_flag_test(l->e, BM_ELEM_TAG)) {
      break;
    }

    forward.append(l->e);
    l = l->next;
  }

  /* Backward walk around face while other vert is non-junction. */
  l = start_loop->prev;
  while (BM_vert_edge_count(BM_edge_other_vert(l->e, l->v)) < 4 && l->e != start_edge && !forward.contains(l->e) && !backward.contains(l->e)) {
    if (!BM_elem_flag_test(l->e, BM_ELEM_TAG)) {
      break;
    }

    backward.append(l->e);
    l = l->prev;
  }

  /* Walker order: reverse(backward) ++ start_edge ++ forward */
  for (int i = backward.size() - 1; i >= 0; i--) {
    r_edges.append(backward[i]);
  }

  r_edges.append(start_edge);

  for (BMEdge *e : forward) {
    r_edges.append(e);
  }
}

static void walk_boundary(BMEdge *start_edge, Vector<BMEdge *> &r_edges)
{
  /* collect boundary edges reachable from start_edge through junction verts valence > 2 */
  Vector<BMEdge *> edge_loop;
  edge_loop.append(start_edge);
  int visited = 0;

  while (visited < edge_loop.size()) {
    const int boundary_end = edge_loop.size();
    for (int i = visited; i < boundary_end; i++) {
      BMEdge *candidate = edge_loop[i];
      for (BMVert *v : {candidate->v1, candidate->v2}) {
        if (BM_vert_edge_count(v) <= 2) {
          continue;
        }
        BMIter eiter;
        BMEdge *e;
        BM_ITER_ELEM (e, &eiter, v, BM_EDGES_OF_VERT) {
          if (BM_edge_is_boundary(e) && BM_elem_flag_test(e, BM_ELEM_TAG) &&
              !edge_loop.contains(e))
          {
            edge_loop.append(e);
          }
        }
      }
    }
    visited = boundary_end;
  }

  if (edge_loop.size() == 1) {
    r_edges.append(start_edge);
    return;
  }

  /* order the set into a chain. remaining = edge_loop minus start_edge. */
  Vector<BMEdge *> remaining;
  for (BMEdge *e : edge_loop) {
    if (e != start_edge) {
      remaining.append(e);
    }
  }

  Vector<BMEdge *> forward;
  Vector<BMEdge *> backward;
  Vector<BMEdge *> *chain = &forward;

  /* Walk shared verts out from ends of start_edge. */
  for (BMVert *p : {start_edge->v1, start_edge->v2}) {
    while (true) {
      BMEdge *found = nullptr;
      for (BMEdge *e : remaining) {
        if (e->v1 == p || e->v2 == p) {
          found = e;
        }
      }
      if (found == nullptr) {
        break;
      }
      chain->append(found);
      p = BM_edge_other_vert(found, p);
      remaining.remove_first_occurrence_and_reorder(found);
    }
    chain = &backward;
  }

  /* Walker order: reverse(backward) ++ start_edge ++ forward */
  for (int i = backward.size() - 1; i >= 0; i--) {
    r_edges.append(backward[i]);
  }

  r_edges.append(start_edge);
  
  for (BMEdge *fe : forward) {
    r_edges.append(fe);
  }
}

static void edge_flow_walk_loop(BMEdge *start_edge, Vector<BMEdge *> &r_edges)
{
  bool is_ngon = false;
  BMIter l_iter;
  BMLoop *l;
  BM_ITER_ELEM (l, &l_iter, start_edge, BM_LOOPS_OF_EDGE) {
    if (l->f->len > 4) {
      is_ngon = true;
      break;
    }
  }

  const int val0 = BM_vert_edge_count(start_edge->v1);
  const int val1 = BM_vert_edge_count(start_edge->v2);
  const bool quad_flow = (val0 == 4 && val1 == 4);
  const bool loop_end = (val0 > 4 && val1 == 4) || (val0 == 4 && val1 > 4);

  if (is_ngon && !quad_flow && !loop_end) {
    walk_ngon(start_edge, r_edges);
  }
  else if (BM_edge_is_boundary(start_edge)) {
    walk_boundary(start_edge, r_edges);   
  }
  else {
    walk_edge_loop(start_edge, r_edges);
  }
}

/* Returns the number of walks dropped because their edges were not contiguous. */
static int edge_flow_collect_loops(BMesh *bm, Vector<EdgeFlowLoop> &r_loops)
{
  BMIter eiter;
  BMEdge *e;
  int dropped = 0;

  BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
    if (!BM_elem_flag_test(e, BM_ELEM_TAG)) {
      continue;
    }

    /* Walk one ordered, contiguous run of tagged edges starting at e. */
    Vector<BMEdge *> loop_edges;
    edge_flow_walk_loop(e, loop_edges);

    for (BMEdge *le : loop_edges) {
      BM_elem_flag_disable(le, BM_ELEM_TAG);
    }

    if (loop_edges.is_empty()) {
      continue;
    }

    /* Find p1, vert of edges[0] not shared with edges[1]. */
    BMVert *p1 = nullptr;
    if (loop_edges.size() == 1) {
      p1 = loop_edges[0]->v1;
    }
    else if (loop_edges[0]->v1 != loop_edges[1]->v1 && loop_edges[0]->v1 != loop_edges[1]->v2) {
      p1 = loop_edges[0]->v1;
    }
    else {
      p1 = loop_edges[0]->v2;
    }

    /* Build ordered vert array. */
    EdgeFlowLoop loop;
    loop.verts.append(p1);
    BMVert *last = p1;
    bool valid = true;
    for (BMEdge *le : loop_edges) {
      BMVert *next = BM_edge_other_vert(le, last);
      if (next == nullptr) {
        /* Backstop for non-contiguous edges */
        valid = false;
        break;
      }
      loop.verts.append(next);
      last = next;
    }

    if (!valid) {
      dropped++;
      continue;
    }

    /* Ensure larger coord endpoint is first for stable loop ordering. */
    const float3 first_co = loop.verts.first()->co;
    const float3 last_co = loop.verts.last()->co;
    if (first_co.x + first_co.y + first_co.z < last_co.x + last_co.y + last_co.z) {
      std::reverse(loop.verts.begin(), loop.verts.end());
    }

    loop.is_cyclic = (loop.verts.first() == loop.verts.last());
    r_loops.append(std::move(loop));
  }

  return dropped;
}

static bool edge_flow_calc_spline_target(BMLoop *l, const float tension, const float min_angle, float3 &r_target)
{
  if (l->f->len != 4 || l->radial_prev->f->len != 4) {
    return false;
  }

  const float3 center_co = BM_edge_other_vert(l->e, l->v)->co;

  BMLoop *ring1 = l->next->next;
  BMLoop *ring2 = l->radial_prev->prev->prev;

  BMVert *v2 = ring1->v;
  BMVert *v3 = ring2->radial_next->v;

  float3 p1;
  float3 p2 = v2->co;
  float3 p3 = v3->co;
  float3 p4;

  if (!BM_edge_is_boundary(ring1->e)) {
    BMLoop *l_outer = ring1->radial_next->next;
    BMVert *v1 = BM_edge_other_vert(l_outer->e, v2);

    if (v1 == nullptr) {
      return false;
    }

    p1 = v1->co;

    /* Ignore curvature where the ring bends sharply back toward the moved vert. */
    const float3 start_arm = p1 - p2;
    const float3 start_spoke = center_co - p2;
    if (angle_v3v3(start_arm, start_spoke) < min_angle) {
      p1 = p2 - (p3 - p2) * 0.5f;
    }
  }
  else {
    /* Create a phantom control point and reflect p3 through p2. */
    p1 = p2 - (p3 - p2);
  }

  if (!BM_edge_is_boundary(ring2->e)) {
    BMLoop *l_outer = ring2->radial_prev->prev;
    BMVert *v4 = BM_edge_other_vert(l_outer->e, v3);

    if (v4 == nullptr) {
      return false;
    }

    p4 = v4->co;

    const float3 end_arm = p4 - p3;
    const float3 end_spoke = center_co - p3;
    if (angle_v3v3(end_arm, end_spoke) < min_angle) {
      p4 = p3 - (p2 - p3) * 0.5f;
    }
  }
  else {
    /* Set v3 to the far vert first. Create a phantom control point and reflect p3 through p2. */
    v3 = BM_edge_other_vert(ring2->e, v3);
    p3 = v3->co;
    p4 = p3 - (p2 - p3);
  }

  if (p1 == p2 || p3 == p4) {
    return false;
  }

  /* Normalize arm lengths before spline interpolation to control the shape of the curve. */
  const float d = math::distance(p2, p3) * 0.5f;
  p1 = p2 + d * math::normalize(p1 - p2);
  p4 = p3 + d * math::normalize(p4 - p3);

  r_target = math::hermite_spline_interp(p1, p2, p3, p4, 0.5f, -tension, 0.0f);
  return true;
}

static void edge_flow_blend_range(const EdgeFlowLoop &loop, const Array<float3> &orig_cos, int range, const bool reverse, const bool smooth) {
  const int count = int(loop.verts.size());
  range = std::min(range, count - 1);
  if (range <= 0) {
    return;
  }

  Array<float> dist(range + 1);
  dist[0] = 0.0f;
  float total_dist = 0.0f;
  for (int i = 1; i <= range; i++) {
    const int a = reverse ? count - 1 - i : i;
    const int b = reverse ? count - i : i - 1;
    total_dist += math::distance(float3(loop.verts[a]->co), float3(loop.verts[b]->co));
    dist[i] = total_dist;
  }

  if (total_dist == 0.0f) {
    return;
  }

  for (int i = 0; i <= range; i++) {
    const int a = reverse ? count - 1 - i : i;
    float t = dist[i] / total_dist;
    if (smooth) {
      t = t * t * (3.0f - 2.0f * t);
    }

    interp_v3_v3v3(loop.verts[a]->co, orig_cos[a], loop.verts[a]->co, t);
  }
}

/* Apply blend_start / blend_end to verts of each loop back towards original positions. */
static void edge_flow_blend_ends(const EdgeFlowLoop &loop, const Array<float3> &orig_cos, int blend_start, int blend_end, const bool smooth) {
  if (loop.is_cyclic) {
    return;
  }

  edge_flow_blend_range(loop, orig_cos, blend_start, false, smooth);
  edge_flow_blend_range(loop, orig_cos, blend_end, true, smooth);
}

static void edge_flow_sample_bezier(const float3 &p1, const float3 &p2, const float3 &p3, const float3 &p4, MutableSpan<float3> r_result)
{
  BLI_assert(int(r_result.size()) >= 2);
  bke::curves::bezier::evaluate_segment(p1, p2, p3, p4, r_result.drop_back(1));
  r_result.last() = p4;
}

static void edge_flow_map_onto_spline(Span<BMVert *> loop_verts, Span<float3> spline) 
{
  const int count = int(loop_verts.size());
  if (count <= 2 || spline.size() < 2) {
    return;
  }

  /* Accumuate arc length along spline sample for points. */
  Array<float> lengths(length_parameterize::segments_num(spline.size(), false));
  length_parameterize::accumulate_lengths<float3>(spline, false, lengths);
  if (lengths.last() == 0.0f) {
    return;
  }

  Array<int> indices(count);
  Array<float> factors(count);
  length_parameterize::sample_uniform(lengths, true, indices, factors);

  Array<float3> sampled(count);
  length_parameterize::interpolate<float3>(spline, indices, factors, sampled);

  for (const int k : IndexRange(count).drop_front(1).drop_back(1)) {
    copy_v3_v3(loop_verts[k]->co, sampled[k]);
  }
}

void bmo_edge_flow_exec(BMesh *bm, BMOperator *op)
{
  const int mode = BMO_slot_int_get(op->slots_in, "mode");
  const float mix = BMO_slot_float_get(op->slots_in, "mix");
  const bool space_evenly = BMO_slot_bool_get(op->slots_in, "space_evenly");
  const int tension_int = BMO_slot_int_get(op->slots_in, "tension");
  const int iterations  = BMO_slot_int_get(op->slots_in, "iterations");
  const float tension   = float(tension_int) / 100.0f;
  const float min_angle = DEG2RADF(float(BMO_slot_int_get(op->slots_in, "min_angle")));
  const bool use_rail = BMO_slot_bool_get(op->slots_in, "use_rail");
  const int rail_mode = BMO_slot_int_get(op->slots_in, "rail_mode");
  const float rail_start = BMO_slot_float_get(op->slots_in, "rail_start");
  const float rail_end = BMO_slot_float_get(op->slots_in, "rail_end");

  /* Tag edges passed in via the slot, then collect ordered loops */
  BMO_slot_buffer_hflag_enable(bm, op->slots_in, "edges", BM_EDGE, BM_ELEM_TAG, false);

  Vector<EdgeFlowLoop> loops;
  const int dropped = edge_flow_collect_loops(bm, loops);
  if (dropped > 0) {
    BMO_error_raise(bm, op, BMO_ERROR_WARN, "Non-contiguous edge chains skipped");
  }

  if (mode == EDGE_FLOW_FLOW) {
    Array<Array<float3>> orig_cos(loops.size());

    for (const int j : loops.index_range()) {
      const EdgeFlowLoop &loop = loops[j];
      orig_cos[j].reinitialize(loop.verts.size());
      for (const int i : loop.verts.index_range()) {
        orig_cos[j][i] = loop.verts[i]->co;
      }
    }

    bool target_applied = false;
    for (int iter = 0; iter < iterations; iter++) {
      for (const EdgeFlowLoop &loop : loops) {
        for (const int i : loop.verts.index_range().drop_back(1)) {
          BMEdge *e = BM_edge_exists(loop.verts[i], loop.verts[i + 1]);
          BLI_assert(e != nullptr);

          /* Skip wire and boundary edges. */
          if (e->l == nullptr || BM_edge_is_boundary(e)) {
            continue;
          }

          BMVert *centers[2];
          float3 targets[2];
          int target_count = 0;

          BMIter l_iter;
          BMLoop *l;
          BM_ITER_ELEM (l, &l_iter, e, BM_LOOPS_OF_EDGE) {
            if (target_count == 2) {
              break;
            }

            float3 target;
            if (edge_flow_calc_spline_target(l, tension, min_angle, target)) {
              centers[target_count] = BM_edge_other_vert(e, l->v);
              targets[target_count] = target;
              target_count++;
            }
          }

          for (int k = 0; k < target_count; k++) {
            copy_v3_v3(centers[k]->co, targets[k]);
            target_applied = true;
          }
        }
      }
    }

    if (!loops.is_empty() && !target_applied) {
      BMO_error_raise(
          bm, op, BMO_ERROR_WARN, "Flow requires quads around selected edges, nothing changed");
      return;
    }

    const int blend_mode = BMO_slot_int_get(op->slots_in, "blend_mode");
    const float blend_start = BMO_slot_float_get(op->slots_in, "blend_start");
    const float blend_end = BMO_slot_float_get(op->slots_in, "blend_end");
    const bool blend_smooth = BMO_slot_int_get(op->slots_in, "blend_type") == 1;

    for (const int j : loops.index_range()) {
      const EdgeFlowLoop &loop = loops[j];
      const int count = int(loop.verts.size());
      const int start = (blend_mode == 1) ? int(roundf(float(count) * blend_start)) : int(blend_start);
      const int end = (blend_mode == 1) ? int(roundf(float(count) * blend_end)) : int(blend_end);
      edge_flow_blend_ends(loop, orig_cos[j], start, end, blend_smooth);
    }

    /* Blend each moved vert back toward its original position. */
    for (const int j : loops.index_range()) {
      const EdgeFlowLoop &loop = loops[j];
      for (const int i : loop.verts.index_range().drop_back(loop.is_cyclic ? 1 : 0)) {
        float3 blended;
        interp_v3_v3v3(blended, orig_cos[j][i], loop.verts[i]->co, mix);
        copy_v3_v3(loop.verts[i]->co, blended);
      }
    }

    return;
  }

  int cyclic_skipped = 0;
  for (EdgeFlowLoop &loop : loops) {
    if (loop.is_cyclic) {
      cyclic_skipped++;
      continue;
    }

    /* No interior verts. */
    if (int(loop.verts.size()) <= 2) {
      continue;
    }

    Array<float3> orig_cos(loop.verts.size());
    for (const int i : loop.verts.index_range()) {
      copy_v3_v3(orig_cos[i], loop.verts[i]->co);
    }

    BMVert *p1 = loop.verts.first();
    BMVert *p2 = loop.verts.last();

    if (mode == EDGE_FLOW_LINEAR) {
      if (space_evenly) {
        const int count = int(loop.verts.size()) - 1;
        const float3 dir = float3(p2->co) - float3(p1->co);

        for (const int i : loop.verts.index_range().drop_front(1).drop_back(1)) {
          const float3 co = float3(p1->co) + dir * (float(i) / float(count));
          const float3 blended = math::interpolate(float3(orig_cos[i]), co, mix);
          copy_v3_v3(loop.verts[i]->co, blended);
        }
      }
      else {
        /* space_evenly off flag */
        const float3 dir_norm = math::normalize(float3(p2->co) - float3(p1->co));

        for (const int i : loop.verts.index_range().drop_front(1).drop_back(1)) {
          const float3 co = float3(orig_cos[i]) - float3(p1->co);
          const float dir_scalar = math::dot(co, dir_norm);
          const float3 new_co = float3(p1->co) + dir_norm * dir_scalar;
          const float3 blended = math::interpolate(float3(orig_cos[i]), new_co, mix);
          copy_v3_v3(loop.verts[i]->co, blended);
        }
      }
    }
    else if (mode == EDGE_FLOW_CURVE) {
      const int count = int(loop.verts.size());
      const float3 start_co = p1->co;
      const float3 end_co = p2->co;

      /* Tangents of the loop's first and last edges. */
      const float3 dir1_full = float3(loop.verts[1]->co) - start_co;
      const float3 dir2_full = float3(loop.verts[count - 2]->co) - end_co;
      const float3 dir1 = math::normalize(dir1_full);
      const float3 dir2 = math::normalize(dir2_full);

      float3 ctrl1;
      float3 ctrl4;
      if (use_rail) {
        if (rail_mode == 0) {
          ctrl1 = start_co + (dir1_full - dir1 * rail_start);
          ctrl4 = end_co + (dir2_full - dir2 * rail_end);
        }
        else {
          ctrl1 = start_co + dir1_full * rail_start;
          ctrl4 = end_co + dir2_full * rail_end;
        }
      }
      else {
        ctrl1 = start_co;
        ctrl4 = end_co;
      }

      const float scale = math::distance(ctrl1, ctrl4) * 0.5f * tension;
      const float3 ctrl2 = ctrl1 + dir1 * scale;
      const float3 ctrl3 = ctrl4 + dir2 * scale;

      /* Resaample arc along spline and blend verts toward new positions. */
      Array<float3> spline(1000);
      edge_flow_sample_bezier(ctrl1, ctrl2, ctrl3, ctrl4, spline);
      edge_flow_map_onto_spline(loop.verts, spline);

      for (const int i : loop.verts.index_range().drop_front(1).drop_back(1)) {
        float3 blended;
        interp_v3_v3v3(blended, orig_cos[i], loop.verts[i]->co, mix);
        copy_v3_v3(loop.verts[i]->co, blended);
      }
    }
  }

  if (cyclic_skipped > 0) {
    BMO_error_raise(
        bm, op, BMO_ERROR_WARN, "Closed loops skipped, Linear and Curve require open chains");
  }
}

}  // namespace blender
