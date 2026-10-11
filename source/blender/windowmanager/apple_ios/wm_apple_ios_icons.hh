/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>

/* Blender interface icon by its icon name (e.g. "MOD_SUBSURF"), matching how Blender colors it.
 * Multicolor icons keep their theme colors everywhere. Object, modifier and other category icons
 * use their theme color only where `themed` (Blender's Outliner and Properties tabs); everywhere
 * else they follow the tint like any other icon. Colored assets carry light and dark variants
 * taken from Blender's light and dark themes. */
static inline UIImage *WMIOSBlenderIcon(NSString *name, BOOL themed)
{
  if (![name isKindOfClass:NSString.class] || name.length == 0) {
    return nil;
  }
  NSString *key = name.lowercaseString;
  if (UIImage *multicolor = [UIImage
          imageNamed:[@"blender.multicolor." stringByAppendingString:key]])
  {
    return [multicolor imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
  }
  if (themed) {
    if (UIImage *tinted = [UIImage imageNamed:[@"blender.themed." stringByAppendingString:key]]) {
      return [tinted imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
    }
  }
  return [[UIImage imageNamed:[@"blender." stringByAppendingString:key]]
      imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
}
