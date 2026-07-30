/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_string_ref.hh"
#include "BLI_sys_types.hh"

#include "BKE_compositor.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"

namespace blender {

// static CLG_LogRef LOG = {"blend.doversion"};

static void do_version_set_grease_pencil_colors_options_to_inputs(bNodeTree &ntree, bNode &node)
{
  if (blender::bke::node_find_socket(node, SOCK_IN, "Mode"_ustr)) {
    return;
  }
  bNodeSocket &socket = version_node_add_socket(ntree, node, SOCK_IN, "NodeSocketMenu", "Mode");
  socket.default_value_typed<bNodeSocketValueMenu>()->value = node.custom1;
}

static void do_version_set_grease_pencil_depth_options_to_inputs(bNodeTree &ntree, bNode &node)
{
  if (blender::bke::node_find_socket(node, SOCK_IN, "Depth Order"_ustr)) {
    return;
  }
  bNodeSocket &socket = version_node_add_socket(
      ntree, node, SOCK_IN, "NodeSocketMenu", "Depth Order");
  socket.default_value_typed<bNodeSocketValueMenu>()->value = node.custom1;
}

static void do_version_merge_layers_options_to_inputs(bNodeTree &ntree, bNode &node)
{
  if (!version_node_ensure_storage_or_invalidate(node)) {
    return;
  }

  auto &storage = *reinterpret_cast<NodeGeometryMergeLayers *>(node.storage);

  if (blender::bke::node_find_socket(node, SOCK_IN, "Mode"_ustr)) {
    return;
  }
  bNodeSocket &socket = version_node_add_socket(ntree, node, SOCK_IN, "NodeSocketMenu", "Mode");
  socket.default_value_typed<bNodeSocketValueMenu>()->value = storage.mode;
}

static void compositing_node_group_to_effect(Main &main, Scene &scene)
{
  bNodeTree *node_group = version_get_scene_compositor_node_tree(&main, &scene);
  if (!node_group) {
    return;
  }

  SceneCompositorEffect &effect = bke::compositor::new_effect(scene, "Effect");
  effect.node_group = node_group;
  if (!node_group->compositor_node_asset_traits) {
    node_group->compositor_node_asset_traits = MEM_new<CompositorNodeAssetTraits>(__func__);
  }
  node_group->compositor_node_asset_traits->flag |= COMPOSIT_NODE_ASSET_SCENE_EFFECT;
  bke::node_update_asset_metadata(*node_group);
  scene.compositing_node_group = nullptr;
}

void do_versions_after_linking_530(FileData * /*fd*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 8)) {
    version_node_socket_index_animdata(
        bmain, NTREE_GEOMETRY, "GeometryNodeSetGreasePencilColor", 5, 1, 6);
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 11)) {
    for (Scene &scene : bmain->scenes) {
      compositing_node_group_to_effect(*bmain, scene);
    }
  }
  else {
    /* The now deprecated compositing_node_group is always written on file writes for forward
     * compatibility, so it has to be reset to nullptr if no versioning was needed.
     *
     * Todo(#140111): Forward compatibility support will be removed in 6.0, and this loop can then
     * be placed behind a `MAIN_VERSION_FILE_OLDER(bmain, 600, xxx)` check . */
    for (Scene &scene : bmain->scenes) {
      scene.compositing_node_group = nullptr;
    }
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

/* Assign COMPOSIT_NODE_ASSET_SCENE_EFFECT if the node group can be a suitable scene compositor
 * effect. It is suitable if it has a color output and a color input as its first socket. */
static void update_node_group_asset_traits_for_scene_compositor_effects(bNodeTree &node_group)
{
  node_group.ensure_interface_cache();
  if (node_group.interface_outputs().is_empty() || node_group.interface_inputs().is_empty()) {
    return;
  }

  if (StringRef(node_group.interface_outputs().first()->socket_type) != "NodeSocketColor") {
    return;
  }

  if (StringRef(node_group.interface_inputs().first()->socket_type) != "NodeSocketColor") {
    return;
  }

  if (!node_group.compositor_node_asset_traits) {
    node_group.compositor_node_asset_traits = MEM_new<CompositorNodeAssetTraits>(__func__);
  }
  node_group.compositor_node_asset_traits->flag |= COMPOSIT_NODE_ASSET_SCENE_EFFECT;
  bke::node_update_asset_metadata(node_group);
}

void blo_do_versions_530(FileData * /*fd*/, Library * /*lib*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 1)) {
    for (Scene &scene : bmain->scenes) {
      VPaint *wpaint = scene.toolsettings->wpaint;
      if (wpaint && wpaint->paint.brush_asset_reference) {
        const StringRefNull old_asset_id =
            wpaint->paint.brush_asset_reference->relative_asset_identifier;
        if (wpaint->paint.brush == nullptr && old_asset_id.endswith("Paint")) {
          /* The "Paint" brush asset was renamed to "Add Weight", find it via the default instead
           * of hard-coding the new name. */
          if (std::optional<AssetWeakReference> paint_brush_asset_reference =
                  BKE_paint_brush_type_default_reference(PaintMode::Weight,
                                                         WPAINT_BRUSH_TYPE_DRAW))
          {
            BKE_paint_brush_set(bmain, &wpaint->paint, *paint_brush_asset_reference);
          }
        }
      }
    }
  }

  /* The compositor previously did not support default inputs for group nodes, but some built-in
   * nodes had the position field default type for some inputs, so node groups would gain it as a
   * default type through some operators. Later, the default inputs were supported for group nodes,
   * though position field were not supported in the compositor, so it would assert. To fix this,
   * we reset any position field default input to the default value. */
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 3)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id) {
      if (node_tree->type == NTREE_COMPOSIT) {
        node_tree->ensure_interface_cache();
        for (bNodeTreeInterfaceSocket *input : node_tree->interface_inputs()) {
          if (input->default_input == NODE_DEFAULT_INPUT_POSITION_FIELD) {
            input->default_input = NODE_DEFAULT_INPUT_VALUE;
          }
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 4)) {
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &sl : area.spacedata) {
          if (sl.spacetype == SPACE_ACTION) {
            SpaceAction *saction = reinterpret_cast<SpaceAction *>(&sl);
            saction->cache_display |= TIME_CACHE_COMPOSITOR;
          }
        }
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 6)) {
    for (Brush &brush : bmain->brushes) {
      if (ELEM(brush.ob_mode, OB_MODE_WEIGHT_PAINT, OB_MODE_VERTEX_PAINT)) {
        brush.mesh_automasking_settings = MEM_new<MeshAutomaskingSettings>(__func__);
        brush.mesh_automasking_settings->cavity_curve = BKE_sculpt_default_cavity_curve();
      }
    }

    auto apply_to_paint = [&](Paint *paint) {
      if (paint == nullptr) {
        return;
      }

      paint->mesh_automasking_settings = MEM_new<MeshAutomaskingSettings>("blo_do_versions_520");
      paint->mesh_automasking_settings->cavity_curve = BKE_sculpt_default_cavity_curve();
      paint->mesh_automasking_settings->cavity_curve_op = BKE_sculpt_default_cavity_curve();
    };

    for (Scene &scene : bmain->scenes) {
      apply_to_paint(reinterpret_cast<Paint *>(scene.toolsettings->vpaint));
      apply_to_paint(reinterpret_cast<Paint *>(scene.toolsettings->wpaint));
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 7)) {
    for (Scene &scene : bmain->scenes) {
      for (ViewLayer &view_layer : scene.view_layers) {
        view_layer.eevee.denoising_pass_flags =
            EEVEE_DENOISING_PASS_USE_ALBEDO_ROUGHNESS_WEIGHTING;
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 8)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id_owner) {
      for (bNode &node : node_tree->nodes) {
        if (STREQ(node.idname, "GeometryNodeSetGreasePencilColor")) {
          do_version_set_grease_pencil_colors_options_to_inputs(*node_tree, node);
        }
        if (STREQ(node.idname, "GeometryNodeSetGreasePencilDepth")) {
          do_version_set_grease_pencil_depth_options_to_inputs(*node_tree, node);
        }
        if (STREQ(node.idname, "GeometryNodeMergeLayers")) {
          do_version_merge_layers_options_to_inputs(*node_tree, node);
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 9)) {
    for (Brush &brush : bmain->brushes) {
      if (brush.curve_hardness == nullptr) {
        brush.curve_hardness = brush.paint_flags & BRUSH_PAINT_HARDNESS_PRESSURE_INVERT ?
                                   BKE_paint_default_curve_inverted() :
                                   BKE_paint_default_curve();
      }
      if (brush.curve_auto_smooth == nullptr) {
        brush.curve_auto_smooth = BKE_paint_default_curve_inverted();
      }
      if (brush.curve_spacing == nullptr) {
        brush.curve_spacing = BKE_paint_default_curve();
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 503, 11)) {
    for (bNodeTree &node_group : bmain->nodetrees) {
      if (node_group.type != NTREE_COMPOSIT) {
        continue;
      }

      update_node_group_asset_traits_for_scene_compositor_effects(node_group);
    }
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

}  // namespace blender
