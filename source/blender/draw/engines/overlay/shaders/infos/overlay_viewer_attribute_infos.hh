/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "draw_object_infos_infos.hh"
#  include "draw_view_infos.hh"
#  include "gpu_index_load_infos.hh"
#  include "overlay_common_infos.hh"
#  include "overlay_shader_shared.hh"
#endif

#ifdef GLSL_CPP_STUBS
#  define CURVES_SHADER
#  define DRW_HAIR_INFO

#  define POINTCLOUD_SHADER
#  define DRW_POINTCLOUD_INFO
#endif

#include "overlay_common_infos.hh"

GPU_SHADER_INTERFACE_INFO(overlay_viewer_attribute_iface)
SMOOTH(float4, final_color)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(overlay_viewer_attribute_common)
PUSH_CONSTANT(float, opacity)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(overlay_viewer_attribute_mesh)
DO_STATIC_COMPILATION()
VERTEX_SOURCE("overlay_viewer_attribute_mesh_vert.glsl")
FRAGMENT_SOURCE("overlay_viewer_attribute_frag.glsl")
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_OUT(1, float4, line_output)
VERTEX_IN(0, float3, pos)
VERTEX_IN(1, float4, attribute_value)
VERTEX_OUT(overlay_viewer_attribute_iface)
ADDITIONAL_INFO(overlay_viewer_attribute_common)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_globals)
GPU_SHADER_CREATE_END()

CREATE_INFO_VARIANT(overlay_viewer_attribute_mesh_clipped,
                    overlay_viewer_attribute_mesh,
                    drw_clipped)

GPU_SHADER_CREATE_INFO(overlay_viewer_attribute_pointcloud)
DO_STATIC_COMPILATION()
VERTEX_SOURCE("overlay_viewer_attribute_pointcloud_vert.glsl")
FRAGMENT_SOURCE("overlay_viewer_attribute_frag.glsl")
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_OUT(1, float4, line_output)
SAMPLER(3, samplerBuffer, attribute_tx)
VERTEX_OUT(overlay_viewer_attribute_iface)
ADDITIONAL_INFO(overlay_viewer_attribute_common)
ADDITIONAL_INFO(draw_pointcloud)
ADDITIONAL_INFO(draw_globals)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
GPU_SHADER_CREATE_END()

CREATE_INFO_VARIANT(overlay_viewer_attribute_pointcloud_clipped,
                    overlay_viewer_attribute_pointcloud,
                    drw_clipped)

GPU_SHADER_CREATE_INFO(overlay_viewer_attribute_curve)
DO_STATIC_COMPILATION()
VERTEX_SOURCE("overlay_viewer_attribute_curve_vert.glsl")
FRAGMENT_SOURCE("overlay_viewer_attribute_frag.glsl")
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_OUT(1, float4, line_output)
VERTEX_IN(0, float3, pos)
VERTEX_IN(1, float4, attribute_value)
VERTEX_OUT(overlay_viewer_attribute_iface)
ADDITIONAL_INFO(overlay_viewer_attribute_common)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_globals)
ADDITIONAL_INFO(draw_modelmat)
GPU_SHADER_CREATE_END()

CREATE_INFO_VARIANT(overlay_viewer_attribute_curve_clipped,
                    overlay_viewer_attribute_curve,
                    drw_clipped)

GPU_SHADER_CREATE_INFO(overlay_viewer_attribute_curves)
DO_STATIC_COMPILATION()
VERTEX_SOURCE("overlay_viewer_attribute_curves_vert.glsl")
FRAGMENT_SOURCE("overlay_viewer_attribute_frag.glsl")
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_OUT(1, float4, line_output)
SAMPLER(2, samplerBuffer, color_tx)
PUSH_CONSTANT(bool, is_point_domain)
VERTEX_OUT(overlay_viewer_attribute_iface)
ADDITIONAL_INFO(overlay_viewer_attribute_common)
ADDITIONAL_INFO(draw_curves)
ADDITIONAL_INFO(draw_curves_infos)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_globals)
ADDITIONAL_INFO(draw_modelmat)
GPU_SHADER_CREATE_END()

CREATE_INFO_VARIANT(overlay_viewer_attribute_curves_clipped,
                    overlay_viewer_attribute_curves,
                    drw_clipped)

GPU_SHADER_NAMED_INTERFACE_INFO(overlay_viewer_flat_geometry_edge_iface, geometry_out)
SMOOTH(float4, final_color)
GPU_SHADER_NAMED_INTERFACE_END(geometry_out)
GPU_SHADER_NAMED_INTERFACE_INFO(overlay_viewer_flat_geometry_edge_noperspective_iface,
                                geometry_noperspective_out)
NO_PERSPECTIVE(float, edge_coord)
GPU_SHADER_NAMED_INTERFACE_END(geometry_noperspective_out)

GPU_SHADER_CREATE_INFO(overlay_viewer_flat_geometry_edges)
DO_STATIC_COMPILATION()
STORAGE_BUF_FREQ(0, read, float, pos[], GEOMETRY)
STORAGE_BUF_FREQ(1, read, float, attribute_value[], GEOMETRY)
PUSH_CONSTANT(int2, gpu_attr_0)
PUSH_CONSTANT(int2, gpu_attr_1)
/* Full width of the edges in pixels, before UI scale. */
PUSH_CONSTANT(float, edge_width)
VERTEX_OUT(overlay_viewer_flat_geometry_edge_iface)
VERTEX_OUT(overlay_viewer_flat_geometry_edge_noperspective_iface)
FRAGMENT_OUT(0, float4, out_color)
FRAGMENT_OUT(1, float4, line_output)
TYPEDEF_SOURCE("overlay_shader_shared.hh")
VERTEX_SOURCE("overlay_viewer_flat_geometry_edges_vert.glsl")
FRAGMENT_SOURCE("overlay_viewer_flat_geometry_edges_frag.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
ADDITIONAL_INFO(draw_globals)
ADDITIONAL_INFO(gpu_index_buffer_load)
GPU_SHADER_CREATE_END()

CREATE_INFO_VARIANT(overlay_viewer_flat_geometry_edges_clipped,
                    overlay_viewer_flat_geometry_edges,
                    drw_clipped)
