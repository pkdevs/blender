/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#import <UIKit/UIKit.h>

/**
 * Accepts files dragged in from other apps (Files, Photos, Safari, ...) and hands them to
 * Blender as a file drop at the point they land, as dropping files on the window does on a Mac.
 * Blender's drop boxes then decide what happens: image empties, importers, opening a .blend, etc.
 *
 * Dragged items are only readable while the drop is delivered, so each is copied into the app's
 * own storage first, where paths written into a .blend keep working. Drags from within the app
 * (such as assets) are left to their own drop interactions.
 */
@interface GHOSTIOSFileDrop : NSObject <UIDropInteractionDelegate>
@property(nonatomic, readonly) UIDropInteraction *interaction;
/** Receives the copied files' paths and the drop point in the interaction view's coordinates.
 * Called on the main thread. */
@property(nonatomic, copy) void (^dropFiles)(NSArray<NSString *> *paths, CGPoint point);
@end
