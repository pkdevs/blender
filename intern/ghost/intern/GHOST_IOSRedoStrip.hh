/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#import <UIKit/UIKit.h>

@interface GHOSTIOSRedoStrip : NSObject
- (instancetype)initWithOverlay:(UIView *)overlay icon:(UIImage *(^)(NSString *))icon;
- (void)updateSnapshot:(NSDictionary *)snapshot;
- (void)layoutInRect:(CGRect)rect;
/* Centers the expanded strip on centerX while keeping it inside rect. */
- (void)layoutInRect:(CGRect)rect centerX:(CGFloat)centerX;
@property(nonatomic, readonly) UIView *view;
@property(nonatomic, readonly) UIButton *collapsedButton;
@property(nonatomic, readonly) CGFloat collapsedWidth;
/* The user opened the current operation's controls. */
@property(nonatomic, readonly, getter=isExpanded) BOOL expanded;
/* Folds the open controls back into the pill. */
- (void)collapse;
@property(nonatomic, copy) void (^presentationDidChange)(BOOL animated);
@end
