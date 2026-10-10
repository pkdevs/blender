/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>

@protocol GHOSTIOSPencilMenu;

@protocol GHOSTIOSPencilMenuDelegate <NSObject>
- (void)pencilMenu:(UIView<GHOSTIOSPencilMenu> *)menu
    requestItemsForRoute:(NSDictionary *)route
                atPoint:(CGPoint)point
             completion:(void (^)(NSArray<NSDictionary *> *))completion;
- (void)pencilMenu:(UIView<GHOSTIOSPencilMenu> *)menu
    didChooseItem:(NSDictionary *)item
          atPoint:(CGPoint)point;
@end

/**
 * Native radial actions, a view drawn in SwiftUI (`intern/swift/PencilMenu.swift`) and created by
 * name. All points are UIKit points in this view, never drawable pixels.
 */
@protocol GHOSTIOSPencilMenu <NSObject>
@property(nonatomic, assign) id<GHOSTIOSPencilMenuDelegate> delegate;
@property(nonatomic, readonly) BOOL active;

/* Location is in this view's coordinates; nil uses the last known Pencil position. */
- (void)toggleWithPencilLocation:(NSValue *)location;
- (BOOL)handlePencilTouch:(UITouch *)touch;
- (BOOL)handlePencilHover:(UIHoverGestureRecognizer *)hover;
- (void)cancel;
@end
