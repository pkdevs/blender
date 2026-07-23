/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_bounds_types.hh"
#include "BLI_function_ref.hh"

namespace blender {

struct BlendDataReader;
struct BlendWriter;
struct Depsgraph;
struct Object;
struct ReportList;
struct Scene;
struct bAnimVizSettings;
struct bMotionPath;
struct bPoseChannel;
struct wmWindow;
struct wmWindowManager;
struct Main;
struct ViewLayer;

/* ---------------------------------------------------- */
/* Animation Visualization */

namespace bke::animviz {

/**
 * Uniquely identifies an entity to evaluate over a frame range.
 */
struct EvaluationTarget {
  ID *id;
  std::string component_name;

  uint64_t hash() const
  {
    return get_default_hash(id, component_name);
  }

  bool operator==(const EvaluationTarget &other) const
  {
    return other.id == id && other.component_name == component_name;
  }
};

/**
 * Called for every frame that the worker evaluated.
 *
 * \return True if the evaluation result is different to buffered data. Returning false signals to
 * the thread that for this frame, the evaluation was not required. This is used on the worker
 * thread to dynamically figure out where the evaluation should end.
 */
using EvalCallback = FunctionRef<bool(Depsgraph *dg, const EvaluationTarget &target, int frame)>;
/* Callback that runs on the main thread periodically. Can be used to copy back data from the
 * buffer. The given range is guaranteed to be evaluated and can be read in a thread safe way. */
using UpdateCallback =
    FunctionRef<void(const EvaluationTarget &target, Bounds<int> evaluated_range)>;
/* Called when the given target is done with the evaluation. Use to free any heap allocated data
 * passed into the system. */
using FinishCallback = FunctionRef<void(const EvaluationTarget &target)>;

/**
 *
 * \param eval_cb Will be called from the worker thread
 * with the evaluated depsgraph for every frame. Don't touch main from this function.
 * \param update_cb Will be called periodically on the main thread. This can be used to copy data
 * from the buffer to a storage in the main thread.
 * \param finish_cb Always called just after the target was removed from the evaluation system. Use
 * to do any last syncronisation and freeing any allocated buffers.
 */
void background_eval_register(Main &bmain,
                              wmWindowManager &wm,
                              wmWindow &window,
                              Scene &scene,
                              ViewLayer &view_layer,
                              const EvaluationTarget &target,
                              EvalCallback eval_cb,
                              UpdateCallback update_cb,
                              FinishCallback finish_cb);

/**
 * Manually deregister a target from background evaluation. This is a no-op if the target is not
 * registered. Needs to be called in case the target is deleted mid evaluation.
 */
void background_eval_deregister(wmWindowManager &wm,
                                wmWindow &window,
                                Scene &scene,
                                const EvaluationTarget &target);

}  // namespace bke::animviz

/**
 * Initialize the default settings for animation visualization.
 */
void animviz_settings_init(struct bAnimVizSettings *avs);

namespace bke::motionpath {

/* Data owned by the thread which calculates the motion path. */
struct WorkerBuffer {
  int start_frame;
  Array<float3> points;
  Array<eMotionPathVert_Flag> flags;
};

struct Runtime;

void async_eval_allocate(bMotionPath &motionpath,
                         wmWindowManager &wm,
                         wmWindow &window,
                         Scene &job_owner,
                         bke::animviz::EvaluationTarget &target);
void async_eval_free(bMotionPath &motion_path);
WorkerBuffer *get_worker_buffer(bMotionPath &motion_path);

}  // namespace bke::motionpath

/**
 * Make a copy of motion-path data, so that viewing with copy on write works.
 */
struct bMotionPath *animviz_copy_motionpath(const struct bMotionPath *mpath_src);

/**
 * Free the given motion path's cache.
 */
void animviz_free_motionpath_cache(struct bMotionPath *mpath);
/**
 * Free the given motion path instance and its data.
 * \note this frees the motion path given!
 */
void animviz_free_motionpath(struct bMotionPath *mpath);

/**
 * Setup motion paths for the given data.
 * \note Only used when explicitly calculating paths on bones which may/may not be consider already
 *
 * \param scene: Current scene (for frame ranges, etc.)
 * \param ob: Object to add paths for (must be provided)
 * \param pchan: Pose-channel to add paths for
 * (optional; if not provided, object-paths are assumed).
 */
struct bMotionPath *animviz_verify_motionpaths(struct ReportList *reports,
                                               struct Scene *scene,
                                               struct Object *ob,
                                               struct bPoseChannel *pchan);

void animviz_motionpath_blend_write(struct BlendWriter *writer, struct bMotionPath *mpath);
void animviz_motionpath_blend_read_data(struct BlendDataReader *reader, struct bMotionPath *mpath);

}  // namespace blender
