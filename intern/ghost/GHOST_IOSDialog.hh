/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class GHOST_IOSDialogRole { Default, Cancel, Destructive };

struct GHOST_IOSDialogAction {
  std::string title;
  GHOST_IOSDialogRole role = GHOST_IOSDialogRole::Default;
};

struct GHOST_IOSDialogToggle {
  std::string title;
  bool selected;
};

struct GHOST_IOSDialog {
  std::string title;
  std::string message;
  std::vector<GHOST_IOSDialogAction> actions;
  std::vector<GHOST_IOSDialogToggle> toggles;
  int preferred_action = -1;
};

using GHOST_IOSDialogCompletion = void (*)(uint64_t request, int action, uint64_t toggles);

/* Copies all values before returning. Completion never touches Blender state. */
bool GHOST_ios_dialog_begin(void *window,
                            uint64_t request,
                            const GHOST_IOSDialog &dialog,
                            GHOST_IOSDialogCompletion completion);
void GHOST_ios_dialog_cancel(uint64_t request);
