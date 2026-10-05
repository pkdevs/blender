/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>

/* Install on UIKit's thread before creating controls. System blue adapts to appearance and contrast. */
static inline void blenderIOSApplyTheme(UIWindow *window)
{
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    UISwitch.appearance.onTintColor = UIColor.systemBlueColor;
    UISegmentedControl.appearance.selectedSegmentTintColor = UIColor.systemBlueColor;
    [UISegmentedControl.appearance setTitleTextAttributes:
        @{NSForegroundColorAttributeName: UIColor.whiteColor} forState:UIControlStateSelected];
  });
  window.tintColor = UIColor.systemBlueColor;
}
