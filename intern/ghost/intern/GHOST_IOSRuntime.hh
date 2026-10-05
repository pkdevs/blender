/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#import <Foundation/Foundation.h>

/** Only the engine may wait for UIKit, for native resource creation/teardown and legacy
 * synchronous OS queries. These blocks must never call back into Blender. UI commands,
 * snapshots and frame presentation use asynchronous mailboxes instead. */
inline void GHOST_ios_main_sync(dispatch_block_t block)
{
  if (NSThread.isMainThread) {
    block();
  }
  else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

void GHOST_ios_engine_wake();
