/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#import <UIKit/UIKit.h>

/**
 * A Blender file as an iOS document. Blender reads and writes the file itself; the document
 * coordinates access to it and, as in Pages, saves changes without being asked.
 */
@interface WMIOSBlendDocument : UIDocument
@property(nonatomic) BOOL templateDocument;
/** Follows Blender's unsaved changes, which drive saving. */
- (void)setBlenderModified:(BOOL)modified;
/**
 * Save now, if there are changes. Documents save only when closed and when the app leaves the
 * foreground, never while being edited: writing the file and its preview would interrupt work.
 */
- (void)saveChangesWithCompletionHandler:(void (^)(BOOL success))completionHandler;
@end

/**
 * Copy a validated document seed ("" aliases General) to a temporary location,
 * for the document browser to add as a new document. Calls `completion` on the main thread, with
 * nil on failure.
 */
void wm_apple_ios_document_create(NSString *app_template, void (^completion)(NSURL *url));

/** Bundled, versioned templates with resolved seed and preview URLs. No Blender calls. */
NSArray<NSDictionary *> *wm_apple_ios_document_templates();
void wm_apple_ios_template_save(NSString *title,
                                NSDictionary *presentation,
                                void (^completion)(NSString *error));
/** Personal templates only, except Duplicate which also accepts bundled templates. */
BOOL wm_apple_ios_template_manage(NSString *identifier, NSString *action, NSString *title);
