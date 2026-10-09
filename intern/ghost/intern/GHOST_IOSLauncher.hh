/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#import <UIKit/UIKit.h>

/**
 * The document launcher, for the app's first window. It shows at once on launch, styled with
 * Blender's brand, logo and a large Suzanne to flick around, while Blender starts behind it. It
 * owns the open document; Blender's window shows only while one is open.
 */
UIViewController *GHOST_ios_launcher_controller();

/** The open document, or nil in the launcher. */
UIDocument *GHOST_ios_launcher_document();
/** Whether a document is open that the system's back action can return from. */
bool GHOST_ios_launcher_can_show_documents();
/** Close the document, which saves it, and return to the launcher. */
void GHOST_ios_launcher_show_documents(id sender);
/** Follow Blender's unsaved changes, which make the document save itself. */
void GHOST_ios_launcher_set_modified(bool modified);

/**
 * Show Blender's window over the launcher, or hide it, fading or at once. Implemented in
 * `GHOST_WindowIOS.mm`.
 */
void GHOST_ios_document_window_set_visible(bool visible, bool animated);
/** A picture of Blender's window as it is on screen, or nil. */
UIView *GHOST_ios_document_window_snapshot();
bool GHOST_ios_document_window_visible();
