/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>

/**
 * Renders the image or animation with a native sheet instead of Blender's render window: live
 * progress, then the result with the system share sheet. Animations become a video. Drawn in
 * SwiftUI (`intern/swift/RenderSheet.swift`) and created by name.
 *
 * Blender renders through `bl_ui.native_render`; its state arrives in the toolbar snapshot.
 */
@protocol GHOSTIOSRender <NSObject>
- (instancetype)initWithPresenter:(UIViewController *)presenter;
- (void)renderAnimation:(BOOL)animation;
/** Every earlier render, in the render sheet's film strip. */
- (void)showRenders;
/** The `render` entry of the toolbar snapshot. */
- (void)updateStatus:(NSDictionary *)status;
@end
