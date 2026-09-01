/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_gsplat_lib.bsl.hh"
#include "eevee_geom_types_lib.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"

/* -------------------------------------------------------------------- */
/** \name Gaussian Splat
 *
 * Gaussian Splat objects load attributes from buffers through sampler buffers.
 * \{ */

float3 attr_load_orco(GSplatPoint splat, float4 /*orco*/, int /*index*/)
{
  /* NOTE: Doesn't support ORCO attribute. */
  return splat.orco_default;
}
float4 attr_load_tangent(GSplatPoint splat, samplerBuffer cd_buf, int /*index*/)
{
  return draw::gsplat::get_customdata_vec4(splat.splat_id, cd_buf);
}
float3 attr_load_uv(GSplatPoint splat, samplerBuffer cd_buf, int /*index*/)
{
  return draw::gsplat::get_customdata_vec3(splat.splat_id, cd_buf);
}
float4 attr_load_color(GSplatPoint splat, samplerBuffer cd_buf, int /*index*/)
{
  return draw::gsplat::get_customdata_vec4(splat.splat_id, cd_buf);
}
float4 attr_load_float4(GSplatPoint splat, samplerBuffer cd_buf, int /*index*/)
{
  return draw::gsplat::get_customdata_vec4(splat.splat_id, cd_buf);
}
float3 attr_load_float3(GSplatPoint splat, samplerBuffer cd_buf, int /*index*/)
{
  return draw::gsplat::get_customdata_vec3(splat.splat_id, cd_buf);
}
float2 attr_load_float2(GSplatPoint splat, samplerBuffer cd_buf, int /*index*/)
{
  return draw::gsplat::get_customdata_vec2(splat.splat_id, cd_buf);
}
float attr_load_float(GSplatPoint splat, samplerBuffer cd_buf, int /*index*/)
{
  return draw::gsplat::get_customdata_float(splat.splat_id, cd_buf);
}

/** \} */
