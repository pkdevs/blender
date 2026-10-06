/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#import <UIKit/UIKit.h>

@interface GHOSTIOSRedoStrip : NSObject
- (instancetype)initWithOverlay:(UIView *)overlay icon:(UIImage *(^)(NSString *))icon;
- (void)updateSnapshot:(NSDictionary *)snapshot;
- (void)layoutInRect:(CGRect)rect;
@property(nonatomic, readonly) UIView *view;
@end
