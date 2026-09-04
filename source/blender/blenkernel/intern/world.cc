/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "MEM_guardedalloc.h"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_layer_types.h"
#include "DNA_light_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_world_types.h"

#include "BLI_listbase.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_set.hh"
#include "BLI_utildefines.hh"

#include "BKE_icons.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_preview_image.hh"
#include "BKE_world.h"

#include "BLT_translation.hh"

#include "DRW_engine.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "GPU_material.hh"

#include "BLO_read_write.hh"

#include "NOD_shader.h"

namespace blender {

/** Free (or release) any data used by this world (does not free the world itself). */
static void world_free_data(ID *id)
{
  World *wrld = id_cast<World *>(id);

  /* is no lib link block, but world extension */
  if (wrld->nodetree) {
    bke::node_tree_free_embedded_tree(wrld->nodetree);
    MEM_delete(wrld->nodetree);
    wrld->nodetree = nullptr;
  }

  GPU_material_free(&wrld->gpumaterial);

  BKE_icon_id_delete(id_cast<ID *>(wrld));
  BKE_previewimg_id_free(&wrld->id);

  MEM_SAFE_DELETE(wrld->lightgroup);
}

static void world_init_data(ID *id)
{
  World *wrld = id_cast<World *>(id);
  INIT_DEFAULT_STRUCT_AFTER(wrld, id);

  wrld->nodetree = bke::node_tree_add_tree_embedded(
      nullptr, &wrld->id, "World Nodetree", ntreeType_Shader->idname.ref());
}

/**
 * Only copy internal data of World ID from source
 * to already allocated/initialized destination.
 * You probably never want to use that directly,
 * use #BKE_id_copy or #BKE_id_copy_ex for typical needs.
 *
 * WARNING! This function will not handle ID user count!
 *
 * \param flag: Copying options (see BKE_lib_id.hh's LIB_ID_COPY_... flags for more).
 */
static void world_copy_data(Main *bmain,
                            std::optional<Library *> owner_library,
                            ID *id_dst,
                            const ID *id_src,
                            const int flag)
{
  World *wrld_dst = id_cast<World *>(id_dst);
  const World *wrld_src = id_cast<const World *>(id_src);

  const bool is_localized = (flag & LIB_ID_CREATE_LOCAL) != 0;
  /* Never handle user-count here for own sub-data. */
  const int flag_subdata = flag | LIB_ID_CREATE_NO_USER_REFCOUNT;
  /* Always need allocation of the embedded ID data. */
  const int flag_embedded_id_data = flag_subdata & ~LIB_ID_CREATE_NO_ALLOCATE;

  if (wrld_src->nodetree) {
    if (is_localized) {
      wrld_dst->nodetree = bke::node_tree_localize(wrld_src->nodetree, &wrld_dst->id);
    }
    else {
      BKE_id_copy_in_lib(bmain,
                         owner_library,
                         &wrld_src->nodetree->id,
                         &wrld_dst->id,
                         reinterpret_cast<ID **>(&wrld_dst->nodetree),
                         flag_embedded_id_data);
    }
  }

  wrld_dst->gpumaterial.clear_no_delete();

  if ((flag & LIB_ID_COPY_NO_PREVIEW) == 0) {
    BKE_previewimg_id_copy(&wrld_dst->id, &wrld_src->id);
  }
  else {
    wrld_dst->preview = nullptr;
  }

  if (wrld_src->lightgroup) {
    wrld_dst->lightgroup = MEM_dupalloc(wrld_src->lightgroup);
  }
}

static void world_foreach_id(ID *id, LibraryForeachIDData *data)
{
  World *world = reinterpret_cast<World *>(id);

  if (world->nodetree) {
    /* nodetree **are owned by IDs**, treat them as mere sub-data and not real ID! */
    BKE_LIB_FOREACHID_PROCESS_FUNCTION_CALL(
        data, BKE_library_foreach_ID_embedded(data, (ID **)&world->nodetree));
  }
}

static void world_foreach_working_space_color(ID *id, const IDTypeForeachColorFunctionCallback &fn)
{
  World *world = reinterpret_cast<World *>(id);

  fn.single(&world->horr);
}

static void world_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  World *wrld = id_cast<World *>(id);

  /* Clean up runtime data, important in undo case to reduce false detection of changed
   * datablocks. */
  wrld->gpumaterial.clear_no_delete();
  wrld->last_update = 0;

  /* Set deprecated #use_nodes for forward compatibility. */
  wrld->use_nodes = true;

  /* write LibData */
  writer->write_id_struct(id_address, wrld);
  BKE_id_blend_write(writer, &wrld->id);

  /* nodetree is integral part of world, no libdata */
  if (wrld->nodetree) {
    BLO_Write_IDBuffer temp_embedded_id_buffer{wrld->nodetree->id, writer};
    writer->write_struct_at_address_cast<bNodeTree>(wrld->nodetree, temp_embedded_id_buffer.get());
    bke::node_tree_blend_write(writer,
                               reinterpret_cast<bNodeTree *>(temp_embedded_id_buffer.get()));
  }

  BKE_previewimg_blend_write(writer, wrld->preview);

  if (wrld->lightgroup) {
    writer->write_struct(wrld->lightgroup);
  }
}

static void world_blend_read_data(BlendDataReader *reader, ID *id)
{
  World *wrld = id_cast<World *>(id);

  BLO_read_struct(reader, PreviewImage, &wrld->preview);
  BKE_previewimg_blend_read(reader, wrld->preview);
  wrld->gpumaterial.clear_no_delete();

  BLO_read_struct(reader, LightgroupMembership, &wrld->lightgroup);
}

IDTypeInfo IDType_ID_WO = {
    .id_code = World::id_type,
    .id_filter = FILTER_ID_WO,
    .dependencies_id_types = FILTER_ID_TE,
    .main_listbase_index = INDEX_ID_WO,
    .struct_size = sizeof(World),
    .name = "World",
    .name_plural = N_("worlds"),
    .translation_context = BLT_I18NCONTEXT_ID_WORLD,
    .flags = IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    .asset_type_info = nullptr,

    .init_data = world_init_data,
    .copy_data = world_copy_data,
    .free_data = world_free_data,
    .make_local = nullptr,
    .foreach_id = world_foreach_id,
    .foreach_cache = nullptr,
    .foreach_path = nullptr,
    .foreach_working_space_color = world_foreach_working_space_color,
    .owner_pointer_get = nullptr,

    .blend_write = world_blend_write,
    .blend_read_data = world_blend_read_data,
    .blend_read_after_liblink = nullptr,

    .blend_read_undo_preserve = nullptr,

    .lib_override_apply_post = nullptr,
};

World *BKE_world_add(Main *bmain, const char *name)
{
  World *wrld;

  wrld = BKE_id_new<World>(bmain, name);

  return wrld;
}

/* Where in the sky the given object stands, as an elevation above the horizon and an azimuth
 * measured the way sky_spherical_coordinates() measures it in the shaders. A light shines along
 * its own negative Z, so the way back toward it is the positive Z axis of its matrix. */
static void world_sky_angles_from_object(const Object &object,
                                         float *r_elevation,
                                         float *r_azimuth)
{
  const float3 toward = math::normalize(object.object_to_world().z_axis());
  *r_elevation = std::asin(math::clamp(toward.z, -1.0f, 1.0f));
  *r_azimuth = std::atan2(toward.x, toward.y);
}

/* Temperatures a light falls back to when it is not carrying one of its own, so that linking a
 * plain light leaves each body looking exactly as it does in Manual mode. These match the node's
 * own defaults: the Sun sits at its true temperature, which is what the sky is worked out for and
 * so costs nothing, and the Moon sits cooler than the sunlight it reflects, which is how moonlight
 * is seen once the eye is dark adapted.
 *
 * This repeats SKY_SUN_TEMPERATURE from intern/sky, deliberately: blenkernel does not link
 * against that library at all, and pulling it in for a single float would not be worth it. */
static const float SUN_TEMPERATURE_KELVIN = 5778.0f;
static const float MOON_TEMPERATURE_KELVIN = 8000.0f;

/* An offset can push a body over the pole, which leaves a sum that is no longer an elevation.
 * Fold it back the way the sky itself does, so the value that is stored, rendered and shown is
 * one and the same. */
static void world_sky_canonical_angles(float *elevation, float *rotation)
{
  float e = std::fmod(*elevation, 2.0f * float(M_PI));
  if (std::abs(e) >= float(M_PI)) {
    e -= std::copysign(2.0f * float(M_PI), e);
  }
  if (e > float(M_PI_2) || e < -float(M_PI_2)) {
    /* Over the pole and down the other side, which is the same place seen from behind. */
    e = std::copysign(float(M_PI), e) - e;
    *rotation += float(M_PI);
  }
  *elevation = e;

  float r = std::fmod(*rotation, 2.0f * float(M_PI));
  if (r > float(M_PI)) {
    r -= 2.0f * float(M_PI);
  }
  else if (r < -float(M_PI)) {
    r += 2.0f * float(M_PI);
  }
  *rotation = r;
}

/* The lamp an object carries, or null if it is not a lamp at all. The picker only offers Sun
 * lamps, but nothing stops a script assigning something else, so this has to be asked rather than
 * assumed. */
static const Light *world_sky_light_data(const Object &object)
{
  if (object.type != OB_LAMP || object.data == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<const Light *>(object.data);
}

/* Point the Hillaire skies in this world at the light objects they are linked to.
 *
 * This walks the tree the same way world_sky_tree_wants_camera() below does. They are kept apart
 * rather than shared, because this one writes into the nodes and that one must not, and folding
 * both into one walk would mean handing a const tree to code that can still reach through the
 * node's untyped storage.
 *
 * This is a purpose-built driver. The values are written into the evaluated node, so that every
 * consumer, from the viewport to the render engines, reads them without having to know where they
 * came from. The node the user edits is left alone, which is what makes switching a source back to
 * Manual lossless. */
static void world_sky_resolve_tree(bNodeTree *ntree,
                                   const Object *sun_object,
                                   const Object *moon_object,
                                   const Object *camera,
                                   Set<bNodeTree *> &visited)
{
  if (ntree == nullptr || !visited.add(ntree)) {
    return;
  }

  for (bNode &node : ntree->nodes) {
    /* Group nodes are walked into, since a sky is just as likely to be tucked inside one as it is
     * to sit at the top level. The visited set keeps a group that contains itself from looping. */
    if (ELEM(node.type_legacy, NODE_GROUP, NODE_CUSTOM_GROUP)) {
      world_sky_resolve_tree(
          reinterpret_cast<bNodeTree *>(node.id), sun_object, moon_object, camera, visited);
      continue;
    }
    if (node.type_legacy != SH_NODE_TEX_SKY || node.storage == nullptr) {
      continue;
    }
    NodeTexSky *sky = static_cast<NodeTexSky *>(node.storage);
    if (sky->sky_model != SHD_SKY_HILLAIRE) {
      continue;
    }

    if (sky->sun_source == SHD_SKY_SOURCE_DRIVEN && sun_object != nullptr) {
      float elevation, azimuth;
      world_sky_angles_from_object(*sun_object, &elevation, &azimuth);
      sky->sun_elevation = elevation + sky->sun_elevation_offset;
      sky->sun_rotation = azimuth + sky->sun_rotation_offset;
      world_sky_canonical_angles(&sky->sun_elevation, &sky->sun_rotation);

      if (const Light *light = world_sky_light_data(*sun_object)) {
        /* The Sun emits, so the light's own temperature stands in for the Sun's. A light that is
         * not using temperature leaves the Sun at its true one. */
        sky->sun_temperature = (light->mode & LA_USE_TEMPERATURE) ? light->temperature :
                                                                    SUN_TEMPERATURE_KELVIN;
        copy_v3_v3(sky->sun_color, &light->r);
      }
    }

    if (sky->moon_source == SHD_SKY_SOURCE_DRIVEN && moon_object != nullptr) {
      float elevation, azimuth;
      world_sky_angles_from_object(*moon_object, &elevation, &azimuth);
      sky->moon_elevation = elevation + sky->moon_elevation_offset;
      sky->moon_rotation = azimuth + sky->moon_rotation_offset;
      world_sky_canonical_angles(&sky->moon_elevation, &sky->moon_rotation);

      if (const Light *light = world_sky_light_data(*moon_object)) {
        /* The Moon's light is deliberately kept apart from the Sun's, even though it started out
         * as sunlight, so that a night can be graded on its own. */
        sky->moon_temperature = (light->mode & LA_USE_TEMPERATURE) ? light->temperature :
                                                                     MOON_TEMPERATURE_KELVIN;
        copy_v3_v3(sky->moon_color, &light->r);
      }
    }

    if (sky->altitude_source == SHD_SKY_ALTITUDE_CAMERA && camera != nullptr) {
      /* How much air stands above the viewer is what the altitude is for, so it is simply how
       * high the camera is standing. Below sea level there is no more atmosphere to add, so the
       * bottom of the range is where it stops. */
      const float height = camera->object_to_world().location().z;
      sky->altitude = std::clamp(height + sky->altitude_offset, 0.0f, 100000.0f);
    }
  }
}

/* Resolve every driven value in this world, once per evaluation. Returns early when there is
 * nothing linked, which is the common case and keeps the walk off the hot path entirely. */
static void world_sky_resolve_sources(Depsgraph *depsgraph, World *world)
{
  if (world->nodetree == nullptr) {
    return;
  }
  /* Which light stands in for which body, and which camera is doing the looking, are properties
   * of the scene, so they are read from the scene being evaluated rather than from the world,
   * which several scenes may share. */
  const Scene *scene = DEG_get_evaluated_scene(depsgraph);
  if (scene == nullptr) {
    return;
  }
  if (scene->sun_object == nullptr && scene->moon_object == nullptr && scene->camera == nullptr) {
    return;
  }
  Set<bNodeTree *> visited;
  world_sky_resolve_tree(
      world->nodetree, scene->sun_object, scene->moon_object, scene->camera, visited);
}

/* Whether any sky in this tree is taking its altitude from the camera. This exists so that the
 * depsgraph can leave the camera out of the world's relations when no sky is asking for it, since
 * tying the two together unconditionally would rebake the sky on every camera move. Reads only,
 * unlike the resolve above, which is why the two walks are not shared. */
static bool world_sky_tree_wants_camera(const bNodeTree *ntree, Set<const bNodeTree *> &visited)
{
  if (ntree == nullptr || !visited.add(ntree)) {
    return false;
  }
  for (const bNode &node : ntree->nodes) {
    if (ELEM(node.type_legacy, NODE_GROUP, NODE_CUSTOM_GROUP)) {
      if (world_sky_tree_wants_camera(reinterpret_cast<const bNodeTree *>(node.id), visited)) {
        return true;
      }
      continue;
    }
    if (node.type_legacy != SH_NODE_TEX_SKY || node.storage == nullptr) {
      continue;
    }
    const NodeTexSky *sky = static_cast<const NodeTexSky *>(node.storage);
    if (sky->sky_model == SHD_SKY_HILLAIRE && sky->altitude_source == SHD_SKY_ALTITUDE_CAMERA) {
      return true;
    }
  }
  return false;
}

bool BKE_world_sky_uses_camera_altitude(const World *world)
{
  Set<const bNodeTree *> visited;
  return world_sky_tree_wants_camera(world->nodetree, visited);
}

void BKE_world_eval(Depsgraph *depsgraph, World *world)
{
  DEG_debug_print_eval(depsgraph, __func__, world->id.name, world);
  world_sky_resolve_sources(depsgraph, world);
  GPU_material_free(&world->gpumaterial);
  world->last_update = DEG_get_update_count(depsgraph);
}

}  // namespace blender
