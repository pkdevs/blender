/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * What the native interface's Swift code may call outside Swift: the engine inbox that UIKit
 * feeds. These functions only queue work or copy out the latest snapshot; none waits for
 * Blender, see `release/ios/README.md`.
 */

#pragma once

#import <UIKit/UIKit.h>

#ifdef __cplusplus
extern "C" {
#endif

void WM_apple_ios_ui_setting_enqueue(const char *setting, const char *value);
void WM_apple_ios_ui_operator_enqueue(const char *op_name);
void WM_apple_ios_ui_command_enqueue(int command);

#ifdef __cplusplus
}
#endif
