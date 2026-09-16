/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_extra_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_extra_camera_pano)

#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "overlay_common_lib.glsl"
#include "select_lib.glsl"

void longitude_clip(
    float3 &vpos, float longitude_max, float longitude_min, float radius, bool skip = false)
{

  double rad = atan(vpos.x, -vpos.z);

  if (!skip) {
    if (rad > longitude_max) {

      float2 pos;
      pos.x = radius * cos(longitude_max - radians(90));
      pos.y = radius * sin(longitude_max - radians(90));

      vpos.x = pos.x;
      vpos.z = pos.y;

      /* vpos *= 0; */
    }
  }
  if (rad < longitude_min) {
    printf("SOMETHING IS SMALLER THAN MIN %f \n", vpos.z);
    float2 pos;
    pos.x = radius * cos(longitude_min - radians(90));
    pos.y = radius * sin(longitude_min - radians(90));

    vpos.x = pos.x;
    vpos.z = pos.y;
  }
}

void swap(float &a, float &b)
{
  float tmp = a;
  a = b;
  b = tmp;
}

bool bounce(float &a, float &b)
{

  if (a < b) {
    swap(a, b);
    return true;
  }
  return false;
}

void main()
{

  select_id_set(in_select_buf[gl_InstanceID]);

  float4x4 inst_obmat = data_buf[gl_InstanceID].object_to_world;
  float4 panoparams_0 = data_buf[gl_InstanceID].pano_params0;
  float4 panoparams_1 = data_buf[gl_InstanceID].pano_params1;
  float4x4 input_mat = inst_obmat;

  float4 inst_data = float4(input_mat[0][3], input_mat[1][3], input_mat[2][3], input_mat[3][3]);
  float4 color = data_buf[gl_InstanceID].color_;
  float4x4 obmat = input_mat;
  obmat[0][3] = obmat[1][3] = obmat[2][3] = 0.0f;
  obmat[3][3] = 1.0f;

  float3 vpos = pos;
  float3 vofs = float3(0.0f);

  const float latitude_mode = panoparams_1.y;

  const float rads90 = radians(90);
  const float cos90 = cos(rads90);
  const float sin90 = sin(rads90);

  float longitude_min = panoparams_0.x;
  float longitude_max = panoparams_0.y;

  bool longitude_swapped = bounce(longitude_max, longitude_min);

  float latitude_min = rads90 - panoparams_0.z;
  float latitude_max = rads90 - panoparams_0.w;

  bool latitude_swapped = bounce(latitude_min, latitude_max);

  float latitude_position = -asin(vpos.z / 1.0f);

  const float fisheye_fov = panoparams_0.x;
  const float longitude_angle = panoparams_1.x;

  float2 shift;
  shift.x = panoparams_1.y;
  shift.y = panoparams_1.z;

  float2 aspect;
  aspect.x = panoparams_1.w > 0 ? panoparams_1.w : 1.0f;
  aspect.y = panoparams_1.w < 0 ? -panoparams_1.w : 1.0f;

  final_color = color;
  if (color.a < 0.0f) {
    final_color.a = 1.0f;
  }

  float2 camera_corner = inst_data.xy;
  float2 camera_center = inst_data.zw;
  float camera_dist = color.a;

  if (flag_test(vclass, VCLASS_CAMERA_FRAME)) {
    if (camera_dist > 0.0f) {
      vpos.z = -abs(camera_dist);
    }
    else {
      vpos.z *= -abs(camera_dist);
    }
    vpos.xy = (camera_center + camera_corner * vpos.xy) * abs(vpos.z);
  }

  if (flag_test(vclass, VCLASS_CAMERA_FISHEYE_FRAME)) {
    printf("FISHEYE FRAME shift.x %f shift.y %f \n", shift.x, shift.y);
    /* Feel free to optimize this. I am not a math guy! */
    const float calculated_height = 1.0f * (1.0 - cos(fisheye_fov / 2));
    const float calculated_radius = (1.0f * sin(fisheye_fov / 2));
    const float shifted_radius_x = (calculated_radius * 2) * shift.x;
    const float shifted_radius_y = (calculated_radius * 2) * shift.y;

    vpos.x = vpos.x > 0 ? calculated_radius + shifted_radius_x :
                          -calculated_radius + shifted_radius_x;
    vpos.y = vpos.y > 0 ? calculated_radius + shifted_radius_y :
                          -calculated_radius + shifted_radius_y;
    vpos.z = -1.0f + calculated_height;
  }
  if (flag_test(vclass, VCLASS_CAMERA_FISHEYE_LONGITUDE)) {
    /* Feel free to optimize this. I am not a math guy! */

    float3 rotated = vpos;
    float rotation_offset = 0;

    if (flag_test(vclass, VCLASS_CAMERA_EQUIRECTANGULAR_LATITUDE)) {

      if (flag_test(vclass, VCLASS_CAMERA_EQUIRECTANGULAR_LEFT)) {
        rotation_offset = -longitude_max + radians(180);
      }
      else if (flag_test(vclass, VCLASS_CAMERA_EQUIRECTANGULAR_RIGHT)) {
        rotation_offset = -longitude_min + radians(180);
      }
      else {
        rotation_offset = radians(45);
      }
    }

    float cosAngle = cos(longitude_angle + rotation_offset);
    float sinAngle = sin(longitude_angle + rotation_offset);

    if (flag_test(vclass, VCLASS_CAMERA_EQUIRECTANGULAR_LATITUDE)) {

      if (latitude_position > rads90 - latitude_max) {
        float new_height = 1.0f * (1.0 - cos(latitude_max));
        float new_radius = 1.0f * sin(latitude_max);

        vpos.z = -1.0 + new_height;
        vpos.y = -new_radius;
      }

      if (latitude_position < rads90 - latitude_min) {
        float new_height = 1.0f * (1.0 - cos(latitude_min));
        float new_radius = 1.0f * sin(latitude_min);

        vpos.z = -1.0 + new_height;
        vpos.y = -new_radius;
      }

      rotated.y = vpos.y * cos90 - vpos.z * sin90;
      rotated.z = vpos.y * sin90 + vpos.z * cos90;
      vpos.y = rotated.y;
      vpos.z = rotated.z;

      rotated.x = vpos.x * cosAngle + vpos.z * sinAngle;
      rotated.z = -vpos.x * sinAngle + vpos.z * cosAngle;

      vpos.x = rotated.x;
      vpos.z = rotated.z;

      if (flag_test(vclass, VCLASS_CAMERA_EQUIRECTANGULAR_LEFT) ||
          flag_test(vclass, VCLASS_CAMERA_EQUIRECTANGULAR_RIGHT))
      {
      }
      else {

        float rad = atan(vpos.x, -vpos.z);

        if (rad > longitude_max) {

          vpos *= 0;
        }

        else if (rad < longitude_min) {

          vpos *= 0;
        }
      }
    }
    else {
      /* Equidistant Fisheye */
      if (latitude_position < rads90 - fisheye_fov / 2) {
        float new_height = 1.0f * (1.0 - cos(fisheye_fov / 2));
        float new_radius = 1.0f * sin(fisheye_fov / 2);
        vpos.z = -1.0f + new_height;
        vpos.y = -new_radius;
      }

      rotated.x = vpos.x * cosAngle - vpos.y * sinAngle;
      rotated.y = vpos.x * sinAngle + vpos.y * cosAngle;
      vpos.x = rotated.x;
      vpos.y = rotated.y;
    }
  }
  if (flag_test(vclass, VCLASS_CAMERA_FISHEYE_LATITUDE)) {

    if (latitude_position < rads90 - fisheye_fov / 2) {
      float new_height = 1.0f * (1.0 - cos(fisheye_fov / 2));
      float new_radius = 1.0f * sin(fisheye_fov / 2);

      vpos.z = -1.0f + new_height;

      vpos.x = 0;
      vpos.x = 0;
      /*       vpos.x = new_radius + (shift.x*2);
            vpos.y = new_radiusshift.y*2;  */
    }
    else {
      vpos *= 1.0f;
    }
  }
  if (flag_test(vclass, VCLASS_CAMERA_FISHEYE_HORIZON)) {

    float3 rotated = vpos;

    float calculated_height = 1.0f * (1.0 - cos(fisheye_fov / 2));
    float calculated_radius = 1.0f * sin(fisheye_fov / 2);
    vpos.xy *= calculated_radius;
    vpos.z = -1.0f + calculated_height;
  }
  if (flag_test(vclass, VCLASS_CAMERA_EQUIRECTANGULAR_LONGITUDE)) {
    float3 rotated = vpos;
    float calculated_radius;

    if (latitude_mode == 1.0f) {

      float calculated_height = 1.0f * (1.0 - cos(latitude_max));
      calculated_radius = 1.0f * sin(latitude_max);
      vpos.yz *= calculated_radius;
      vpos.x = 1.0f - calculated_height;

      rotated.x = vpos.x * cos90 + vpos.y * sin90;
      rotated.y = vpos.x * sin90 + vpos.y * cos90;

      vpos.x = rotated.x;
      vpos.y = rotated.y;
    }

    if (latitude_mode == -1.0f) {

      float calculated_height = 1.0f * (1.0 + cos(latitude_min));
      calculated_radius = 1.0f * sin(latitude_min);
      vpos.yz *= calculated_radius;
      vpos.x = -1.0f + calculated_height;

      rotated.x = vpos.x * cos90 + vpos.y * sin90;
      rotated.y = vpos.x * sin90 + vpos.y * cos90;

      vpos.x = rotated.x;
      vpos.y = rotated.y;
    }

    float l_max = longitude_max;
    float l_min = longitude_min;

    float rad = atan(vpos.x, -vpos.z);
    if (gl_VertexID == 0) {

      rad *= -1;
    }

    if (int(abs(degrees(rad))) == 180) {
      if (vpos.y < 0) {
        rad *= -1;
      }
    }

    if (rad > longitude_max) {
      vpos.x = calculated_radius * cos(l_max - radians(90));
      vpos.z = calculated_radius * sin(l_max - radians(90));
    }

    if (rad < longitude_min) {
      vpos.x = calculated_radius * cos(l_min - radians(90));
      vpos.z = calculated_radius * sin(l_min - radians(90));
    }
  }

  if (flag_test(vclass, VCLASS_CAMERA_EQUIRECTANGULAR_TRIA)) {

    vpos *= .2;
    float use_latitude = latitude_swapped ? latitude_min : latitude_max;

    float calculated_height = 1.0f * (1.0 - cos(use_latitude));
    float calculated_radius = 1.0f * sin(use_latitude);
    float tria_angle = (longitude_min + longitude_max) / 2;

    float margin = 0.02;

    if (latitude_swapped) {
      margin = -margin;
      if (vpos.y > 0) {
        vpos.y = -vpos.y;
      }
    }

    vpos.y += 1.0 - calculated_height + margin;

    vpos.z -= calculated_radius;

    float3 rotated;
    rotated.x = vpos.x * cos(-tria_angle) + vpos.z * sin(-tria_angle);
    rotated.z = -vpos.x * sin(-tria_angle) + vpos.z * cos(-tria_angle);

    vpos.x = rotated.x;
    vpos.z = rotated.z;
  }

  if (flag_test(vclass, VCLASS_CAMERA_FISHEYE_TRIA)) {

    vpos *= .2;
    float radius = 1.0f;
    float calculated_height = radius * (1.0 - cos(fisheye_fov / 2));
    float calculated_radius = radius * sin(fisheye_fov / 2);

    const float shifted_radius_x = (calculated_radius * 2) * shift.x;
    const float shifted_radius_y = (calculated_radius * 2) * shift.y;

    vpos.x += shifted_radius_x * aspect.x;
    vpos.y += (shifted_radius_y * aspect.y) + (calculated_radius * aspect.y);

    vpos.z = -1.0f + calculated_height;
  }

  float3 world_pos = (obmat * float4(vofs + vpos, 1.0f)).xyz;

  gl_Position = drw_point_world_to_homogenous(world_pos);

  edge_pos = edge_start = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) *
                          uniform_buf.size_viewport;

#if defined(SELECT_ENABLE)
  gl_Position.xy += uniform_buf.size_viewport_inv * gl_Position.w *
                    ((gl_VertexID % 2 == 0) ? -1.0f : 1.0f);
#endif

  view_clipping_distances(world_pos);
}
