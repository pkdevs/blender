/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BKE_instances.hh"

#include "DNA_pointcloud_types.h"

#include "NOD_geometry_nodes_viewer_bundles.hh"

#include "draw_cache_impl.hh"
#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Displays geometry passed to the viewer node in bundles (e.g. #GeometryOverlayBundle). Unlike
 * regular viewer geometry this is only drawn as an overlay with a flat color. Loose edges are
 * expanded to a screen space width.
 */
class ViewerFlatGeometry : Overlay {
 private:
  PassMain ps_ = {"viewer_flat_geometry_ps_"};
  PassMain::Sub *mesh_sub_ = nullptr;
  PassMain::Sub *pointcloud_sub_ = nullptr;
  /* A simple pass is used because the edge width can be different for every mesh. */
  PassSimple edges_ps_ = {"viewer_flat_geometry_edges_ps_"};

 public:
  void begin_sync(Resources &res, const State &state) final
  {
    ps_.init();
    edges_ps_.init();
    enabled_ = state.is_space_v3d() && !res.is_selection() && !state.is_depth_only_drawing &&
               !state.hide_overlays && state.show_attribute_viewer();
    if (!enabled_) {
      return;
    }
    ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
    ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
                      DRW_STATE_BLEND_ALPHA,
                  state.clipping_plane_count);
    auto create_sub = [&](const char *name, gpu::Shader *shader) {
      PassMain::Sub &sub = ps_.sub(name);
      sub.shader_set(shader);
      /* The shaders for the attribute viewer overlay support a color per element. Use a constant
       * opacity of 1.0 to instead use the opacity from the attribute. */
      sub.push_constant("opacity", 1.0f);
      return &sub;
    };
    mesh_sub_ = create_sub("mesh", res.shaders->attribute_viewer_mesh.get());
    pointcloud_sub_ = create_sub("pointcloud", res.shaders->attribute_viewer_pointcloud.get());

    edges_ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    edges_ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
    edges_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH |
                            DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA,
                        state.clipping_plane_count);
    edges_ps_.shader_set(res.shaders->viewer_flat_geometry_edges.get());
  }

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State & /*state*/) final
  {
    if (!enabled_ || !ob_ref.preview_overlay_only()) {
      return;
    }
    Object &object = *ob_ref.object;
    switch (object.type) {
      case OB_MESH: {
        Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
        const ResourceHandleRange res_handle = manager.unique_handle(ob_ref);
        mesh_sub_->draw(DRW_mesh_batch_cache_get_viewer_color_surface(mesh), res_handle);
        const float edge_width = edge_width_get(ob_ref);
        if (edge_width > 0.0f) {
          if (gpu::Batch *batch = DRW_mesh_batch_cache_get_viewer_color_loose_edges(mesh)) {
            edges_ps_.push_constant("edge_width", edge_width);
            edges_ps_.draw_expand(batch, GPU_PRIM_TRIS, 2, 1, res_handle);
          }
        }
        break;
      }
      case OB_POINTCLOUD: {
        using nodes::viewer_bundles::GeometryOverlayBundle;
        PointCloud &pointcloud = DRW_object_get_data_for_drawing<PointCloud>(*ob_ref.object);
        if (pointcloud.totpoint == 0 || pointcloud.type != PointCloudType::Points) {
          break;
        }
        const ResourceHandleRange res_handle = manager.unique_handle(ob_ref);
        const StringRef color_name = GeometryOverlayBundle::color_attribute_name;
        /* The color attribute is added when logging the geometry if it doesn't exist. */
        if (gpu::VertBuf **colors = DRW_pointcloud_evaluated_attribute(&pointcloud, color_name)) {
          gpu::Batch *batch = pointcloud_sub_pass_setup(
              *pointcloud_sub_, ob_ref, res_handle, nullptr);
          pointcloud_sub_->bind_texture("attribute_tx", colors);
          pointcloud_sub_->draw(batch, res_handle);
        }
        break;
      }
      default:
        break;
    }
  }

  void pre_draw(Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }
    manager.generate_commands(ps_, view);
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }
    GPU_framebuffer_bind(framebuffer);
    manager.submit_only(ps_, view);
    manager.submit(edges_ps_, view);
  }

 private:
  /** Retrieve the edge width from the top level instance that corresponds to the source bundle.
   */
  static float edge_width_get(const ObjectRef &ob_ref)
  {
    using nodes::viewer_bundles::GeometryOverlayBundle;
    const bke::GeometrySet *base_geometry = ob_ref.preview_base_geometry();
    const int instance_index = ob_ref.preview_instance_index();
    if (!base_geometry || instance_index < 0) {
      return GeometryOverlayBundle::default_edge_width;
    }
    const bke::Instances *instances = base_geometry->get_instances();
    if (!instances) {
      return GeometryOverlayBundle::default_edge_width;
    }
    const VArray edge_widths = *instances->attributes().lookup<float>(
        GeometryOverlayBundle::edge_width_instance_attribute_name, bke::AttrDomain::Instance);
    if (!edge_widths) {
      return GeometryOverlayBundle::default_edge_width;
    }
    return edge_widths[instance_index];
  }
};

}  // namespace blender::draw::overlay
