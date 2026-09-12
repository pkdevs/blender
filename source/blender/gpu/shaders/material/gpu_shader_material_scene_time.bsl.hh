/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_material_interface.bsl.hh"

[[node]]
void node_scene_time(float &seconds, float &frame, [[resource_table]] KernelGlobals &kg)
{
  scene_time_uniforms(kg, seconds, frame);
}
