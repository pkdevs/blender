/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>

/**
 * The last operation's controls, drawn in SwiftUI (`intern/swift/RedoStrip.swift`) and created by
 * name.
 */
@protocol GHOSTIOSRedoStrip <NSObject>
- (instancetype)initWithOverlay:(UIView *)overlay icon:(UIImage * (^)(NSString *))icon;
- (void)updateSnapshot:(NSDictionary *)snapshot;
/* Centers the expanded strip on centerX while keeping it inside rect. */
- (void)layoutInRect:(CGRect)rect centerX:(CGFloat)centerX;
@property(nonatomic, readonly) UIButton *collapsedButton;
@property(nonatomic, readonly) CGFloat collapsedWidth;
/* The user opened the current operation's controls. */
@property(nonatomic, readonly, getter=isExpanded) BOOL expanded;
/* Folds the open controls back into the pill. */
- (void)collapse;
@property(nonatomic, copy) void (^presentationDidChange)(BOOL animated);
@end
