/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * What differs between the iPad and Apple Vision Pro, for the native interface they share.
 * visionOS windows are glass already and have no screen of their own; the iPad has Liquid Glass,
 * a screen and haptics.
 */

#pragma once

#import <UIKit/UIKit.h>

/** Glass for a panel or rail: Liquid Glass on iOS, the system's glass material on visionOS. */
static inline UIVisualEffect *WMIOSGlassEffect(BOOL clear, BOOL interactive)
{
#if defined(APPLE_TARGET_VISIONOS)
  (void)clear;
  (void)interactive;
  return [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThinMaterial];
#else
  UIGlassEffect *glass = [UIGlassEffect
      effectWithStyle:clear ? UIGlassEffectStyleClear : UIGlassEffectStyleRegular];
  glass.interactive = interactive;
  return glass;
#endif
}

/** Feel that something could not be done; visionOS has no haptics. */
static inline void WMIOSNotifyError()
{
#if !defined(APPLE_TARGET_VISIONOS)
  [[[[UINotificationFeedbackGenerator alloc] init] autorelease]
      notificationOccurred:UINotificationFeedbackTypeError];
#endif
}
