/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */
#include <chrono>
#include <thread>

#include "BLI_bounds.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_threads.hh"
#include "BLI_vector.hh"

#include "DNA_action_types.h"
#include "DNA_object_types.h"

#include "BLT_translation.hh"

#include "BKE_anim_visualization.h"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "GPU_batch.hh"

#include "BLO_read_write.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "MEM_guardedalloc.h"

namespace blender {

namespace bke::animviz {

struct TargetData {
  EvaluationTarget target;
  EvalCallback eval;
  UpdateCallback update;
  FinishCallback finish;
  bool finished_left = false;
  bool finished_right = false;

  TargetData(const EvaluationTarget &target,
             EvalCallback eval,
             UpdateCallback update,
             FinishCallback finish)
      : target(target), eval(eval), update(update), finish(finish)
  {
  }
};

/* Data visible to the worker thread. */
struct WorkerData {
  /* Can be set from the main thread to tell the evaluating thread to start over.
   * Is used because we cannot just stop the depsgraph evaluation. */
  std::atomic<bool> restart;
  /* The frame of the scene at the last registration. Is used to compute the dirty frames in a
   * reasonable order. */
  std::atomic<int> current_frame;

  TicketMutex *data_mutex;
  /* The depsgraph to evaluate in the background. All copy on eval nodes have to be
   * evaluated before it is sent to the thread. */
  Depsgraph *dg = nullptr;
  Vector<TargetData> target_data;
  /* All frames on which animation data was changed. The range around those frames will be
   * evaluated. */
  VectorSet<int> dirty_frames;

  WorkerData()
  {
    data_mutex = BLI_ticket_mutex_alloc();
  }

  ~WorkerData()
  {
    BLI_ticket_mutex_free(data_mutex);
  }

  void lock()
  {
    BLI_ticket_mutex_lock(data_mutex);
  }
  void unlock()
  {
    BLI_ticket_mutex_unlock(data_mutex);
  }

  int find_target_index(const EvaluationTarget &target) const
  {
    for (const int i : this->target_data.index_range()) {
      if (this->target_data[i].target == target) {
        return i;
      }
    }
    return -1;
  }
};

struct BGEvalJobData {
  /* Has to stay first. */
  WorkerData worker_data;
  /* All targets currently evaluating. */
  Set<EvaluationTarget> active_targets;
};

/* Evaluate all dirty frames and a range around them for which the `eval` callback returns true. */
static bool evaluate_dirty_frames(WorkerData *eval_data, wmJobWorkerStatus *worker_status)
{
  eval_data->lock();
  /* Making a copy for threadsafe access. */
  VectorSet<int> dirty_frames = eval_data->dirty_frames;
  eval_data->unlock();
  if (dirty_frames.is_empty()) {
    return true;
  }
  const int cfra = eval_data->current_frame.load(std::memory_order_acquire);
  while (!dirty_frames.is_empty()) {
    Vector<int> dirty_frames_in_range;

    /* Start with the dirty frame closest to the current scene frame. */
    int center_frame = dirty_frames[0];
    for (const int frame : dirty_frames) {
      if (abs(frame - cfra) < abs(center_frame - cfra)) {
        center_frame = frame;
      }
    }
    dirty_frames_in_range.append(center_frame);

    eval_data->lock();
    /* Always evaluate center frame. */
    DEG_evaluate_on_framechange(eval_data->dg, center_frame);
    for (TargetData &target_data : eval_data->target_data) {
      const EvaluationTarget &target = target_data.target;
      target_data.eval(eval_data->dg, target, center_frame);
    }
    eval_data->unlock();

    Bounds<int> evaluated_range = {center_frame, center_frame};

    bool all_done_left = false;
    bool all_done_right = false;
    while (!all_done_left || !all_done_right) {
      int frame;
      /* TODO what about frame skipping i.e. every second frame. Would be determined by the
       * evaluation targets. */
      const bool update_left = abs(evaluated_range.min - center_frame) <
                               abs(evaluated_range.max - center_frame);
      if ((update_left && !all_done_left) || all_done_right) {
        frame = evaluated_range.min - 1;
        evaluated_range.min -= 1;
      }
      else {
        frame = evaluated_range.max + 1;
        evaluated_range.max += 1;
      }

      if (dirty_frames.contains(frame)) {
        /* Happens if the modifications overlap in their affected range. */
        dirty_frames_in_range.append(frame);
      }

      all_done_left = true;
      all_done_right = true;
      eval_data->lock();
      DEG_graph_relations_update(eval_data->dg);
      DEG_evaluate_on_framechange(eval_data->dg, frame);
      for (TargetData &target_data : eval_data->target_data) {
        const EvaluationTarget &target = target_data.target;
        const bool modified_data = target_data.eval(eval_data->dg, target, frame);
        if (!modified_data) {
          if (frame < center_frame) {
            target_data.finished_left = true;
          }
          else {
            target_data.finished_right = true;
          }
        }
        all_done_left &= target_data.finished_left;
        all_done_right &= target_data.finished_right;
      }
      eval_data->unlock();
      worker_status->do_update = true;
      if (eval_data->restart.load(std::memory_order_acquire)) {
        return false;
      }

      /* TODO remove before flight. */
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    /* Only remove dirty frames when their full range was evaluated. */
    for (const int frame : dirty_frames_in_range) {
      dirty_frames.remove(frame);
    }
    eval_data->lock();
    eval_data->dirty_frames = dirty_frames;
    eval_data->unlock();
  }

  return true;
}

static void run_job(void *job_data, wmJobWorkerStatus *worker_status)
{
  WorkerData *eval_data = static_cast<WorkerData *>(job_data);
  bool completed = false;
  while (!completed) {
    eval_data->restart.store(false, std::memory_order_release);
    completed = evaluate_dirty_frames(eval_data, worker_status);
  }
}

static Depsgraph *build_worker_depsgraph(Main &bmain,
                                         Scene &scene,
                                         ViewLayer &view_layer,
                                         const Set<EvaluationTarget> &targets)
{
  Vector<ID *> ids;
  ids.reserve(targets.size());
  for (const EvaluationTarget &target : targets) {
    ids.append(target.id);
  }
  Depsgraph *dg = DEG_graph_new(&bmain, &scene, &view_layer, DAG_EVAL_VIEWPORT);
  DEG_graph_build_from_ids(dg, ids);
  /* Evaluate once on the main thread so the copy on eval nodes have run. */
  DEG_evaluate_on_refresh(dg);
  /* Don't allow reading main from the worker thread. */
  DEG_set_allow_read_from_main(dg, false);
  return dg;
}

static void update_job(void *job_data)
{
  WorkerData *eval_data = static_cast<WorkerData *>(job_data);
  for (TargetData &target_data : eval_data->target_data) {
    target_data.update(target_data.target);
  }
}

static void finish_job(void *job_data)
{
  WorkerData *eval_data = static_cast<WorkerData *>(job_data);
  for (TargetData &target_data : eval_data->target_data) {
    target_data.finish(target_data.target);
  }
}

static void free_job_data(void *job_data)
{
  BGEvalJobData *eval_data = static_cast<BGEvalJobData *>(job_data);
  DEG_graph_free(eval_data->worker_data.dg);
  MEM_delete(eval_data);
}

constexpr const char *job_name = "Async Evaluate Frame Range";

void background_eval_register(Main &bmain,
                              wmWindowManager &wm,
                              wmWindow &window,
                              Scene &scene,
                              ViewLayer &view_layer,
                              const EvaluationTarget &target,
                              EvalCallback eval_cb,
                              UpdateCallback update_cb,
                              FinishCallback free_cb)
{
  /* TODO dont rebuild DG every time. Keep data around for a while before ending worker. This will
   * mean regular updates to the same data are more efficient. */
  wmJob *wm_job = WM_jobs_get(
      &wm, &window, &scene, job_name, eWM_JobFlag(0), WM_JOB_TYPE_MOTION_PATH_EVAL);

  /* For the future, explicitly passing in a dirty frame lets us compute the depsgraph from an
   * arbitrary frame, not just the scene frame. This could be useful when modifying data in the
   * Graph Editor. */
  const int dirty_frame = BKE_scene_frame_get(&scene);
  const int current_frame = BKE_scene_frame_get(&scene);
  if (WM_jobs_is_running(wm_job)) {
    BGEvalJobData *eval_data = static_cast<BGEvalJobData *>(WM_jobs_customdata_get(wm_job));

    if (eval_data->active_targets.contains(target)) {
      eval_data->worker_data.lock();
      const int target_index = eval_data->worker_data.find_target_index(target);
      BLI_assert(target_index != -1);
      TargetData &data = eval_data->worker_data.target_data[target_index];
      data.finished_left = false;
      data.finished_right = false;
      eval_data->worker_data.dirty_frames.add(dirty_frame);
      eval_data->worker_data.current_frame.store(current_frame, std::memory_order_release);
      DEG_set_allow_read_from_main(eval_data->worker_data.dg, true);
      DEG_evaluate_on_refresh(eval_data->worker_data.dg);
      DEG_set_allow_read_from_main(eval_data->worker_data.dg, false);
      eval_data->worker_data.restart.store(true, std::memory_order_release);
      eval_data->worker_data.unlock();
      return;
    }

    eval_data->worker_data.lock();

    /* Add the target to the target list. */
    eval_data->active_targets.add(target);
    DEG_graph_free(eval_data->worker_data.dg);
    eval_data->worker_data.dg = build_worker_depsgraph(
        bmain, scene, view_layer, eval_data->active_targets);
    eval_data->worker_data.target_data.append({target, eval_cb, update_cb, free_cb});
    eval_data->worker_data.dirty_frames.add(dirty_frame);
    eval_data->worker_data.current_frame.store(current_frame, std::memory_order_release);

    eval_data->worker_data.restart.store(true, std::memory_order_release);
    eval_data->worker_data.unlock();
    return;
  }

  BGEvalJobData *eval_data = MEM_new<BGEvalJobData>(__func__);
  eval_data->active_targets.add(target);
  eval_data->worker_data.target_data.append({target, eval_cb, update_cb, free_cb});
  eval_data->worker_data.dg = build_worker_depsgraph(
      bmain, scene, view_layer, eval_data->active_targets);
  eval_data->worker_data.dirty_frames.add(dirty_frame);
  eval_data->worker_data.current_frame.store(current_frame, std::memory_order_release);

  WM_jobs_customdata_set(wm_job, eval_data, free_job_data);
  WM_jobs_callbacks(wm_job, run_job, nullptr, update_job, finish_job);
  WM_jobs_start(&wm, wm_job);
}

void background_eval_deregister(wmWindowManager &wm,
                                wmWindow &window,
                                Scene &scene,
                                const EvaluationTarget &target)
{
  wmJob *wm_job = WM_jobs_get(
      &wm, &window, &scene, job_name, eWM_JobFlag(0), WM_JOB_TYPE_MOTION_PATH_EVAL);

  if (!WM_jobs_is_running(wm_job)) {
    /* No job to deregister from. */
    return;
  }

  BGEvalJobData *eval_data = static_cast<BGEvalJobData *>(WM_jobs_customdata_get(wm_job));
  if (!eval_data->active_targets.contains(target)) {
    /* Given target not registered. */
    return;
  }

  eval_data->worker_data.lock();

  eval_data->active_targets.remove(target);
  const int target_index = eval_data->worker_data.find_target_index(target);
  BLI_assert(target_index != -1);
  eval_data->worker_data.target_data.remove_and_reorder(target_index);
  Main *bmain = DEG_get_bmain(eval_data->worker_data.dg);
  ViewLayer *view_layer = DEG_get_input_view_layer(eval_data->worker_data.dg);
  DEG_graph_free(eval_data->worker_data.dg);
  eval_data->worker_data.dg = build_worker_depsgraph(
      *bmain, scene, *view_layer, eval_data->active_targets);

  /* No need to restart the job since no data was added to the worker that would need
   * evaluation. */
  eval_data->worker_data.unlock();
}

}  // namespace bke::animviz

namespace bke::motionpath {

struct Runtime {
  /* Heap allocated data for the worker thread to write to. */
  /* TODO maybe not required to heap allocate. */
  WorkerBuffer *worker_buffer;
  /* Async animviz job data. Used to deregister from the job. */
  wmWindowManager *wm;
  wmWindow *window;
  Scene *job_owner;
  bke::animviz::EvaluationTarget target;
};

void async_eval_allocate(bMotionPath &motion_path,
                         wmWindowManager &wm,
                         wmWindow &window,
                         Scene &job_owner,
                         bke::animviz::EvaluationTarget &target)
{
  if (motion_path.runtime->worker_buffer) {
    /* Data already created for async eval. */
    return;
  }

  WorkerBuffer *buffer = MEM_new<WorkerBuffer>(
      __func__, motion_path.start_frame, motion_path.length);
  for (const int i : IndexRange(motion_path.length)) {
    copy_v3_v3(buffer->points[i], motion_path.points[i].co);
    buffer->flags[i] = motion_path.points[i].flag;
    buffer->ready_to_copy[i].store(false, std::memory_order_release);
  }

  motion_path.runtime->worker_buffer = buffer;
  motion_path.runtime->wm = &wm;
  motion_path.runtime->window = &window;
  motion_path.runtime->job_owner = &job_owner;
  motion_path.runtime->target = target;
}

void async_eval_free(bMotionPath &motion_path)
{
  if (!motion_path.runtime->worker_buffer) {
    BLI_assert_unreachable();
    return;
  }
  MEM_delete(motion_path.runtime->worker_buffer);
  motion_path.runtime->worker_buffer = nullptr;
  motion_path.runtime->wm = nullptr;
  motion_path.runtime->window = nullptr;
  motion_path.runtime->job_owner = nullptr;
}

WorkerBuffer *get_worker_buffer(bMotionPath &motion_path)
{
  if (!motion_path.runtime) {
    return nullptr;
  }
  return motion_path.runtime->worker_buffer;
}

}  // namespace bke::motionpath

/* ******************************************************************** */
/* Animation Visualization */

void animviz_settings_init(bAnimVizSettings *avs)
{
  /* sanity check */
  if (avs == nullptr) {
    return;
  }

  /* path settings */
  avs->path_bc = avs->path_ac = 10;

  avs->path_sf = 1;   /* XXX: Take from scene instead? */
  avs->path_ef = 250; /* XXX: Take from scene instead? */

  avs->path_viewflag = MOTIONPATH_VIEW_KFRAS | MOTIONPATH_VIEW_KFNOS;

  avs->path_step = 1;

  avs->path_bakeflag |= MOTIONPATH_BAKE_HEADS;
}

/* ------------------- */

void animviz_free_motionpath_cache(bMotionPath *mpath)
{
  /* sanity check */
  if (mpath == nullptr) {
    return;
  }

  /* free the path if necessary */
  if (mpath->points) {
    MEM_delete(mpath->points);
  }

  GPU_VERTBUF_DISCARD_SAFE(mpath->points_vbo);
  GPU_BATCH_DISCARD_SAFE(mpath->batch_line);
  GPU_BATCH_DISCARD_SAFE(mpath->batch_points);

  /* reset the relevant parameters */
  mpath->points = nullptr;
  mpath->length = 0;
}

void animviz_free_motionpath(bMotionPath *mpath)
{
  /* sanity check */
  if (mpath == nullptr) {
    return;
  }

  if (mpath->runtime->worker_buffer) {
    bke::animviz::background_eval_deregister(*mpath->runtime->wm,
                                             *mpath->runtime->window,
                                             *mpath->runtime->job_owner,
                                             mpath->runtime->target);
    bke::motionpath::async_eval_free(*mpath);
  }
  MEM_delete(mpath->runtime);
  animviz_free_motionpath_cache(mpath);

  /* now the instance itself */
  MEM_delete(mpath);
}

/* ------------------- */

bMotionPath *animviz_copy_motionpath(const bMotionPath *mpath_src)
{
  bMotionPath *mpath_dst;

  if (mpath_src == nullptr) {
    return nullptr;
  }

  mpath_dst = MEM_dupalloc(mpath_src);
  mpath_dst->points = MEM_dupalloc(mpath_src->points);

  /* should get recreated on draw... */
  mpath_dst->points_vbo = nullptr;
  mpath_dst->batch_line = nullptr;
  mpath_dst->batch_points = nullptr;
  mpath_dst->runtime = MEM_new<bke::motionpath::Runtime>(__func__);

  return mpath_dst;
}

/* ------------------- */

bMotionPath *animviz_verify_motionpaths(ReportList *reports,
                                        Scene *scene,
                                        Object *ob,
                                        bPoseChannel *pchan)
{
  bAnimVizSettings *avs;
  bMotionPath *mpath, **dst;

  /* sanity checks */
  if (ELEM(nullptr, scene, ob)) {
    return nullptr;
  }

  /* get destination data */
  if (pchan) {
    /* Paths for pose-channel - assume that pose-channel belongs to the object. */
    avs = &ob->pose->avs;
    dst = &pchan->mpath;
  }
  else {
    /* paths for object */
    avs = &ob->avs;
    dst = &ob->mpath;
  }

  /* Avoid 0 size allocations. */
  if (avs->path_sf >= avs->path_ef) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Motion path frame extents invalid for %s (%d to %d)%s",
                (pchan) ? pchan->name : ob->id.name,
                avs->path_sf,
                avs->path_ef,
                (avs->path_sf == avs->path_ef) ? RPT_(", cannot have single-frame paths") : "");
    return nullptr;
  }

  /* Adding 1 because the avs range is inclusive on both ends. */
  const int expected_length = (avs->path_ef - avs->path_sf) + 1;
  BLI_assert(expected_length > 1); /* Because the `if` above. */

  /* If there is already a motionpath, just return that, provided its settings
   * are ok (saves extra free+alloc). */
  if (*dst != nullptr) {
    mpath = *dst;

    if (avs->path_bakeflag & MOTIONPATH_BAKE_CAMERA_SPACE) {
      mpath->flag |= MOTIONPATH_FLAG_BAKE_CAMERA;
    }
    else {
      mpath->flag &= ~MOTIONPATH_FLAG_BAKE_CAMERA;
    }

    /* Only reuse a path if it was already a valid path, and of the expected length. */
    if (mpath->start_frame != mpath->end_frame && mpath->length == expected_length) {
      mpath->start_frame = avs->path_sf;
      mpath->end_frame = avs->path_ef + 1;
      return mpath;
    }

    /* Clear the existing cache, to allocate a new one below. */
    animviz_free_motionpath_cache(mpath);
  }
  else {
    mpath = MEM_new<bMotionPath>("bMotionPath");
    mpath->runtime = MEM_new<bke::motionpath::Runtime>(__func__);
    *dst = mpath;
  }

  /* Copy mpath settings from the viz settings. */
  mpath->start_frame = avs->path_sf;
  mpath->end_frame = avs->path_ef + 1;
  mpath->length = expected_length;

  if (avs->path_bakeflag & MOTIONPATH_BAKE_HEADS) {
    mpath->flag |= MOTIONPATH_FLAG_BHEAD;
  }
  else {
    mpath->flag &= ~MOTIONPATH_FLAG_BHEAD;
  }

  if (avs->path_bakeflag & MOTIONPATH_BAKE_CAMERA_SPACE) {
    mpath->flag |= MOTIONPATH_FLAG_BAKE_CAMERA;
  }
  else {
    mpath->flag &= ~MOTIONPATH_FLAG_BAKE_CAMERA;
  }

  /* Set default custom values (RGB). */
  mpath->color[0] = 1.0;
  mpath->color[1] = 0.0;
  mpath->color[2] = 0.0;

  mpath->color_post[0] = 0.1;
  mpath->color_post[1] = 1.0;
  mpath->color_post[2] = 0.1;

  mpath->line_thickness = 2;
  mpath->flag |= MOTIONPATH_FLAG_LINES;

  /* Allocate a cache. */
  mpath->points = MEM_new_array<bMotionPathVert>(mpath->length, "bMotionPathVerts");

  /* Tag viz settings as currently having some path(s) which use it. */
  avs->path_bakeflag |= MOTIONPATH_BAKE_HAS_PATHS;

  return mpath;
}

void animviz_motionpath_blend_write(BlendWriter *writer, bMotionPath *mpath)
{
  /* sanity checks */
  if (mpath == nullptr) {
    return;
  }
  bke::motionpath::Runtime *backup = mpath->runtime;
  mpath->runtime = nullptr;
  /* firstly, just write the motionpath struct */
  writer->write_struct(mpath);

  /* now write the array of data */
  writer->write_struct_array(mpath->length, mpath->points);
  mpath->runtime = backup;
}

void animviz_motionpath_blend_read_data(BlendDataReader *reader, bMotionPath *mpath)
{
  /* sanity check */
  if (mpath == nullptr) {
    return;
  }

  /* relink points cache */
  BLO_read_array_and_validate_size(reader, &mpath->points, &mpath->length);

  mpath->points_vbo = nullptr;
  mpath->batch_line = nullptr;
  mpath->batch_points = nullptr;
  mpath->runtime = MEM_new<bke::motionpath::Runtime>(__func__);
}

}  // namespace blender
