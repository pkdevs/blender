/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class wmIOSDialogRole { Default, Cancel, Destructive };

struct wmIOSDialogAction {
  std::string title;
  wmIOSDialogRole role = wmIOSDialogRole::Default;
};

struct wmIOSDialogToggle {
  std::string title;
  bool selected;
};

struct wmIOSDialog {
  std::string title;
  std::string message;
  std::vector<wmIOSDialogAction> actions;
  std::vector<wmIOSDialogToggle> toggles;
  int preferred_action = -1;
};

using wmIOSDialogCompletion = void (*)(uint64_t request, int action, uint64_t toggles);

/* Copies all values before returning. Completion never touches Blender state. */
bool wm_apple_ios_dialog_begin(void *window,
                               uint64_t request,
                               const wmIOSDialog &dialog,
                               wmIOSDialogCompletion completion);
void wm_apple_ios_dialog_cancel(uint64_t request);
