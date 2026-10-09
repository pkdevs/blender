/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>

/**
 * Renders the image or animation with a native sheet instead of Blender's render window: live
 * progress, then the result with the system share sheet. Animations become a video.
 *
 * Blender renders through `bl_ui.native_render`; its state arrives in the toolbar snapshot.
 */
@interface GHOSTIOSRender : NSObject
- (instancetype)initWithPresenter:(UIViewController *)presenter;
- (void)renderAnimation:(BOOL)animation;
/** The `render` entry of the toolbar snapshot. */
- (void)updateStatus:(NSDictionary *)status;
@end
