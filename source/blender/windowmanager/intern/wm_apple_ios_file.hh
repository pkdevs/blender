/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "WM_types.hh"

struct bContext;
struct wmOperator;

bool WM_apple_ios_fileselect_begin(bContext *C, wmOperator *op);
bool WM_apple_ios_fileselect_active(wmOperator *op);
void WM_apple_ios_fileselect_cancel(wmOperator *op);
void WM_apple_ios_fileselect_flush(bContext *C);
wmOperatorStatus WM_apple_ios_fileselect_exec(bContext *C, wmOperator *op);
