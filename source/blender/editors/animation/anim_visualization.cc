/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <chrono>
#include <thread>

#include "BLI_bounds.hh"
#include "BLI_threads.hh"
#include "BLI_vector.hh"

#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "WM_types.hh"
#include "WM_api.hh"

#include "ED_anim_api.hh"

namespace blender::animviz {

struct TargetData {
  EvaluationTarget target;
  void *buffer;
  EvalCallback eval;
  UpdateCallback update;
  FreeBufferCallback free_buffer;
  bool finished_left = false;
  bool finished_right = false;

  TargetData(const EvaluationTarget &target,
             void *buffer,
             EvalCallback eval,
             UpdateCallback update,
             FreeBufferCallback free_buffer)
      : target(target), buffer(buffer), eval(eval), update(update), free_buffer(free_buffer)
  {
  }
};

/* Data visible to the worker thread. */
struct WorkerData {
  /* Can be set from the main thread to tell the evaluating thread to start over.
   * Is used because we cannot just stop the depsgraph evaluation. */
  std::atomic<bool> restart;
  /* The center frame around which to run the evaluation. */
  std::atomic<int> evaluation_center;
  /* The range of frames that were already evaluated. Inclusive/Inclusive. Any frames in that range
   * are safe to read in the main thread. */
  std::atomic<Bounds<int>> evaluated_range;

  TicketMutex *data_mutex;
  /* The depsgraph to evaluate in the background. All copy on eval nodes have to be
   * evaluated before it is sent to the thread. */
  Depsgraph *dg = nullptr;
  Vector<TargetData> target_data;

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

static void run_job(void *job_data, wmJobWorkerStatus *worker_status)
{
  WorkerData *eval_data = static_cast<WorkerData *>(job_data);
restart:
  eval_data->restart.store(false, std::memory_order_release);
  const int center_frame = eval_data->evaluation_center.load(std::memory_order_acquire);
  eval_data->lock();
  DEG_evaluate_on_framechange(eval_data->dg, center_frame);
  for (TargetData &target_data : eval_data->target_data) {
    const EvaluationTarget &target = target_data.target;
    void *target_buffer = target_data.buffer;
    target_data.eval(eval_data->dg, *target.id, center_frame, target_buffer);
  }
  eval_data->unlock();

  Bounds<int> evaluated_range = {center_frame, center_frame};

  bool all_done_left = true;
  bool all_done_right = true;
  while (true) {
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

    all_done_left = true;
    all_done_right = true;
    eval_data->lock();
    DEG_evaluate_on_framechange(eval_data->dg, frame);
    for (TargetData &target_data : eval_data->target_data) {
      const EvaluationTarget &target = target_data.target;
      void *target_buffer = target_data.buffer;
      const bool modified_data = target_data.eval(eval_data->dg, *target.id, frame, target_buffer);
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
    eval_data->evaluated_range.store(evaluated_range, std::memory_order_release);
    worker_status->do_update = true;
    if (eval_data->restart.load(std::memory_order_acquire)) {
      goto restart;
    }

    if (all_done_left && all_done_right) {
      break;
    }
    /* TODO remove before flight. */
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
  /* TODO merge ID list with existing IDs. */
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
  Bounds<int> evaluated_range = eval_data->evaluated_range.load(std::memory_order_acquire);
  for (TargetData &target_data : eval_data->target_data) {
    target_data.update(*target_data.target.id, target_data.buffer, evaluated_range);
  }
}

static void finish_job(void *job_data)
{
  WorkerData *eval_data = static_cast<WorkerData *>(job_data);
  for (TargetData &target_data : eval_data->target_data) {
    target_data.free_buffer(target_data.buffer);
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
                              void *target_buffer,
                              EvalCallback eval_cb,
                              UpdateCallback update_cb,
                              FreeBufferCallback free_cb)
{
  /* TODO dont rebuild DG every time. Keep data around for a while before ending worker. This will
   * mean regular updates to the same data are more efficient. */
  wmJob *wm_job = WM_jobs_get(
      &wm, &window, &scene, job_name, eWM_JobFlag(0), WM_JOB_TYPE_MOTION_PATH_EVAL);

  if (WM_jobs_is_running(wm_job)) {
    BGEvalJobData *eval_data = static_cast<BGEvalJobData *>(WM_jobs_customdata_get(wm_job));

    if (eval_data->active_targets.contains(target)) {
      eval_data->worker_data.lock();
      const int target_index = eval_data->worker_data.find_target_index(target);
      BLI_assert(target_index != -1);
      TargetData &data = eval_data->worker_data.target_data[target_index];
      data.free_buffer(data.buffer);
      data.buffer = target_buffer;
      data.finished_left = false;
      data.finished_right = false;
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
    eval_data->worker_data.target_data.append(
        {target, target_buffer, eval_cb, update_cb, free_cb});

    eval_data->worker_data.restart.store(true, std::memory_order_release);
    eval_data->worker_data.unlock();
    return;
  }

  BGEvalJobData *eval_data = MEM_new<BGEvalJobData>(__func__);
  eval_data->active_targets.add(target);
  eval_data->worker_data.target_data.append({target, target_buffer, eval_cb, update_cb, free_cb});
  eval_data->worker_data.dg = build_worker_depsgraph(
      bmain, scene, view_layer, eval_data->active_targets);
  const int center_frame = BKE_scene_frame_get(&scene);
  eval_data->worker_data.evaluation_center.store(center_frame, std::memory_order_release);
  eval_data->worker_data.evaluated_range.store({center_frame, center_frame},
                                               std::memory_order_release);

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

  /* No need to restart the job since this no data was added to the worker that would need
   * evaluation. */
  eval_data->worker_data.unlock();
}

}  // namespace animviz
