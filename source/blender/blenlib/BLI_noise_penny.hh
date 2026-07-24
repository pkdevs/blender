/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Penny Noise: CPU implementation of the procedural noise family used by the
 * Penny Noise texture node (Geometry Nodes) and the Penny F-Curve modifier.
 * The Cycles (SVM, OSL) and EEVEE (GLSL) backends reimplement the same math in
 * device code and stay bit-identical to this one.
 */

#pragma once

#include "BLI_math_vector_types.hh"

namespace blender::noise::penny {

/* Noise algorithm selector. Values must match the RNA enum (rna_nodetree.cc),
 * the SVM dispatch (intern/cycles/kernel/svm/penny.h) and the penny() switch. */
enum class PennyType : int {
  fBm = 0,
  Turbulence = 1,
  Stupl = 2,
  Luka = 3,
  Blistered = 4,
  Box = 5,
  Mod = 6,
  Nutous = 7,
  Cranal = 8,
  Buya = 9,
  Cell = 10,
  DisplacedTurbulence = 11,
  Hama = 12,
  Naki = 13,
  Poxo = 14,
  Pezo = 15,
  Dents = 16,
  Sema = 17,
  VL = 18,
};

/* Parameters for a single penny() evaluation. Scale is applied to the position
 * by the caller, not here. */
struct PennyParams {
  /* Noise algorithm; cast to PennyType. */
  int type = 0;
  /* 0 = Normal; 1..24 blend a second decorrelated sample (Photoshop-style:
   * 1=Add, 2=Multiply, ... 24=Average). */
  int blend_mode = 0;
  /* Noise octaves. Dents and Poxo use the fractional part for smooth LOD. */
  float detail = 6.0f;
  /* Per-octave amplitude falloff (fBm gain), [0, 1]. */
  float roughness = 0.5f;
  /* Output Levels remap (min, mid, max); (0, 0.5, 1) is identity. */
  float3 levels = float3(0.0f, 0.5f, 1.0f);
  /* Per-axis integer tiling period; values <= 0 clamp to 1. */
  int4 period = int4(0, 0, 0, 0);
};

/* Evaluate Penny Noise at a 4D position (XYZ + W). Returns the three noise
 * channels; the node's Factor output is the .x channel. */
float3 penny(const float4 &position, const PennyParams &params);

}  // namespace blender::noise::penny
