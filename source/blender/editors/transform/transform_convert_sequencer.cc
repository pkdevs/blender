/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include "DNA_screen_types.h"
#include "DNA_sequence_types.h"
#include "DNA_space_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_vector_c.hh"

#include "BKE_context.hh"

#include "ED_markers.hh"
#include "ED_sequencer.hh"

#include "SEQ_animation.hh"
#include "SEQ_channels.hh"
#include "SEQ_edit.hh"
#include "SEQ_effects.hh"
#include "SEQ_iterator.hh"
#include "SEQ_relations.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_transform.hh"

#include "UI_view2d.hh"

#include "transform.hh"
#include "transform_convert.hh"
#include "transform_mode.hh"

namespace blender::ed::transform {

#define STRIP_EDGE_PAN_INSIDE_PAD 3.5f
#define STRIP_EDGE_PAN_OUTSIDE_PAD 0.0f /* Disable clamping for panning, use whole screen. */
#define STRIP_EDGE_PAN_SPEED_RAMP 1.0f
#define STRIP_EDGE_PAN_MAX_SPEED 26.0f /* In UI units per second, slower than default. */
#define STRIP_EDGE_PAN_DELAY 0.5f
#define STRIP_EDGE_PAN_ZOOM_INFLUENCE 0.0f

namespace {

/** Used for sequencer transform. */
struct TransDataSeq {
  Strip *strip;
  /** A copy of #Strip.flag that may be modified for nested strips. */
  int flag;
  /** One of #SEQ_SELECT, #SEQ_LEFTSEL and #SEQ_RIGHTSEL. */
  short sel_flag;
  /** Initial location of the opposite handle for transition strips. */
  int opposite_handle;
};

}  // namespace

/* -------------------------------------------------------------------- */
/** \name Sequencer Transform Creation
 * \{ */

/* This function applies the rules for transforming a strip so duplicate
 * checks don't need to be added in multiple places.
 *
 * count and flag MUST be set.
 */
static void SeqTransInfo(TransInfo *t, Strip *strip, int *r_count, int *r_flag)
{
  Scene *scene = CTX_data_sequencer_scene(t->context);
  Editing *ed = seq::editing_get(scene);
  const ListBaseT<SeqTimelineChannel> *channels = seq::channels_displayed_get(ed);

  /* For extend we need to do some tricks. */
  if (t->mode == TFM_TIME_EXTEND) {

    /* *** Extend Transform *** */
    int cfra = scene->r.cfra;
    int left = strip->left_handle();
    int right = strip->right_handle(scene);

    if ((strip->flag & SEQ_SELECT) == 0 || seq::transform_is_locked(channels, strip)) {
      *r_count = 0;
      *r_flag = 0;
    }
    else {
      *r_count = 1; /* Unless its set to 0, extend will never set 2 handles at once. */
      *r_flag = (strip->flag | SEQ_SELECT) & ~(SEQ_LEFTSEL | SEQ_RIGHTSEL);

      if (t->frame_side == 'R') {
        if (right <= cfra) {
          *r_count = *r_flag = 0;
        } /* Ignore. */
        else if (left > cfra) {
        } /* Keep the selection. */
        else {
          *r_flag |= SEQ_RIGHTSEL;
        }
      }
      else {
        if (left >= cfra) {
          *r_count = *r_flag = 0;
        } /* Ignore. */
        else if (right < cfra) {
        } /* Keep the selection. */
        else {
          *r_flag |= SEQ_LEFTSEL;
        }
      }
    }
  }
  else {

    t->frame_side = 'B';

    /* *** Normal Transform *** */

    /* Count. */

    /* Non nested strips (reset selection and handles). */
    if ((strip->flag & SEQ_SELECT) == 0 || seq::transform_is_locked(channels, strip)) {
      *r_count = 0;
      *r_flag = 0;
    }
    else {
      if ((strip->flag & (SEQ_LEFTSEL | SEQ_RIGHTSEL)) == (SEQ_LEFTSEL | SEQ_RIGHTSEL)) {
        *r_flag = strip->flag;
        *r_count = 2; /* We need 2 transdata's. */
      }
      else {
        *r_flag = strip->flag;
        *r_count = 1; /* Selected or with a handle selected. */
      }
    }
  }
}

static int SeqTransCount(TransInfo *t, ListBaseT<Strip> *seqbase)
{
  int tot = 0, count, flag;

  for (Strip &strip : *seqbase) {
    SeqTransInfo(t, &strip, &count, &flag); /* Ignore the flag. */
    tot += count;
  }

  return tot;
}

static TransData *SeqToTransData(Scene *scene,
                                 TransData *td,
                                 TransData2D *td2d,
                                 TransDataSeq *tdsq,
                                 Strip *strip,
                                 int flag,
                                 int sel_flag)
{
  int start_left;

  switch (sel_flag) {
    case SEQ_SELECT:
      /* Use seq_tx_get_final_left() and an offset here
       * so transform has the left hand location of the strip.
       * `tdsq->start_offset` is used when flushing the tx data back. */
      start_left = strip->left_handle();
      td2d->loc[0] = start_left;
      break;
    case SEQ_LEFTSEL:
      start_left = strip->left_handle();
      td2d->loc[0] = start_left;
      tdsq->opposite_handle = strip->right_handle(scene);
      break;
    case SEQ_RIGHTSEL:
      td2d->loc[0] = strip->right_handle(scene);
      tdsq->opposite_handle = strip->left_handle();
      break;
  }

  td2d->loc[1] = strip->channel; /* Channel - Y location. */
  td2d->loc[2] = 0.0f;
  td2d->loc2d = nullptr;

  tdsq->strip = strip;

  /* Use instead of strip->flag for nested strips and other
   * cases where the selection may need to be modified. */
  tdsq->flag = flag;
  tdsq->sel_flag = sel_flag;

  td->extra = static_cast<void *>(tdsq); /* Allow us to update the strip from here. */

  td->flag = 0;
  td->loc = td2d->loc;
  copy_v3_v3(td->center, td->loc);
  copy_v3_v3(td->iloc, td->loc);

  memset(td->axismtx, 0, sizeof(td->axismtx));
  td->axismtx[2][2] = 1.0f;

  td->val = nullptr;

  td->flag |= TD_SELECTED;
  td->dist = 0.0;

  unit_m3(td->mtx);
  unit_m3(td->smtx);

  /* Time Transform (extend). */
  td->val = td2d->loc;
  td->ival = td2d->loc[0];

  return td;
}

static int SeqToTransData_build(
    TransInfo *t, ListBaseT<Strip> *seqbase, TransData *td, TransData2D *td2d, TransDataSeq *tdsq)
{
  Scene *scene = CTX_data_sequencer_scene(t->context);
  int count, flag;
  int tot = 0;

  for (Strip &strip : *seqbase) {

    SeqTransInfo(t, &strip, &count, &flag);

    /* Use 'flag' which is derived from strip->flag but modified for special cases. */
    if (flag & SEQ_SELECT) {
      if (flag & (SEQ_LEFTSEL | SEQ_RIGHTSEL)) {
        if (flag & SEQ_LEFTSEL) {
          SeqToTransData(scene, td++, td2d++, tdsq++, &strip, flag, SEQ_LEFTSEL);
          tot++;
        }
        if (flag & SEQ_RIGHTSEL) {
          SeqToTransData(scene, td++, td2d++, tdsq++, &strip, flag, SEQ_RIGHTSEL);
          tot++;
        }
      }
      else {
        SeqToTransData(scene, td++, td2d++, tdsq++, &strip, flag, SEQ_SELECT);
        tot++;
      }
    }
  }
  return tot;
}

static void free_transform_custom_data(TransCustomData *custom_data)
{
  if ((custom_data->data != nullptr) && custom_data->use_free) {
    TransSeq *ts = static_cast<TransSeq *>(custom_data->data);
    MEM_delete(static_cast<TransDataSeq *>(ts->tdseq));
    MEM_delete(ts);
    custom_data->data = nullptr;
  }
}

/* Canceled, need to update the strips display. */
static void seq_transform_cancel(TransInfo *t, Span<Strip *> transformed_strips)
{
  Scene *scene = CTX_data_sequencer_scene(t->context);
  ListBaseT<Strip> *seqbase = seq::active_seqbase_get(seq::editing_get(scene));

  seq::edit_cancel_removal(scene);

  if (t->remove_on_cancel) {
    for (Strip *strip : transformed_strips) {
      seq::edit_flag_for_removal(scene, strip);
    }
    seq::edit_remove_flagged_strips(scene, seqbase);
    vse::sync_active_scene_and_time_with_scene_strip(*t->context);
    return;
  }

  vse::sync_active_scene_and_time_with_scene_strip(*t->context);

  for (Strip *strip : transformed_strips) {
    /* Handle pre-existing overlapping strips even when operator is canceled.
     * This is necessary for #SEQUENCER_OT_duplicate_move macro for example. */
    if (seq::transform_test_overlap(scene, seqbase, strip)) {
      seq::transform_seqbase_shuffle(seqbase, strip, scene);
    }
  }
}

static ListBaseT<Strip> *seqbase_active_get(const TransInfo *t)
{
  Scene *scene = CTX_data_sequencer_scene(t->context);
  Editing *ed = seq::editing_get(scene);
  return seq::active_seqbase_get(ed);
}

bool seq_transform_check_overlap_flags(Span<Strip *> transformed_strips)
{
  for (Strip *strip : transformed_strips) {
    if (flag_is_set(strip->runtime->flag, seq::StripRuntimeFlag::Overlap)) {
      return true;
    }
  }
  return false;
}

static VectorSet<Strip *> seq_transform_collection_from_transdata(TransDataContainer *tc)
{
  VectorSet<Strip *> strips;
  TransData *td = tc->data;
  for (int a = 0; a < tc->data_len; a++, td++) {
    Strip *strip = (static_cast<TransDataSeq *>(td->extra))->strip;
    strips.add(strip);
  }
  return strips;
}

static void freeSeqData(TransInfo *t, TransDataContainer *tc, TransCustomData *custom_data)
{
  Scene *scene = CTX_data_sequencer_scene(t->context);
  Editing *ed = seq::editing_get(scene);
  if (ed == nullptr) {
    free_transform_custom_data(custom_data);
    return;
  }

  for (Strip &strip : *seqbase_active_get(t)) {
    strip.runtime->flag &= ~(seq::StripRuntimeFlag::ClampedLH | seq::StripRuntimeFlag::ClampedRH);
    strip.runtime->flag &= ~seq::StripRuntimeFlag::IgnoreChannelLock;
    strip.runtime->flag &= ~seq::StripRuntimeFlag::ShowOffsets;
  }

  VectorSet transformed_strips = seq_transform_collection_from_transdata(tc);
  seq::expand_strips(ed, transformed_strips, seq::StripRelation::Effects);

  if (t->state == TRANS_CANCEL) {
    seq_transform_cancel(t, transformed_strips);
    free_transform_custom_data(custom_data);
    return;
  }

  /* Last, handle overlap. `transform_handle_overlap` removes invalid transitions after the overlap
   * has been handled. If there's no overlap delete them here. */
  TransSeq *ts = static_cast<TransSeq *>(tc->custom.type.data);
  ListBaseT<Strip> *seqbasep = seqbase_active_get(t);
  const bool use_sync_markers = ((t->area->spacedata.first_as<SpaceSeq>())->flag &
                                 SEQ_MARKER_TRANS) != 0;
  if (seq_transform_check_overlap_flags(transformed_strips)) {
    seq::transform_handle_overlap(
        scene, seqbasep, transformed_strips, ts->time_dependent_strips, use_sync_markers);
  }
  else {
    seq::edit_remove_flagged_strips(scene, seqbasep);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_SEQUENCER_STRIPS);
  free_transform_custom_data(custom_data);
}

static VectorSet<Strip *> query_selected_strips_no_handles(ListBaseT<Strip> *seqbase)
{
  VectorSet<Strip *> strips;
  for (Strip &strip : *seqbase) {
    if ((strip.flag & SEQ_SELECT) != 0 && ((strip.flag & (SEQ_LEFTSEL | SEQ_RIGHTSEL)) == 0)) {
      strips.add(&strip);
    }
  }
  return strips;
}

enum SeqInputSide {
  SEQ_INPUT_LEFT = -1,
  SEQ_INPUT_RIGHT = 1,
};

static Strip *effect_input_get(Strip *effect, SeqInputSide side)
{
  Strip *input = effect->input1;
  if (effect->input2 && (effect->input2->left_handle() - effect->input1->left_handle()) * side > 0)
  {
    input = effect->input2;
  }
  return input;
}

static Strip *effect_base_input_get(Strip *effect, SeqInputSide side)
{
  Strip *input = effect, *strip_iter = effect;
  while (strip_iter != nullptr) {
    input = strip_iter;
    strip_iter = effect_input_get(strip_iter, side);
  }
  return input;
}

/**
 * Strips that aren't selected, but their position entirely depends on
 * transformed strips. This collection is used to offset animation.
 */
static void query_time_dependent_strips_strips(TransInfo *t,
                                               VectorSet<Strip *> &time_dependent_strips)
{
  Scene *scene = CTX_data_sequencer_scene(t->context);
  Editing *ed = seq::editing_get(scene);
  ListBaseT<Strip> *seqbase = seqbase_active_get(t);

  /* Query dependent strips where used strips do not have handles selected.
   * If all inputs of any effect even indirectly(through another effect) points to selected strip,
   * its position will change. */

  VectorSet<Strip *> strips_no_handles = query_selected_strips_no_handles(seqbase);
  time_dependent_strips.add_multiple(strips_no_handles);

  seq::expand_strips(ed, strips_no_handles, seq::StripRelation::EffectChain);
  bool strip_added = true;

  while (strip_added) {
    strip_added = false;

    for (Strip *strip : strips_no_handles) {
      if (time_dependent_strips.contains(strip)) {
        continue; /* Strip is already in collection, skip it. */
      }

      /* If both input1 and input2 exist, both must be selected. */
      if (strip->input1 && time_dependent_strips.contains(strip->input1)) {
        if (strip->input2 && !time_dependent_strips.contains(strip->input2)) {
          continue;
        }
        strip_added = true;
        time_dependent_strips.add(strip);
      }
    }
  }

  /* Query dependent strips where used strips do have handles selected.
   * If any 2-input effect changes position because handles were moved, animation should be offset.
   * With single input effect, it is less likely desirable to move animation. */

  VectorSet selected_strips = seq::query_selected_strips(seqbase);
  seq::expand_strips(ed, selected_strips, seq::StripRelation::EffectChain);
  for (Strip *strip : selected_strips) {
    /* Check only 2 input effects. */
    if (strip->input1 == nullptr || strip->input2 == nullptr) {
      continue;
    }

    /* Find immediate base inputs(left and right side). */
    Strip *input_left = effect_base_input_get(strip, SEQ_INPUT_LEFT);
    Strip *input_right = effect_base_input_get(strip, SEQ_INPUT_RIGHT);

    if ((input_left->flag & SEQ_RIGHTSEL) != 0 && (input_right->flag & SEQ_LEFTSEL) != 0) {
      time_dependent_strips.add(strip);
    }
  }

  /* Remove all non-effects. */
  time_dependent_strips.remove_if([&](Strip *strip) {
    return seq::transform_strip_can_be_translated(strip) || strip->is_overlap_transition();
  });
}

static bool cutpoint_moves(Strip *left_strip, Strip *right_strip)
{
  auto valid_selection{[](const Strip *strip, eStripFlag valid_selection_side) -> bool {
    const bool left = flag_is_set(strip->flag, SEQ_LEFTSEL);
    const bool right = flag_is_set(strip->flag, SEQ_RIGHTSEL);
    const bool select = flag_is_set(strip->flag, SEQ_SELECT);
    const bool invalid_only_side = valid_selection_side == SEQ_LEFTSEL ? right : left;
    return (left && right) || (select && !invalid_only_side);
  }};
  return valid_selection(left_strip, SEQ_RIGHTSEL) && valid_selection(right_strip, SEQ_LEFTSEL);
}

static bool create_non_transition_clamp_data(TransInfo *t, const Scene *scene, Strip *strip)
{
  Editing *ed = seq::editing_get(scene);
  TransSeq *ts = static_cast<TransSeq *>(TRANS_DATA_CONTAINER_FIRST_SINGLE(t)->custom.type.data);

  bool left_sel = (strip->flag & SEQ_LEFTSEL);
  bool right_sel = (strip->flag & SEQ_RIGHTSEL);

  /* If any strips start out with hold offsets visible, disable handle clamping on init. */
  const bool clamp_default = (U.sequencer_editor_flag & USER_SEQ_ED_CLAMP_STRIPS_BY_DEFAULT);
  if (clamp_default && (strip->startofs < 0 || strip->end_offset() < 0) &&
      !seq::transform_single_image_check(strip))
  {
    t->modifiers &= ~MOD_STRIP_SOFT_CLAMP;
  }

  /* If both handles are selected, there must be enough underlying content to clamp holds. */
  bool can_clamp_holds = !(left_sel && right_sel) ||
                         (strip->content_length() >=
                          strip->right_handle(scene) - strip->left_handle());
  can_clamp_holds &= !seq::transform_single_image_check(strip);

  /* A handle is selected. Update x-axis clamping data. */
  if (left_sel || right_sel) {
    if (left_sel) {
      /* Ensure that this strip's left handle cannot pass its right handle. */
      if (!(left_sel && right_sel)) {
        int offset = (strip->right_handle(scene) - 1) - strip->left_handle();
        ts->hard_clamp.xmax = min_ii(ts->hard_clamp.xmax, offset);
      }

      if (can_clamp_holds) {
        /* Ensure that the left handle's frame is greater than or equal to the content start. */
        ts->soft_clamp_min = max_ii(ts->soft_clamp_min, -strip->startofs);
      }

      /* Prevent transitions from going past strip bounds. */
      Strip *left_transition = seq::get_transition_on_side(ed, strip, seq::Side::Left);
      if (left_transition && !right_sel && (left_transition->input1->flag & SEQ_RIGHTSEL)) {
        const int offset = strip->right_handle(scene) - left_transition->right_handle(scene);
        ts->soft_clamp_max = min_ii(ts->soft_clamp_max, offset);

        /* Prevent the transition from overlapping the adjacent transition. Don't clamp if the
         * adjacent transition is also moved. */
        Strip *left_adjacent_transition = seq::get_transition_on_side(
            ed, left_transition->input1, seq::Side::Left);
        if (left_adjacent_transition &&
            !cutpoint_moves(left_adjacent_transition->input1, left_adjacent_transition->input2))
        {
          const int adjacent_offset = left_adjacent_transition->right_handle(scene) -
                                      left_transition->left_handle();
          ts->hard_clamp.xmin = max_ii(ts->hard_clamp.xmin, adjacent_offset);
        }
      }
    }
    if (right_sel) {
      if (!(left_sel && right_sel)) {
        /* Ensure that this strip's right handle cannot pass its left handle. */
        int offset = (strip->left_handle() + 1) - strip->right_handle(scene);
        ts->hard_clamp.xmin = max_ii(ts->hard_clamp.xmin, offset);
      }

      if (can_clamp_holds) {
        /* Ensure that the right handle's frame is less than or equal to the content end. */
        ts->soft_clamp_max = min_ii(ts->soft_clamp_max, strip->end_offset());
      }

      /* Prevent transitions from going past strip bounds. */
      Strip *right_transition = seq::get_transition_on_side(ed, strip, seq::Side::Right);
      if (right_transition && !left_sel && (right_transition->input2->flag & SEQ_LEFTSEL)) {
        const int offset = strip->left_handle() - right_transition->left_handle();
        ts->soft_clamp_min = max_ii(ts->soft_clamp_min, offset);

        /* Prevent the transition from overlapping the adjacent transition. Don't clamp if the
         * adjacent transition is also moved. */
        Strip *right_adjacent_transition = seq::get_transition_on_side(
            ed, right_transition->input2, seq::Side::Right);
        if (right_adjacent_transition &&
            !cutpoint_moves(right_adjacent_transition->input1, right_adjacent_transition->input2))
        {
          const int adjacent_offset = right_adjacent_transition->left_handle() -
                                      right_transition->right_handle(scene);
          ts->hard_clamp.xmax = min_ii(ts->hard_clamp.xmax, adjacent_offset);
        }
      }
    }
    return true; /* Has handle(s) selected. */
  }
  return false;
}

/* Clamp same channel transitions to strip / content area. Clamping overlap of adjacent transitions
 * is done separately in #create_adjacent_transition_clamp_data. */
static void create_transition_clamp_data(TransInfo *t, const Scene *scene, Strip *strip)
{
  TransSeq *ts = static_cast<TransSeq *>(TRANS_DATA_CONTAINER_FIRST_SINGLE(t)->custom.type.data);

  t->modifiers |= MOD_STRIP_SYMMETRIC;

  const bool left_sel = flag_is_set(strip->flag, SEQ_LEFTSEL);
  const bool right_sel = flag_is_set(strip->flag, SEQ_RIGHTSEL);

  const Strip *left_input = strip->input1;
  const Strip *right_input = strip->input2;

  if (left_sel || right_sel) {
    left_input->runtime->flag |= seq::StripRuntimeFlag::ShowOffsets;
    right_input->runtime->flag |= seq::StripRuntimeFlag::ShowOffsets;
  }

  /* If the transition is already past the content start/end, disable clamping. */
  if ((!right_input->is_effect() && strip->left_handle() < right_input->content_start()) ||
      (!left_input->is_effect() && strip->right_handle(scene) > left_input->content_end(scene)))
  {
    t->modifiers &= ~MOD_STRIP_SOFT_CLAMP;
  }

  const int left_edge_offset = left_input->left_handle() - strip->left_handle();
  const int left_cutpoint_offset = (right_input->left_handle() - 1) - strip->left_handle();
  /* Effects have no offsets. Content start returns a float value. */
  const int left_content_offset = right_input->is_effect() ?
                                      (INT_MIN + 1) :
                                      int(ceil(right_input->content_start()) -
                                          strip->left_handle());
  /* Prevent the left handle from moving past left strip bounds or right strip content bounds.*/
  if (left_sel) {
    ts->hard_clamp.xmin = max_ii(ts->hard_clamp.xmin, left_edge_offset);
    ts->hard_clamp.xmax = min_ii(ts->hard_clamp.xmax, left_cutpoint_offset);
    ts->soft_clamp_min = max_ii(ts->soft_clamp_min, left_content_offset);
  }
  /* Symmetric strip / content bounds clamping for the left handle. Done when only the right handle
   * is selected, since selecting both handles moves the entire transition. */
  else if (right_sel) {
    ts->symmetric_hard_clamp_max = min_ii(ts->symmetric_hard_clamp_max, -left_edge_offset);
    ts->symmetric_hard_clamp_min = max_ii(ts->symmetric_hard_clamp_min, -left_cutpoint_offset);
    ts->symmetric_soft_clamp_max = min_ii(ts->symmetric_soft_clamp_max, -left_content_offset);
  }

  const int right_edge_offset = right_input->right_handle(scene) - strip->right_handle(scene);
  const int right_cutpoint_offset = (right_input->left_handle() + 1) - strip->right_handle(scene);
  /* Effects have no offsets. Content start returns a float value. */
  const int right_content_offset = left_input->is_effect() ?
                                       INT_MAX :
                                       int(floor(left_input->content_end(scene)) -
                                           strip->right_handle(scene));
  /* Prevent the right handle from moving past right strip bounds or left strip content bounds.*/
  if (right_sel) {
    ts->hard_clamp.xmax = min_ii(ts->hard_clamp.xmax, right_edge_offset);
    ts->hard_clamp.xmin = max_ii(ts->hard_clamp.xmin, right_cutpoint_offset);
    ts->soft_clamp_max = min_ii(ts->soft_clamp_max, right_content_offset);
  }
  /* Symmetric strip / content bounds clamping for the right handle. Done when only the left handle
   * is selected, since selecting both handles moves the entire transition. */
  else if (left_sel) {
    ts->symmetric_hard_clamp_min = max_ii(ts->symmetric_hard_clamp_min, -right_edge_offset);
    ts->symmetric_hard_clamp_max = min_ii(ts->symmetric_hard_clamp_max, -right_cutpoint_offset);
    ts->symmetric_soft_clamp_min = max_ii(ts->symmetric_soft_clamp_min, -right_content_offset);
  }
}

static void create_adjacent_transition_clamp_data(
    TransSeq *ts, const Scene *scene, VectorSet<std::pair<Strip *, Strip *>> &adjacent_pairs)
{
  for (auto [left_strip, right_strip] : adjacent_pairs) {
    const bool left_strip_left_sel = left_strip->flag & SEQ_LEFTSEL;
    const bool left_strip_right_sel = left_strip->flag & SEQ_RIGHTSEL;
    const bool right_strip_left_sel = right_strip->flag & SEQ_LEFTSEL;
    const bool right_strip_right_sel = right_strip->flag & SEQ_RIGHTSEL;

    const int offset = right_strip->left_handle() - left_strip->right_handle(scene);
    /* When both transitions are being scaled toward each other. Uneven distances are truncated
     * toward zero, leaving a one frame gap to prevent overlap. */
    const int symmetric_offset = offset / 2;

    /* Moving the right handle of the left transition: clamp to right transition bounds. */
    if (left_strip_right_sel && !right_strip_left_sel) {
      ts->hard_clamp.xmax = min_ii(ts->hard_clamp.xmax, offset);
      if (right_strip_right_sel) {
        ts->symmetric_hard_clamp_max = min_ii(ts->symmetric_hard_clamp_max, symmetric_offset);
      }
    }
    /* Moving the left handle of the left transition: clamp to right transition bounds if
     * symmetric transitions are enabled. */
    if ((left_strip_left_sel && !left_strip_right_sel) && !right_strip_right_sel) {
      ts->symmetric_hard_clamp_min = max_ii(ts->symmetric_hard_clamp_min, -offset);
    }

    /* Moving the left handle of the right transition: clamp to left transition bounds. */
    if (!left_strip_right_sel && right_strip_left_sel) {
      ts->hard_clamp.xmin = max_ii(ts->hard_clamp.xmin, -offset);
      if (left_strip_left_sel) {
        ts->symmetric_hard_clamp_min = max_ii(ts->symmetric_hard_clamp_min, -symmetric_offset);
      }
    }
    /* Moving the right handle of the right transition: clamp to left transition bounds if
     * symmetric transitions are enabled. */
    if (!left_strip_left_sel && (!right_strip_left_sel && right_strip_right_sel)) {
      ts->symmetric_hard_clamp_max = min_ii(ts->symmetric_hard_clamp_max, offset);
    }
  }
}

/* (left, right) adjacent transition pairs to clamp transitions from overlapping each other. */
static void get_transition_pairs(Editing *ed,
                                 Strip *middle_transition,
                                 VectorSet<std::pair<Strip *, Strip *>> &r_adjacent_pairs)
{
  Strip *left_adjacent_transition = seq::get_transition_on_side(
      ed, middle_transition->input1, seq::Side::Left);
  Strip *right_adjacent_transition = seq::get_transition_on_side(
      ed, middle_transition->input2, seq::Side::Right);

  if (left_adjacent_transition) {
    r_adjacent_pairs.add({left_adjacent_transition, middle_transition});
  }
  if (right_adjacent_transition) {
    r_adjacent_pairs.add({middle_transition, right_adjacent_transition});
  }
}

static void create_trans_seq_clamp_data(TransInfo *t, const Scene *scene)
{
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  TransSeq *ts = static_cast<TransSeq *>(tc->custom.type.data);
  Editing *ed = seq::editing_get(scene);

  if (U.sequencer_editor_flag & USER_SEQ_ED_CLAMP_STRIPS_BY_DEFAULT) {
    t->modifiers |= MOD_STRIP_SOFT_CLAMP;
  }

  /* Prevent snaps and change in `values` past `hard_clamp` for all selected strips. */
  BLI_rcti_init(&ts->hard_clamp, INT_MIN, INT_MAX, -seq::MAX_CHANNELS, seq::MAX_CHANNELS);

  bool only_handles_selected = true;
  bool valid_transition_input_selection = true;

  bool has_transition_handles = false;
  bool has_non_transition = false;

  VectorSet<std::pair<Strip *, Strip *>> adjacent_transition_pairs;
  /* Loop over the strips that can be transformed, ignoring locked channels. */
  for (Strip *strip : seq_transform_collection_from_transdata(tc)) {
    /* Early break if both x and y are hard clamped to 0. */
    if (has_non_transition && has_transition_handles) {
      break;
    }
    if (seq::transform_is_locked(seq::channels_displayed_get(ed), strip)) {
      continue;
    }
    /* Limit y-axis clamping to at least the channel bounds. */
    ts->hard_clamp.ymin = max_ii(ts->hard_clamp.ymin, 1 - strip->channel);
    ts->hard_clamp.ymax = min_ii(ts->hard_clamp.ymax, seq::MAX_CHANNELS - strip->channel);

    if (strip->is_overlap_transition()) {
      if ((strip->flag & (SEQ_LEFTSEL | SEQ_RIGHTSEL)) != 0) {
        has_transition_handles = true;
      }
      else if (!flag_is_set(strip->input1->flag, SEQ_SELECT) ||
               !flag_is_set(strip->input2->flag, SEQ_SELECT))
      {
        valid_transition_input_selection = false;
        break;
      }
      get_transition_pairs(ed, strip, adjacent_transition_pairs);
      create_transition_clamp_data(t, scene, strip);
    }
    else if (strip->is_effect_with_inputs()) {
      /* Non-transition effects with inputs don't have handles. */
      only_handles_selected = false;
      has_non_transition = true;
      /* If there is an effect strip without its inputs selected, prevent any x-direction movement,
       * since these strips are tied to their inputs and can only move up and down. */
      if (!(strip->input1->flag & SEQ_SELECT) &&
          (!strip->input2 || !(strip->input2->flag & SEQ_SELECT)))
      {
        ts->hard_clamp.xmin = 0;
        ts->hard_clamp.xmax = 0;
      }
    }
    else {
      has_non_transition = true;
      only_handles_selected &= create_non_transition_clamp_data(t, scene, strip);
    }
  }

  /* 1. Non-transitions and transition handles can't be selected at the same time. Other code
   *    should prevent this invalid selection state, but prevent movement if in such state.
   * 2. Transition inputs must be selected if the transition is selected but its handles aren't.
   */
  const bool invalid_selection = (has_transition_handles && has_non_transition) ||
                                 !valid_transition_input_selection;
  if (invalid_selection) {
    ts->hard_clamp.xmin = 0;
    ts->hard_clamp.xmax = 0;
  }

  /* TODO(john): This ensures that y-axis movement is restricted only if all of the selected items
   * are handles, since currently it is possible to select whole strips and handles at the same
   * time. This should be removed for 5.0 when we make this behavior impossible. */
  if (only_handles_selected || invalid_selection) {
    ts->hard_clamp.ymin = 0;
    ts->hard_clamp.ymax = 0;
  }

  if (!invalid_selection) {
    create_adjacent_transition_clamp_data(ts, scene, adjacent_transition_pairs);
  }

  /* If either axis is locked (min/max offset is zero), then movement is only possible along one
   * axis, and distinguishing them makes no sense, so just disable both axis constraints. */
  if ((ts->hard_clamp.xmin == 0 && ts->hard_clamp.xmax == 0) ||
      (ts->hard_clamp.ymin == 0 && ts->hard_clamp.ymax == 0))
  {
    t->flag |= T_NO_CONSTRAINT;
  }
}

static void createTransSeqData(bContext *C, TransInfo *t)
{
  Scene *scene = CTX_data_sequencer_scene(C);
  if (!scene) {
    return;
  }
  Editing *ed = seq::editing_get(scene);
  TransData *td = nullptr;
  TransData2D *td2d = nullptr;
  TransDataSeq *tdsq = nullptr;
  TransSeq *ts = nullptr;

  int count = 0;

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  if (ed == nullptr) {
    tc->data_len = 0;
    return;
  }

  /* Disable cursor wrapping for edge pan. */
  if (t->mode == TFM_TRANSLATION) {
    t->flag |= T_NO_CURSOR_WRAP;
  }

  tc->custom.type.free_cb = freeSeqData;
  t->frame_side = transform_convert_frame_side_dir_get(t, float(scene->r.cfra));

  count = SeqTransCount(t, ed->current_strips());

  /* Allocate memory for data. */
  tc->data_len = count;

  /* Stop if trying to build list if nothing selected. */
  if (count == 0) {
    return;
  }

  tc->custom.type.data = ts = MEM_new<TransSeq>(__func__);
  tc->custom.type.use_free = true;
  td = tc->data = MEM_new_array_zeroed<TransData>(tc->data_len, "TransSeq TransData");
  td2d = tc->data_2d = MEM_new_array_zeroed<TransData2D>(tc->data_len, "TransSeq TransData2D");
  ts->tdseq = tdsq = MEM_new_array_zeroed<TransDataSeq>(tc->data_len, "TransSeq TransDataSeq");

  /* Custom data to enable edge panning during transformation. */
  view2d_edge_pan_init(t->context,
                       &ts->edge_pan,
                       STRIP_EDGE_PAN_INSIDE_PAD,
                       STRIP_EDGE_PAN_OUTSIDE_PAD,
                       STRIP_EDGE_PAN_SPEED_RAMP,
                       STRIP_EDGE_PAN_MAX_SPEED,
                       STRIP_EDGE_PAN_DELAY,
                       STRIP_EDGE_PAN_ZOOM_INFLUENCE);
  view2d_edge_pan_set_limits(&ts->edge_pan, -FLT_MAX, FLT_MAX, 1, seq::MAX_CHANNELS + 1);
  ts->initial_v2d_cur = t->region->v2d.cur;

  /* Loop 2: build transdata array. */
  SeqToTransData_build(t, ed->current_strips(), td, td2d, tdsq);

  create_trans_seq_clamp_data(t, scene);

  query_time_dependent_strips_strips(t, ts->time_dependent_strips);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform Flush
 * \{ */

static void view2d_edge_pan_loc_compensate(TransInfo *t, float r_offset[2])
{
  TransSeq *ts = static_cast<TransSeq *>(TRANS_DATA_CONTAINER_FIRST_SINGLE(t)->custom.type.data);

  const rctf rect_prev = t->region->v2d.cur;

  if (t->options & CTX_VIEW2D_EDGE_PAN) {
    if (t->state == TRANS_CANCEL) {
      view2d_edge_pan_cancel(t->context, &ts->edge_pan);
    }
    else {
      /* Edge panning functions expect window coordinates, mval is relative to region. */
      const int xy[2] = {
          t->region->winrct.xmin + int(t->mval[0]),
          t->region->winrct.ymin + int(t->mval[1]),
      };
      view2d_edge_pan_apply(t->context, &ts->edge_pan, xy);
    }
  }

  if (t->state != TRANS_CANCEL) {
    if (!BLI_rctf_compare(&rect_prev, &t->region->v2d.cur, FLT_EPSILON)) {
      /* Additional offset due to change in view2D rect. */
      BLI_rctf_transform_pt_v(&t->region->v2d.cur, &rect_prev, r_offset, r_offset);
      transformViewUpdate(t);
    }
  }
}

static void get_strip_offsets(TransInfo *t,
                              TransData *td,
                              const float edge_pan_offset[2],
                              float r_offset[2],
                              float r_offset_clamped[2])
{
  /* Apply extra offset caused by edge panning. */
  add_v2_v2(td->loc, edge_pan_offset);

  sub_v2_v2v2(r_offset, td->loc, td->iloc);
  copy_v2_v2(r_offset_clamped, r_offset);

  if (t->state != TRANS_CANCEL) {
    transform_convert_sequencer_clamp(t, r_offset_clamped);
  }
}

static void flush_transition_transforms(TransInfo *t,
                                        TransData *td,
                                        float offset_clamped[2],
                                        std::optional<int> &r_left_new,
                                        std::optional<int> &r_right_new)
{
  const TransDataSeq *tdsq = static_cast<TransDataSeq *>(td->extra);

  /* Location before the start of the transform. */
  const int x_old = round_fl_to_int(td->iloc[0]);
  /* Clamped offset from #x_old. */
  const int x_offset = round_fl_to_int(offset_clamped[0]);

  /* New value for the handle opposite of #tdsq->sel_flag. */
  int opposite_new = tdsq->opposite_handle;
  if (flag_is_set(t->modifiers, MOD_STRIP_SYMMETRIC)) {
    opposite_new -= x_offset;
  }
  /* If both handles are selected the entire transition is moved. In this case update the handles
   * with the same offset instead of doing symmetric handle movement. */
  switch (tdsq->sel_flag) {
    case SEQ_LEFTSEL:
      r_left_new = x_old + x_offset;
      r_right_new = r_right_new.value_or(opposite_new);
      break;
    case SEQ_RIGHTSEL:
      r_right_new = x_old + x_offset;
      r_left_new = r_left_new.value_or(opposite_new);
      break;
  }
}

/* Flushes the translation and channel updates. Possible updates to the handle locations are
 * returned and handled outside this function. This is to avoid unexpected handle clamping when
 * both handles are selected and the `new_frame` is right of the old one. See #126191. */
static int flush_strip_transforms(Scene *scene,
                                  TransData *td,
                                  const float offset[2],
                                  const float offset_clamped[2],
                                  std::optional<int> &r_left_new,
                                  std::optional<int> &r_right_new)
{
  const TransDataSeq *tdsq = static_cast<TransDataSeq *>(td->extra);
  Strip *strip = tdsq->strip;

  const int new_frame = round_fl_to_int(td->iloc[0] + offset_clamped[0]);

  /* Compute handle clamping state to be drawn. */
  strip->runtime->flag &= ~(seq::StripRuntimeFlag::ClampedLH | seq::StripRuntimeFlag::ClampedRH);

  if (!seq::transform_single_image_check(strip) && !strip->is_effect()) {
    if (offset_clamped[0] > offset[0] && new_frame == strip->content_start()) {
      strip->runtime->flag |= seq::StripRuntimeFlag::ClampedLH;
    }
    else if (offset_clamped[0] < offset[0] && new_frame == strip->content_end(scene)) {
      strip->runtime->flag |= seq::StripRuntimeFlag::ClampedRH;
    }
  }

  int delta_x = 0;
  int new_channel = strip->channel;

  switch (tdsq->sel_flag) {
    case SEQ_SELECT: {
      new_channel = round_fl_to_int(td->iloc[1] + offset_clamped[1]);
      delta_x = new_frame - strip->left_handle();

      /* Locked strips aren't included in the transform data.
       * Set the channel for effect strips too. */
      strip->channel_set(new_channel);

      if (!seq::transform_strip_can_be_translated(strip)) {
        break;
      }

      seq::transform_translate_strip(scene, strip, delta_x);
      break;
    }
    case SEQ_LEFTSEL: {
      r_left_new = new_frame;
      delta_x = new_frame - strip->left_handle();
      break;
    }
    case SEQ_RIGHTSEL: {
      r_right_new = new_frame;
      delta_x = new_frame - strip->right_handle(scene);
      break;
    }
  }

  /* Move attached transitions with the strips if both transition inputs are selected. Only do it
   * for the right transition to avoid moving it twice. */
  Editing *ed = seq::editing_get(scene);
  Strip *right_transition = seq::get_transition_on_side(ed, strip, seq::Side::Right);
  if (right_transition && cutpoint_moves(right_transition->input1, right_transition->input2) &&
      ELEM(tdsq->sel_flag, SEQ_SELECT, SEQ_RIGHTSEL))
  {
    seq::transform_translate_strip(scene, right_transition, delta_x);
    right_transition->channel_set(new_channel);
  }

  /* Return delta for animation offset. */
  return delta_x;
}

static void flushTransSeq(TransInfo *t)
{
  /* Editing null check already done. */
  ListBaseT<Strip> *seqbasep = seqbase_active_get(t);
  Scene *scene = CTX_data_sequencer_scene(t->context);

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  /* This is calculated for offsetting animation of effects that change position with inputs.
   * Maximum(positive or negative) value is used for historic reasons. This works fairly well in
   * most scenarios, but there can be some edge cases. */
  int max_offset = 0;

  float edge_pan_offset[2] = {0.0f, 0.0f};
  view2d_edge_pan_loc_compensate(t, edge_pan_offset);

  /* Flush to 2D vector from internally used 3D vector. */
  TransData *td_iteration = tc->data;
  for (int a = 0; a < tc->data_len; a++, td_iteration++) {
    TransData *td1 = td_iteration; /* Left or right handle. */
    TransData *td2 = nullptr;      /* Right handle if both are selected. */

    Strip *strip = static_cast<TransDataSeq *>(td1->extra)->strip;

    /* If both the left and right handles for the same strip are selected, they are next to each
     * other in #tc. In this case get the #TransData for both handles (#td1 and #td2). */
    if (a + 1 < tc->data_len) {
      TransData *td_tmp = td_iteration + 1;
      Strip *strip_tmp = static_cast<TransDataSeq *>(td_tmp->extra)->strip;
      if (strip_tmp == strip) {
        td2 = td_tmp;
        a++;
        td_iteration++;
      }
    }

    const int left_old = strip->left_handle();
    const int right_old = strip->right_handle(scene);
    std::optional<int> left_new{};
    std::optional<int> right_new{};

    auto flush_transforms{[&](TransData *td) {
      float offset[2], offset_clamped[2];
      get_strip_offsets(t, td, edge_pan_offset, offset, offset_clamped);

      if (strip->is_overlap_transition()) {
        flush_transition_transforms(t, td, offset_clamped, left_new, right_new);
      }
      else {
        const int delta = flush_strip_transforms(
            scene, td, offset, offset_clamped, left_new, right_new);
        if (abs(delta) > abs(max_offset)) {
          max_offset = delta;
        }
      }
    }};

    /* Flush one or both handles of the strip depending on selection. */
    flush_transforms(td1);
    if (td2 != nullptr) {
      flush_transforms(td2);
    }

    if (left_new && right_new) {
      strip->handles_set(scene, *left_new, *right_new);
    }
    else if (left_new) {
      strip->left_handle_set(scene, *left_new);
    }
    else if (right_new) {
      strip->right_handle_set(scene, *right_new);
    }

    /* Move transition keyframes to match the new bounds. */
    if (strip->is_overlap_transition()) {
      seq::transform_transition_animdata(scene,
                                         strip,
                                         left_old,
                                         right_old,
                                         left_new.value_or(left_old),
                                         right_new.value_or(right_old));
    }
  }

  TransSeq *ts = static_cast<TransSeq *>(TRANS_DATA_CONTAINER_FIRST_SINGLE(t)->custom.type.data);

  /* Update animation for effects. */
  for (Strip *strip : ts->time_dependent_strips) {
    seq::offset_animdata(scene, strip, max_offset);
  }

  /* Need to do the overlap check in a new loop otherwise adjacent strips
   * will not be updated and we'll get false positives. */
  VectorSet transformed_strips = seq_transform_collection_from_transdata(tc);
  seq::expand_strips(seq::editing_get(scene), transformed_strips, seq::StripRelation::Effects);

  seq::transform_set_overlap_flags(scene, seqbasep, transformed_strips);
}

static void recalcData_sequencer(TransInfo *t)
{
  TransData *td;
  int a;
  Strip *strip_prev = nullptr;

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  Scene *scene = CTX_data_sequencer_scene(t->context);

  for (a = 0, td = tc->data; a < tc->data_len; a++, td++) {
    TransDataSeq *tdsq = static_cast<TransDataSeq *>(td->extra);
    Strip *strip = tdsq->strip;

    if (strip != strip_prev) {
      seq::relations_invalidate_cache(scene, strip);
    }

    strip_prev = strip;
  }

  vse::sync_active_scene_and_time_with_scene_strip(*t->context);
  DEG_id_tag_update(&scene->id, ID_RECALC_SEQUENCER_STRIPS);

  flushTransSeq(t);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Special After Transform Sequencer
 * \{ */

static void special_aftertrans_update__sequencer(bContext *C, TransInfo *t)
{
  Scene *scene = CTX_data_sequencer_scene(C);
  SpaceSeq *sseq = t->area->spacedata.first_as<SpaceSeq>();
  if ((sseq->flag & SPACE_SEQ_DESELECT_STRIP_HANDLE) != 0 &&
      transform_mode_edge_seq_slide_use_restore_handle_selection(t))
  {
    TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
    VectorSet<Strip *> strips = seq_transform_collection_from_transdata(tc);
    for (Strip *strip : strips) {
      strip->flag &= ~(SEQ_LEFTSEL | SEQ_RIGHTSEL);
    }
  }

  sseq->flag &= ~SPACE_SEQ_DESELECT_STRIP_HANDLE;

  /* #freeSeqData in `transform_conversions.cc` does this
   * keep here so the `else` at the end won't run. */
  if (t->state == TRANS_CANCEL) {
    return;
  }

  /* Marker transform, not especially nice but we may want to move markers
   * at the same time as strips in the Video Sequencer. */
  if (sseq->flag & SEQ_MARKER_TRANS) {
    /* Can't use #TFM_TIME_EXTEND
     * for some reason EXTEND is changed into TRANSLATE, so use frame_side instead. */

    if (t->mode == TFM_SEQ_SLIDE) {
      if (t->frame_side == 'B') {
        ED_markers_post_apply_transform(
            &scene->markers, scene, TFM_TIME_TRANSLATE, t->values_final[0], t->frame_side);
      }
    }
    else if (ELEM(t->frame_side, 'L', 'R')) {
      ED_markers_post_apply_transform(
          &scene->markers, scene, TFM_TIME_EXTEND, t->values_final[0], t->frame_side);
    }
  }
}

bool transform_convert_sequencer_clamp(const TransInfo *t, float r_val[2])
{
  if (t->data_container_len == 0) {
    /* During drag and drop, there is no custom data. We don't need to clamp here anyways,
     * since we're not adjusting handles, and channels are already clamped in drag/drop code. */
    return false;
  }

  const TransSeq *ts = static_cast<TransSeq *>(
      TRANS_DATA_CONTAINER_FIRST_SINGLE(t)->custom.type.data);
  int val[2] = {round_fl_to_int(r_val[0]), round_fl_to_int(r_val[1])};
  bool clamped = false;

  /* Unconditional channel, retiming key, and handle clamping. Should never be ignored. */
  if (BLI_rcti_clamp_pt_v(&ts->hard_clamp, val)) {
    clamped = true;
  }

  auto clamp_x{[&val, &clamped](int min, int max) {
    if (val[0] < min) {
      val[0] = min;
      clamped = true;
    }
    else if (val[0] > max) {
      val[0] = max;
      clamped = true;
    }
  }};

  /* Optional clamping of handles to underlying holds. Can be disabled by the user. */
  if (t->modifiers & MOD_STRIP_SOFT_CLAMP) {
    clamp_x(ts->soft_clamp_min, ts->soft_clamp_max);
  }

  /* Clamping of symmetric transitions. */
  if ((t->modifiers & MOD_STRIP_SYMMETRIC) != 0) {
    clamp_x(ts->symmetric_hard_clamp_min, ts->symmetric_hard_clamp_max);
    /* Optional clamping of handles to content range of inputs. Can be disabled by the user. */
    if (t->modifiers & MOD_STRIP_SOFT_CLAMP) {
      clamp_x(ts->symmetric_soft_clamp_min, ts->symmetric_soft_clamp_max);
    }
  }

  if (clamped) {
    r_val[0] = float(val[0]);
    r_val[1] = float(val[1]);
  }
  return clamped;
}

/** \} */

TransConvertTypeInfo TransConvertType_Sequencer = {
    /*flags*/ (T_POINTS | T_2D_EDIT),
    /*create_trans_data*/ createTransSeqData,
    /*recalc_data*/ recalcData_sequencer,
    /*special_aftertrans_update*/ special_aftertrans_update__sequencer,
};

}  // namespace blender::ed::transform
