/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_uniform_shared.hh"

#ifdef GLSL_CPP_STUBS
#  define CURVES_SHADER
#  define DRW_HAIR_INFO

#  define POINTCLOUD_SHADER
#  define DRW_POINTCLOUD_INFO

#  define MAT_TRANSPARENT
#  define MAT_SHADER_TO_RGBA
#endif

namespace eevee {

/* Common interface. */
struct SurfSmooth {
  /* World Position. */
  [[smooth]] float3 P;
  /* World Normal. */
  [[smooth]] float3 N;
};

struct SurfFlat {
  [[flat]] uint resource_id_raw;
};

struct ShadowSmooth {
  [[smooth]] float3 position;
  [[smooth]] float3 vector;
};

struct ShadowFlat {
  [[flat]] int shadow_view_id;
};

struct CurveSmooth {
  [[smooth]] float3 tangent;
  [[smooth]] float3 binormal;
  [[smooth]] float time;
  [[smooth]] float time_width;
  [[smooth]] float radius;
  /* Smooth to be used for barycentric. */
  [[smooth]] float point_id;
};

struct CurveFlat {
  [[flat]] int strand_id;
};

struct PointCloudSmooth {
  [[smooth]] float radius;
  [[smooth]] float3 position;
};

struct PointCloudFlat {
  [[flat]] int id;
};

struct ClipSmooth {
  [[smooth]] float clip_distance;
};

}  // namespace eevee

/* WORKAROUND: Until we get condition support for interfaces. */
struct eevee_shadow_iface_info {
  [[legacy_iface]] static eevee::ShadowSmooth shadow_clip;
  [[legacy_iface]] static eevee::ShadowFlat shadow_iface;
};

/* WORKAROUND: Until we get condition support for interfaces. */
struct eevee_geom_curves_iface_info {
  [[legacy_iface]] static eevee::CurveSmooth curve_interp;
  [[legacy_iface]] static eevee::CurveFlat curve_interp_flat;
};

/* WORKAROUND: Until we get condition support for interfaces. */
struct eevee_geom_pointcloud_iface_info {
  [[legacy_iface]] static eevee::PointCloudSmooth pointcloud_interp;
  [[legacy_iface]] static eevee::PointCloudFlat pointcloud_interp_flat;
};

/* WORKAROUND: Until we remove global accesses to the interface. */
struct eevee_geom_iface_info {
  [[legacy_iface]] static eevee::SurfSmooth interp;
  [[legacy_iface]] static eevee::SurfFlat interp_flat;
};

/* WORKAROUND: Until we remove global accesses to the interface. */
struct eevee_clip_plane {
  [[legacy_iface]] static eevee::ClipSmooth clip_interp;
  [[uniform(CLIP_PLANE_BUF)]] static const ClipPlaneData &clip_plane;
};
