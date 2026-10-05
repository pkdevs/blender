/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <functional>

struct GHOST_IOSDialog;

namespace blender {
struct bContext;
struct wmWindow;
struct ReportList;

/* Engine-thread API. UIKit exchanges only owned values and request identifiers. */
bool WM_apple_ios_dialog_begin(bContext *C,
                               const GHOST_IOSDialog &dialog,
                               std::function<void(bContext *, int, uint64_t)> complete,
                               std::function<void(bContext *)> cancel = {},
                               const char *key = "");
void WM_apple_ios_dialog_flush(bContext *C);
bool WM_apple_ios_dialog_active(const wmWindow *win);
void WM_apple_ios_reports(wmWindow *win, ReportList *reports, int minimum_level);

}  // namespace blender
