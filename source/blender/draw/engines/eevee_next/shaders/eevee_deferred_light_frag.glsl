/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Compute light objects lighting contribution using Gbuffer data.
 *
 * Output light either directly to the radiance buffers or to temporary radiance accumulation
 * buffer that will be processed by other deferred lighting passes.
 */

#pragma BLENDER_REQUIRE(eevee_gbuffer_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_renderpass_lib.glsl)
#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_light_eval_lib.glsl)

void main()
{
  ivec2 texel = ivec2(gl_FragCoord.xy);

  float depth = texelFetch(hiz_tx, texel, 0).r;
  vec3 P = get_world_space_from_depth(uvcoordsvar.xy, depth);

  /* TODO(fclem): High precision derivative. */
  vec3 Ng = safe_normalize(cross(dFdx(P), dFdy(P)));
  vec3 V = cameraVec(P);
  float vP_z = dot(cameraForward, P) - dot(cameraForward, cameraPos);

  vec4 gbuffer_0_packed = texelFetch(gbuffer_closure_tx, ivec3(texel, 0), 0);
  vec4 gbuffer_1_packed = texelFetch(gbuffer_closure_tx, ivec3(texel, 1), 0);

  ClosureReflection reflection_data;
  reflection_data.N = gbuffer_normal_unpack(gbuffer_0_packed.xy);
  reflection_data.roughness = gbuffer_0_packed.z;

  ClosureDiffuse diffuse_data;
  diffuse_data.N = gbuffer_normal_unpack(gbuffer_1_packed.xy);
  /* These are only set for SSS case. */
  diffuse_data.sss_radius = vec3(0.0);
  diffuse_data.sss_id = 0u;
  float thickness = 0.0;

  ClosureRefraction refraction_data;
  refraction_data.N = diffuse_data.N;
  refraction_data.roughness = gbuffer_1_packed.z;
  refraction_data.ior = 0.0; /* Not needed. */

  bool is_refraction = gbuffer_is_refraction(gbuffer_1_packed);
  if (is_refraction) {
    /* Still evaluate the diffuse light so that dithered SSS / Refraction combination still
     * produces a complete diffuse light buffer that will be correctly convolved by the SSSS.
     * The refraction pixels will just set the diffuse radiance to 0. */
  }
  else if (textureSize(gbuffer_closure_tx, 0).z >= 3) {
    vec4 gbuffer_2_packed = texelFetch(gbuffer_closure_tx, ivec3(texel, 2), 0);
    diffuse_data.sss_radius = gbuffer_sss_radii_unpack(gbuffer_2_packed.xyz);
    diffuse_data.sss_id = gbuffer_object_id_unorm16_unpack(gbuffer_2_packed.w);
    thickness = gbuffer_thickness_unpack(gbuffer_1_packed.z);
  }

  vec3 diffuse_light = vec3(0.0);
  vec3 reflection_light = vec3(0.0);
  vec3 refraction_light = vec3(0.0);
  float shadow = 1.0;

  light_eval(diffuse_data,
             reflection_data,
             P,
             Ng,
             V,
             vP_z,
             thickness,
             diffuse_light,
             reflection_light,
             shadow);

  render_pass_value_out(uniform_buf.render_pass.shadow_id, shadow);

  /* Store lighting for next deferred pass. */
  /* Output object ID for sub-surface screen space processing. */
  float f_sss_id = gbuffer_object_id_f16_pack(diffuse_data.sss_id);

  imageStore(direct_diffuse_img, texel, vec4(diffuse_light, f_sss_id));
  imageStore(direct_reflect_img, texel, vec4(reflection_light, 1.0));
  imageStore(direct_refract_img, texel, vec4(refraction_light, 1.0));
}
