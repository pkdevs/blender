/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#include "WM_native_interface.hh"

namespace blender {

static const wmNativeInterface *native_interface = nullptr;

const wmNativeInterface *WM_native_interface()
{
  return native_interface;
}

void WM_native_interface_set(const wmNativeInterface *native)
{
  native_interface = native;
}

}  // namespace blender
