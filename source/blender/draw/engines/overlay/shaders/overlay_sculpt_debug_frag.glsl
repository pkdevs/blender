/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_sculpt_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_sculpt_debug)

void main()
{
  frag_color = debug_color;
}
