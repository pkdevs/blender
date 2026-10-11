/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * What differs between the iPad and Apple Vision Pro for the window: visionOS windows have no
 * screen of their own.
 */

#pragma once

#import <UIKit/UIKit.h>

/** The display in points: the screen on iOS, a window's default size on visionOS. */
static inline CGRect GHOSTIOSDisplayBounds()
{
#if defined(APPLE_TARGET_VISIONOS)
  return CGRectMake(0.0, 0.0, 1280.0, 720.0);
#else
  return UIScreen.mainScreen.bounds;
#endif
}

/** Pixels per point of the display, or of `view` once it is in a window. */
static inline CGFloat GHOSTIOSDisplayScale(UIView *view)
{
  if (view.window) {
    return view.traitCollection.displayScale > 0.0 ? view.traitCollection.displayScale : 2.0;
  }
#if defined(APPLE_TARGET_VISIONOS)
  return 2.0;
#else
  return UIScreen.mainScreen.nativeScale > 0.0 ? UIScreen.mainScreen.nativeScale :
                                                 UIScreen.mainScreen.scale;
#endif
}

static inline NSInteger GHOSTIOSMaximumFramesPerSecond()
{
#if defined(APPLE_TARGET_VISIONOS)
  return 90;
#else
  return UIScreen.mainScreen.maximumFramesPerSecond;
#endif
}
