/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#import <UIKit/UIKit.h>

/* Floating asset pane along the bottom of the viewport. UIKit lists, filters and searches the
 * assets Blender publishes; tapping or dragging one into the viewport goes through the engine
 * inbox, which runs Blender's own asset drag and drop. */
@interface GHOSTIOSAssets : NSObject <UIDropInteractionDelegate>
- (instancetype)initWithOverlay:(UIView *)overlay;
@property(nonatomic, readonly) UIBarButtonItem *button;
@property(nonatomic, readonly) UIView *view;
@property(nonatomic, readonly) BOOL visible;
@property(nonatomic, readonly) CGFloat height;
/* Install on the viewport's view to accept assets dragged out of the pane. */
@property(nonatomic, readonly) UIDropInteraction *dropInteraction;
/* Converts a point in the drop interaction's view to GHOST client coordinates. */
@property(nonatomic, copy) CGPoint (^clientPoint)(CGPoint point);
@property(nonatomic, copy) void (^visibilityDidChange)(void);
- (void)updateSnapshot:(NSDictionary *)snapshot;
/* Blender's interaction mode ("objectmode", "sculpt", ...). Until a type is picked, the pane
 * lists objects in Object Mode and everything in other modes. */
- (void)setBlenderMode:(NSString *)mode;
/* Places the pane along the bottom edge of rect. */
- (void)layoutInRect:(CGRect)rect;
- (void)toggle:(id)sender;
@end
