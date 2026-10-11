/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>

/**
 * A small glass pill under the top bar showing shader compilation progress while it holds up the
 * viewport. UIKit runs apart from Blender's thread, so it stays live while Blender waits.
 */
@interface WMIOSCompileIndicator : NSObject
/** Shown centered in `overlay`, just below `anchor` (the top bar). */
- (instancetype)initWithOverlay:(UIView *)overlay below:(UIView *)anchor;
/** Stop polling; the indicator stays hidden. */
- (void)stop;
@end
