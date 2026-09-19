/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Based on Frosbite Unified Volumetric.
 * https://www.ea.com/frostbite/news/physically-based-unified-volumetric-rendering-in-frostbite */

/* Store volumetric properties into the froxel textures. */

#pragma once

#include "draw_object_infos_infos.hh"

#ifdef GLSL_CPP_STUBS
#  define MAT_VOLUME
#endif

#include "eevee_volume_lib.bsl.hh"

/* Needed includes for shader nodes. */
#include "eevee_attributes_volume_lib.glsl"
#include "eevee_nodetree_frag_lib.bsl.hh"
#include "eevee_occupancy_lib.bsl.hh"
#include "eevee_sampling_lib.bsl.hh"
#include "eevee_surf_common.bsl.hh"

namespace eevee {

struct VolumeProperties {
  float3 scattering;
  float3 absorption;
  float3 emission;
  float anisotropy;
};

struct SurfVolume {
  [[compilation_constant]] bool is_homogenous;
  [[compilation_constant]] bool is_volume_object;
  [[compilation_constant]] bool is_world;

  [[legacy_info]] ShaderCreateInfo draw_modelmat_common;

  [[image(VOLUME_OCCUPANCY_SLOT, read, UINT_32)]] uimage3DAtomic occupancy_img;

  [[image(
      VOLUME_PROP_SCATTERING_IMG_SLOT, read_write, UFLOAT_11_11_10)]] image3D out_scattering_img;
  [[image(
      VOLUME_PROP_EXTINCTION_IMG_SLOT, read_write, UFLOAT_11_11_10)]] image3D out_extinction_img;
  [[image(VOLUME_PROP_EMISSION_IMG_SLOT, read_write, UFLOAT_11_11_10)]] image3D out_emissive_img;
  [[image(VOLUME_PROP_PHASE_IMG_SLOT, read_write, SFLOAT_16)]] image3D out_phase_img;
  [[image(VOLUME_PROP_PHASE_WEIGHT_IMG_SLOT, read_write, SFLOAT_16)]] image3D out_phase_weight_img;

  void write_froxel(int3 froxel, VolumeProperties prop)
  {
    float2 phase = float2(prop.anisotropy, 1.0f);

    /* Do not add phase weight if there's no scattering. */
    if (all(equal(prop.scattering, float3(0.0f)))) {
      phase = float2(0.0f);
    }

    float3 extinction = prop.scattering + prop.absorption;

    if (!is_world) [[static_branch]] {
      /* Additive Blending. No race condition since we have a barrier between each conflicting
       * invocations. */
      prop.scattering += imageLoadFast(out_scattering_img, froxel).rgb;
      prop.emission += imageLoadFast(out_emissive_img, froxel).rgb;
      extinction += imageLoadFast(out_extinction_img, froxel).rgb;
      phase.x += imageLoadFast(out_phase_img, froxel).r;
      phase.y += imageLoadFast(out_phase_weight_img, froxel).r;
    }

    imageStoreFast(out_scattering_img, froxel, prop.scattering.xyzz);
    imageStoreFast(out_extinction_img, froxel, extinction.xyzz);
    imageStoreFast(out_emissive_img, froxel, prop.emission.xyzz);
    imageStoreFast(out_phase_img, froxel, phase.xxxx);
    imageStoreFast(out_phase_weight_img, froxel, phase.yyyy);
  }

  VolumeProperties eval_froxel([[resource_table]] KernelGlobals &kg,
                               ShadingData sd,
                               [[resource_table]] const Uniform &uni,
                               const ViewMatrices view,
                               const ObjectMatrices obj,
                               const ObjectInfos ob_infos,
                               int3 froxel,
                               float jitter)
  {
    float3 uvw = (float3(froxel) + float3(0.5f, 0.5f, 0.5f - jitter)) *
                 uni.uniform_buf.volumes.inv_tex_size;

    float3 vP = volume_jitter_to_view(uni, view, uvw);
    float3 wP = view.point_view_to_world(vP);
    float3 lP = obj.point_world_to_object(wP);
    /* Compute Original Coordinate (ORCO). */
    float3 lP_orco = lP * ob_infos.orco_mul + ob_infos.orco_add;

    sd.P = wP;
    sd.ray_length = distance(sd.P, view.position());

    attrib_load(VolumePoint{lP, lP_orco});

    nodetree_volume(kg, sd);

    if (is_volume_object) [[static_branch]] {
      const auto &drw_volume = buffer_get(draw_volume_infos, drw_volume);
      sd.volume_scattering *= drw_volume.density_scale;
      sd.volume_absorption *= drw_volume.density_scale;
      sd.emission *= drw_volume.density_scale;
    }

    VolumeProperties prop;
    prop.scattering = sd.volume_scattering;
    prop.absorption = sd.volume_absorption;
    prop.emission = sd.emission;
    prop.anisotropy = sd.volume_anisotropy;
    return prop;
  }
};

/* Note: Only the front fragments have to be invoked. */
[[fragment]] [[early_fragment_tests]] [[texture_atomic]]
void surf_volume(
    [[resource_table]] KernelGlobals &kg,
    [[resource_table]] PipelineConstants &pipe,
    [[resource_table]] SurfVolume &srt,
    [[resource_table]] const Uniform &uni,
    [[resource_table]] const draw::Model &models,
    [[resource_table]] const draw::View &views,
    [[resource_table]] const draw::Infos &infos,
    [[resource_table]] const Sampling &sampling,
    [[resource_table]] const UtilityTexture & /*util_tx*/,
    [[in]] const VertOutCommon &interp,
    [[in]] const VertOutResID &interp_flat,
    [[in]] [[condition(is_curves)]] const VertOutCurves &curves_interp,
    [[in]] [[condition(is_curves)]] const VertOutCurvesFlat &curves_interp_flat,
    [[in]] [[condition(is_pointcloud)]] const VertOutPointcloud &ptcloud_interp,
    [[in]] [[condition(is_pointcloud)]] const VertOutPointcloudFlat &ptcloud_interp_flat,
    [[frag_coord]] const float4 frag_co,
    [[front_facing]] const bool front_face)
{
  int3 froxel = int3(int2(frag_co.xy), 0);
  float offset = sampling.rng_1D_get(SAMPLING_VOLUME_W);
  float jitter = volume_froxel_jitter(froxel.xy, offset);

  draw::ID id{interp_flat.resource_id_raw};
  const uint resource_id = id.resource_id<1>();
  const ObjectMatrices obj = models.get(resource_id);
  const ObjectInfos ob_infos = infos.get(resource_id);
  const ViewMatrices view = views.get(0);

  VolumeProperties prop;

  ShadingData sd = init_globals(uni, interp, interp_flat, view, front_face, frag_co);
  if (pipe.is_mesh) [[static_branch]] {
    init_globals_mesh(interp, sd);
  }
  else if (pipe.is_curves) [[static_branch]] {
    init_globals_curves(interp, curves_interp, curves_interp_flat, sd, view);
  }
  else if (pipe.is_pointcloud) [[static_branch]] {
    init_globals_pointcloud(ptcloud_interp, ptcloud_interp_flat, sd);
  }

  if (srt.is_homogenous) [[static_branch]] {
    /* Homogenous volumes only evaluate properties at volume entrance and write the same values for
     * each froxel. */
    prop = srt.eval_froxel(kg, sd, uni, view, obj, ob_infos, froxel, jitter);
  }

  occupancy::Bits occupancy;

  if (!srt.is_world) [[static_branch]] {
    for (int j = 0; j < 8; j++) {
      occupancy.bits[j] = imageLoad(srt.occupancy_img, int3(froxel.xy, j)).r;
    }
  }

  /* Check all occupancy bits. */
  for (int j = 0; j < 8; j++) {
    for (int i = 0; i < 32; i++) {
      froxel.z = j * 32 + i;

      if (froxel.z >= imageSize(srt.out_scattering_img).z) {
        break;
      }

      if (!srt.is_world) [[static_branch]] {
        if (((occupancy.bits[j] >> i) & 1u) == 0) {
          continue;
        }
      }

      if (!srt.is_homogenous) [[static_branch]] {
        /* Heterogeneous volumes evaluate properties at every froxel position. */
        prop = srt.eval_froxel(kg, sd, uni, view, obj, ob_infos, froxel, jitter);
      }
      srt.write_froxel(froxel, prop);
    }
  }
}

}  // namespace eevee
