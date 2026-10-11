/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Blender's window for the SwiftUI document window, `swift/DocumentWindow.swift`: GHOST's window
 * functions with C names, which Swift can call.
 */

#import "wm_apple_ios_view_controller.hh"

static GHOST_WindowIOS *ghost_window(void *window)
{
  return static_cast<GHOST_WindowIOS *>(window);
}

extern "C" bool wm_apple_ios_window_presented_frame(void *window)
{
  return window && GHOST_ios_window_presented_frame(ghost_window(window));
}

extern "C" CGPoint wm_apple_ios_window_pixels(void *window, CGPoint point)
{
  return window ? GHOST_ios_window_pixels(ghost_window(window), point) : point;
}

extern "C" void wm_apple_ios_window_send_key(void *window, const char *key)
{
  if (!window || !key) {
    return;
  }
  const GHOST_TKey ghost_key = STREQ(key, "X") ? GHOST_kKeyX :
                               STREQ(key, "Y") ? GHOST_kKeyY :
                               STREQ(key, "Z") ? GHOST_kKeyZ :
                                                 GHOST_kKeyUnknown;
  if (ghost_key != GHOST_kKeyUnknown) {
    GHOST_ios_window_send_key(ghost_window(window), ghost_key);
  }
}

extern "C" bool wm_apple_ios_documents_available()
{
  return wm_apple_ios_launcher_can_show_documents();
}

extern "C" void wm_apple_ios_show_documents(id sender)
{
  wm_apple_ios_launcher_show_documents(sender);
}
