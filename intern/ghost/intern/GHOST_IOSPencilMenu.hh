/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>

@class GHOSTIOSPencilMenu;

@protocol GHOSTIOSPencilMenuDelegate <NSObject>
- (void)pencilMenu:(GHOSTIOSPencilMenu *)menu
    requestItemsForRoute:(NSDictionary *)route
                atPoint:(CGPoint)point
             completion:(void (^)(NSArray<NSDictionary *> *))completion;
- (void)pencilMenu:(GHOSTIOSPencilMenu *)menu
    didChooseItem:(NSDictionary *)item
          atPoint:(CGPoint)point;
@end

/** Native radial actions. All points are UIKit points in this view, never drawable pixels. */
@interface GHOSTIOSPencilMenu : UIView
@property(nonatomic, assign) id<GHOSTIOSPencilMenuDelegate> delegate;
@property(nonatomic, readonly) BOOL active;
@property(nonatomic, readonly) BOOL menuVisible;

/* Location is in this view's coordinates; nil uses the last known Pencil position. */
- (void)toggleWithPencilLocation:(NSValue *)location;
- (BOOL)handlePencilTouch:(UITouch *)touch;
- (BOOL)handlePencilHover:(UIHoverGestureRecognizer *)hover;
- (void)cancel;
@end
