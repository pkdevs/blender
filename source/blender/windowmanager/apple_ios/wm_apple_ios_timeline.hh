/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>

/**
 * Floating playback strip, drawn in SwiftUI
 * (`source/blender/windowmanager/apple_ios/swift/Timeline.swift`) and created by name. It mirrors
 * the scene frame; edits go through the engine inbox.
 */
@protocol WMIOSTimeline <NSObject>
- (instancetype)initWithOverlay:(UIView *)overlay;
@property(nonatomic, readonly) UIBarButtonItem *button;
@property(nonatomic, readonly) BOOL visible;
@property(nonatomic, readonly) CGFloat height;
/* Called when the strip shows, hides, or changes height. */
@property(nonatomic, copy) void (^visibilityDidChange)(void);
- (void)updateSnapshot:(NSDictionary *)snapshot;
/* Places the strip along the bottom edge of rect. */
- (void)layoutInRect:(CGRect)rect;
- (void)toggle:(id)sender;
@end
