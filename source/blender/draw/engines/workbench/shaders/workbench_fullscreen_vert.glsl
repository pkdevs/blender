/* SPDX-FileCopyrightText: 2015-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/workbench_transparent_resolve_infos.hh"

VERTEX_SHADER_CREATE_INFO(workbench_transparent_resolve)

#include "gpu_shader_fullscreen.bsl.hh"

void main()
{
  fullscreen_vertex(gl_VertexID, gl_Position);
}
