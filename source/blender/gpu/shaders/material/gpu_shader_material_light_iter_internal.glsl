/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef GPU_SHADER

/**
 * Dummy functions for gpu_shader_dependency.
 * Functions need parameters to be reflected, but we don't really rely on the reflection data.
 */
[[node]]
void LIGHT_ITER_BEGIN(float dummy) {};
[[node]]
void LIGHT_ITER_END(float dummy) {};

#endif
