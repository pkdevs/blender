/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>

/**
 * The inspector and hierarchy panels, drawn in SwiftUI (`intern/swift/ScenePanels.swift`) and
 * created by name, as this library is built without Swift. UIKit owns only immutable snapshots.
 * All edits go through the engine inbox.
 */
@protocol GHOSTIOSScenePanels <NSObject>
- (instancetype)initWithOverlay:(UIView *)overlay icon:(UIImage * (^)(NSString *))icon;
@property(nonatomic, readonly) UIBarButtonItem *inspectorButton;
@property(nonatomic, readonly) UIBarButtonItem *hierarchyButton;
@property(nonatomic, copy) UIMenu * (^menuProvider)(NSDictionary *);
- (void)updateSnapshot:(NSDictionary *)snapshot;
- (void)layoutInRect:(CGRect)rect;
- (void)dismiss;
- (void)presentShadingFrom:(UIBarButtonItem *)button controller:(UIViewController *)controller;
- (UIBarButtonItemGroup *)updateToolSettings:(NSDictionary *)settings
                             acknowledgement:(NSNumber *)ack;
- (void)hideToolSettings;
@end
