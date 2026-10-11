/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
/** \file
 * \ingroup GHOST
 *
 * Blender's engine thread on iPadOS and visionOS, for the native interface around it.
 */

#pragma once

#import <Foundation/Foundation.h>

/** Only the engine may wait for UIKit, for native resource creation/teardown and legacy
 * synchronous OS queries. These blocks must never call back into Blender. UI commands,
 * snapshots and frame presentation use asynchronous mailboxes instead. */
static inline void GHOST_ios_main_sync(dispatch_block_t block)
{
  if (NSThread.isMainThread) {
    block();
  }
  else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

#ifdef __cplusplus
extern "C" {
#endif
void GHOST_ios_engine_wake();
/** Keep Blender's loop running while the app is in the background, for a render. */
void GHOST_ios_engine_continue_work(bool continues);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#  include <functional>
namespace blender {
struct bContext;
}
/** Run `work` on the engine thread with Blender's context, also while the app is inactive. Work
 * runs between frames, so it may load and write files. */
void GHOST_ios_engine_perform(std::function<void(blender::bContext *)> work);
/** In the background iOS allows no GPU work, such as rendering a file's preview. */
bool GHOST_ios_app_in_background();
/** How long Blender's current pass has been running, 0 when it is between passes. Safe to call
 * from UIKit while the engine is held up. */
double GHOST_ios_engine_busy_seconds();
/** Shader and pipeline compilation in the current burst of work, false when there is none. */
bool GHOST_ios_shader_compilation(int *finished, int *total);
#endif
