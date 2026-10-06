/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#import <UIKit/UIKit.h>

/* Floating playback strip. UIKit mirrors the scene frame; edits go through the engine inbox. */
@interface GHOSTIOSTimeline : NSObject
- (instancetype)initWithOverlay:(UIView *)overlay;
@property(nonatomic, readonly) UIBarButtonItem *button;
@property(nonatomic, readonly) UIView *view;
@property(nonatomic, readonly) BOOL visible;
@property(nonatomic, readonly) CGFloat height;
@property(nonatomic, copy) void (^visibilityDidChange)(void);
- (void)updateSnapshot:(NSDictionary *)snapshot;
/* Places the strip along the bottom edge of rect. */
- (void)layoutInRect:(CGRect)rect;
- (void)toggle:(id)sender;
@end
