/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include "MEM_guardedalloc.h"

#include "DNA_screen_types.h"
#include "DNA_sequence_types.h"
#include "DNA_space_types.h"

#include "BLI_math_matrix_c.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_rect.hh"

#include "BKE_context.hh"

#include "SEQ_edit.hh"
#include "SEQ_iterator.hh"
#include "SEQ_relations.hh"
#include "SEQ_retiming.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_transform.hh"

#include "transform.hh"
#include "transform_convert.hh"

namespace blender::ed::transform {

namespace {

/** Used for sequencer retiming transform. */
struct TransDataSeq {
  Strip *strip;
  /* Transition to be moved when retiming keys for the same frame are selected in adjacent strips.
   * Mirrors the behavior of strip handle moving. Initially set null by MEM_new_array_zeroed. */
  Strip *attached_transition;
  int orig_timeline_frame;
  int key_index; /* Some actions may need to destroy original data, use index to access it. */
};

}  // namespace

static Strip *get_transition_of_left_input(Editing *ed, const Strip *strip)
{
  Span<Strip *> effect_strips = seq::lookup_effects_by_strip(ed, strip);
  for (Strip *effect : effect_strips) {
    if (effect->is_same_channel_transition() && effect->input1 == strip) {
      return effect;
    }
  }
  return nullptr;
}

static TransData *SeqToTransData(const Scene *scene,
                                 Strip *strip,
                                 const SeqRetimingKey *key,
                                 TransData *td,
                                 TransData2D *td2d,
                                 TransDataSeq *tdseq)
{

  td2d->loc[0] = seq::retiming_key_frame_get(scene, strip, key);
  td2d->loc[1] = key->retiming_factor;
  td2d->loc2d = nullptr;
  td->loc = td2d->loc;
  copy_v3_v3(td->iloc, td->loc);
  copy_v3_v3(td->center, td->loc);
  memset(td->axismtx, 0, sizeof(td->axismtx));
  td->axismtx[2][2] = 1.0f;
  unit_m3(td->mtx);
  unit_m3(td->smtx);

  tdseq->strip = strip;
  tdseq->orig_timeline_frame = seq::retiming_key_frame_get(scene, strip, key);
  tdseq->key_index = seq::retiming_key_index_get(strip, key);

  /* Move transitions when retiming keys for the same frame are selected in adjacent strips. */
  const bool key_moves_right_handle = tdseq->orig_timeline_frame == strip->right_handle(scene) &&
                                      !seq::retiming_key_is_transition_type(key);
  if (key_moves_right_handle) {
    Strip *transition = get_transition_of_left_input(seq::editing_get(scene), strip);
    if (transition) {
      SeqRetimingKey *right_input_key = seq::retiming_key_get_by_frame(
          scene, transition->input2, tdseq->orig_timeline_frame);
      const bool key_moves_left_handle = right_input_key &&
                                         flag_is_set(right_input_key->flag, SEQ_KEY_SELECTED) &&
                                         !seq::retiming_key_is_transition_type(right_input_key);
      if (key_moves_left_handle) {
        tdseq->attached_transition = transition;
      }
    }
  }

  td->extra = static_cast<void *>(tdseq);
  td->flag |= TD_SELECTED;
  td->dist = 0.0;

  return td;
}

static void freeSeqData(TransInfo *t, TransDataContainer *tc, TransCustomData *custom_data)
{
  const TransData *const td = tc->data;
  Scene *scene = t->scene;
  Editing *ed = seq::editing_get(t->scene);
  ListBaseT<Strip> *seqbasep = seq::active_seqbase_get(ed);

  /* Handle overlapping strips. */

  VectorSet<Strip *> transformed_strips;
  for (int i = 0; i < tc->data_len; i++) {
    Strip *strip = (static_cast<TransDataSeq *>((td + i)->extra))->strip;
    transformed_strips.add(strip);
  }

  seq::iterator_set_expand(ed, transformed_strips, seq::query_strip_direct_effect_chain);

  VectorSet<Strip *> dependant;
  dependant.add_multiple(transformed_strips);
  dependant.remove_if([&](Strip *strip) { return seq::transform_strip_can_be_translated(strip); });

  /* Last, handle overlap. `transform_handle_overlap` removes invalid transitions after the overlap
   * has been handled. If there's no overlap delete them here. */
  if (seq_transform_check_overlap_flags(transformed_strips)) {
    const bool use_sync_markers = ((static_cast<SpaceSeq *>(t->area->spacedata.first))->flag &
                                   SEQ_MARKER_TRANS) != 0;
    seq::transform_handle_overlap(
        scene, seqbasep, transformed_strips, dependant, use_sync_markers);
  }
  else {
    seq::edit_remove_flagged_strips(scene, seqbasep);
  }

  if ((custom_data->data != nullptr) && custom_data->use_free) {
    TransSeq *ts = static_cast<TransSeq *>(custom_data->data);
    MEM_delete(static_cast<TransDataSeq *>(ts->tdseq));
    MEM_delete(ts);
    custom_data->data = nullptr;
  }
}

static void create_trans_seq_clamp_data(TransInfo *t, const Scene *scene)
{
  const Editing *ed = seq::editing_get(scene);

  const TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  TransSeq *ts = static_cast<TransSeq *>(tc->custom.type.data);

  t->modifiers |= MOD_STRIP_CLAMP_HOLDS;

  /* Prevent snaps and change in `values` past `hard_clamp` for all selected
     retiming keys. */
  BLI_rcti_init(&ts->hard_clamp, INT_MIN, INT_MAX, 0, 0);

  TransData *td = tc->data;
  for (int i = 0; i < tc->data_len; i++, td++) {
    const TransDataSeq *tdseq = static_cast<TransDataSeq *>(td->extra);
    const Strip *strip = tdseq->strip;
    const MutableSpan keys = seq::retiming_keys_get(strip);
    SeqRetimingKey *key = &keys[tdseq->key_index];

    /* Transitions are moved when retiming keys for the same frame are selected in adjacent strips.
     * If this is the case, soft clamp the transition inside input strip bounds. */
    const Strip *transition = tdseq->attached_transition;
    if (transition) {
      int min_offset = transition->input1->left_handle() - transition->left_handle();
      int max_offset = transition->input2->right_handle(scene) - transition->right_handle(scene);
      ts->soft_clamp_min = max_ii(ts->soft_clamp_min, min_offset);
      ts->soft_clamp_max = min_ii(ts->soft_clamp_max, max_offset);
    }

    /* Transition retiming key. */
    if (seq::retiming_key_is_transition_type(key) &&
        !seq::retiming_selection_has_whole_transition(ed, key))
    {
      SeqRetimingKey *key_start = seq::retiming_transition_start_get(key);
      SeqRetimingKey *key_end = key_start + 1;
      SeqRetimingKey *key_prev = key_start - 1;
      SeqRetimingKey *key_next = key_end + 1;

      const float midpoint = key_start->original_strip_frame_index;

      /* Ensure start transition key cannot pass the previous key, or linked end transition key
       * cannot pass the next key. This transform behavior is symmetrical and limited by the
       * smallest distance between keys. */
      const int max_offset = min_ii(key_start->strip_frame_index - key_prev->strip_frame_index - 1,
                                    key_next->strip_frame_index - key_end->strip_frame_index - 1);

      if (key_start->flag & SEQ_KEY_SELECTED) {
        /* Ensure start transition key cannot pass the midpoint. */
        ts->hard_clamp.xmax = min_ii(midpoint - key_start->strip_frame_index, ts->hard_clamp.xmax);
        ts->hard_clamp.xmin = max_ii(-max_offset, ts->hard_clamp.xmin);
      }
      else {
        /* Ensure end transition key cannot pass the midpoint. */
        ts->hard_clamp.xmin = max_ii(-(key_end->strip_frame_index - midpoint - 1),
                                     ts->hard_clamp.xmin);
        ts->hard_clamp.xmax = min_ii(max_offset, ts->hard_clamp.xmax);
      }
    }
    /* Non-transition retiming key. */
    else {
      SeqRetimingKey *key_prev = key - 1, *key_next = key + 1;
      if (!seq::retiming_is_last_key(strip, key)) {
        /* Ensure that this key cannot pass the next key. */
        ts->hard_clamp.xmax = min_ii(key_next->strip_frame_index - key->strip_frame_index - 1,
                                     ts->hard_clamp.xmax);
        /* TODO(john): There is an off-by-one error for the last "fake" key's
         * `strip_frame_index`, which is 1 less than it should be. This is not an immediate issue
         * but should be fixed.
         */
      }
      if (key->strip_frame_index != 0) {
        /* Ensure that this key cannot pass the previous key. */
        ts->hard_clamp.xmin = max_ii(-(key->strip_frame_index - key_prev->strip_frame_index - 1),
                                     ts->hard_clamp.xmin);
      }
    }
  }
}

static void createTransSeqRetimingData(bContext * /*C*/, TransInfo *t)
{
  const Editing *ed = seq::editing_get(t->scene);
  if (ed == nullptr) {
    return;
  }

  const Map selection = seq::retiming_selection_get(seq::editing_get(t->scene));

  if (selection.is_empty()) {
    return;
  }

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  tc->custom.type.free_cb = freeSeqData;
  tc->data_len = selection.size();

  TransSeq *ts = MEM_new<TransSeq>(__func__);
  tc->custom.type.data = ts;
  tc->custom.type.use_free = true;

  TransData *td = MEM_new_array_zeroed<TransData>(tc->data_len, "TransSeq TransData");
  TransData2D *td2d = MEM_new_array_zeroed<TransData2D>(tc->data_len, "TransSeq TransData2D");
  TransDataSeq *tdseq = MEM_new_array_zeroed<TransDataSeq>(tc->data_len, "TransSeq TransDataSeq");
  tc->data = td;
  tc->data_2d = td2d;
  ts->tdseq = tdseq;

  for (auto item : selection.items()) {
    SeqToTransData(t->scene, item.value, item.key, td++, td2d++, tdseq++);
  }

  create_trans_seq_clamp_data(t, t->scene);
}

static void recalcData_sequencer_retiming(TransInfo *t)
{
  const TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  const TransData *td = nullptr;
  int i;

  VectorSet<Strip *> transformed_strips;

  for (i = 0, td = tc->data; i < tc->data_len; i++, td++) {
    const TransDataSeq *tdseq = static_cast<TransDataSeq *>(td->extra);
    Strip *strip = tdseq->strip;

    if (!seq::retiming_show_keys(strip)) {
      continue;
    }

    float offset[2];
    float offset_clamped[2];
    sub_v2_v2v2(offset, td->loc, td->iloc);
    copy_v2_v2(offset_clamped, offset);

    transform_convert_sequencer_clamp(t, offset_clamped);
    const int new_frame = round_fl_to_int(td->iloc[0] + offset_clamped[0]);

    transformed_strips.add(strip);

    /* Calculate translation. */

    const MutableSpan keys = seq::retiming_keys_get(strip);
    SeqRetimingKey *key = &keys[tdseq->key_index];

    const int delta_x = new_frame - seq::retiming_key_frame_get(t->scene, strip, key);

    if (seq::retiming_key_is_transition_type(key) &&
        !seq::retiming_selection_has_whole_transition(seq::editing_get(t->scene), key))
    {
      seq::retiming_transition_key_frame_set(t->scene, strip, key, round_fl_to_int(new_frame));
    }
    else {
      seq::retiming_key_frame_set(t->scene, strip, key, new_frame);
    }

    /* Move transitions when retiming keys for the same frame are selected in adjacent strips. */
    if (tdseq->attached_transition) {
      seq::transform_translate_strip(t->scene, tdseq->attached_transition, delta_x);
    }

    seq::relations_invalidate_cache(t->scene, strip);
  }

  /* Test overlap, displays red outline. */
  Editing *ed = seq::editing_get(t->scene);
  ListBaseT<Strip> *seqbase = seq::active_seqbase_get(ed);

  seq::iterator_set_expand(ed, transformed_strips, seq::query_strip_direct_effect_chain);
  seq::transform_set_overlap_flags(t->scene, seqbase, transformed_strips);
}

TransConvertTypeInfo TransConvertType_SequencerRetiming = {
    /*flags*/ (T_POINTS | T_2D_EDIT),
    /*create_trans_data*/ createTransSeqRetimingData,
    /*recalc_data*/ recalcData_sequencer_retiming,
};

}  // namespace blender::ed::transform
