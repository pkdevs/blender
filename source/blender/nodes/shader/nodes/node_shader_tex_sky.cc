/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"
#include "node_util.hh"
#include "sky_hillaire.h"
#include "sky_hosek.h"
#include "sky_night.h"
#include "sky_nishita.h"

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_texture.h"

#include "BLI_math_matrix.hh"

#include "DNA_scene_types.h"
#include "DNA_world_types.h"

#include "BKE_node.hh"

#include "DEG_depsgraph_query.hh"

#include "IMB_colormanagement.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "NOD_socket_search_link.hh"

namespace blender {

namespace nodes::node_shader_tex_sky_cc {

/* Row of two buttons choosing whether a group of values is read from a light object or set by
 * hand. The values set by hand are kept either way, so switching back and forth is lossless. */
static void sky_source_toggle(ui::Layout &layout, PointerRNA *ptr, const StringRefNull prop)
{
  ui::Layout &row = layout.row(true);
  row.use_property_split_set(false);
  /* An expanded enum drops the labels off its own items when it is given an empty name, so the
   * name has to be left alone here even though it is never drawn. */
  row.prop(ptr, prop, ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
}

/* The Sun's light object lives on the Scene rather than on the node. That is where it belongs:
 * which light stands in for the Sun is a fact about the shot, like the camera, and not about a
 * World that several scenes may share. */
static void sky_object_row(ui::Layout &layout, bContext *C, const StringRefNull prop)
{
  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr) {
    return;
  }
  PointerRNA scene_ptr = RNA_id_pointer_create(&scene->id);
  layout.prop(&scene_ptr, prop, ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_LIGHT_SUN);
}

/* Only a sky that belongs to a World is resolved during evaluation, so only there can the Sun
 * follow a light object. Everywhere else the choice is not offered at all, rather than offered
 * and quietly ignored.
 *
 * The walk up handles a World's own embedded node tree. It stops at a real node group, which is a
 * standalone ID with no owner to walk to, so the toggle is not offered for a sky sitting inside
 * one even though the evaluator would resolve it. A group can be shared by several worlds and by
 * materials, and there is no one light object that is right for all of them, so declining to
 * offer the choice there is the honest answer rather than a gap to be filled in. */
static bool sky_can_be_driven(PointerRNA *ptr)
{
  ID *id = ptr->owner_id;
  while (id != nullptr && GS(id->name) == ID_NT) {
    id = BKE_id_owner_get(id, false);
  }
  return id != nullptr && GS(id->name) == ID_WO;
}

/* The same node as the depsgraph left it, where a driven Sun has already been resolved to
 * wherever its light object is pointing. The node the user edits still holds the values set by
 * hand, so it is the evaluated copy that has to be read to show what is actually being rendered.
 * Empty when this node is not the scene world's, or when nothing has been evaluated yet. */
static std::optional<PointerRNA> sky_evaluated_node(bContext *C, PointerRNA *ptr)
{
  const Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->world == nullptr || scene->world->nodetree == nullptr) {
    return std::nullopt;
  }
  if (ptr->owner_id != &scene->world->nodetree->id) {
    return std::nullopt;
  }
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    return std::nullopt;
  }
  World *world_eval = DEG_get_evaluated(depsgraph, scene->world);
  if (world_eval == nullptr || world_eval->nodetree == nullptr) {
    return std::nullopt;
  }
  const bNode &node = *ptr->data_as<bNode>();
  bNode *node_eval = bke::node_find_node_by_name(*world_eval->nodetree, node.name);
  if (node_eval == nullptr || node_eval->type_legacy != SH_NODE_TEX_SKY) {
    return std::nullopt;
  }
  return RNA_pointer_create_discrete(&world_eval->nodetree->id, RNA_Node, node_eval);
}

/* Column of values that the linked light object is driving. Read-only, because editing them would
 * write into the evaluated copy, which is thrown away on the next update. */
static ui::Layout &sky_driven_column(ui::Layout &layout)
{
  ui::Layout &col = layout.column(true);
  col.enabled_set(false);
  return col;
}

static void sky_panel_sun(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  layout.prop(ptr, "sun_disc", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  if (RNA_boolean_get(ptr, "sun_disc")) {
    ui::Layout &col = layout.column(true);
    col.prop(ptr, "sun_size", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    col.prop(ptr, "sun_intensity", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }

  const bool can_drive = sky_can_be_driven(ptr);
  if (can_drive) {
    sky_source_toggle(layout, ptr, "sun_source");
  }
  const bool driven = can_drive && RNA_enum_get(ptr, "sun_source") == SHD_SKY_SOURCE_DRIVEN;
  if (driven) {
    sky_object_row(layout, C, "sun_object");
    ui::Layout &col = layout.column(true);
    col.prop(ptr, "sun_elevation_offset", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    col.prop(ptr, "sun_rotation_offset", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

    if (std::optional<PointerRNA> eval = sky_evaluated_node(C, ptr)) {
      ui::Layout &angles = sky_driven_column(layout);
      angles.prop(&*eval, "sun_elevation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      angles.prop(&*eval, "sun_rotation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      ui::Layout &color = sky_driven_column(layout);
      color.prop(&*eval, "sun_temperature", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      color.prop(&*eval, "sun_color", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    }
  }
  else {
    ui::Layout &col = layout.column(true);
    col.prop(ptr, "sun_elevation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    col.prop(ptr, "sun_rotation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    ui::Layout &color = layout.column(true);
    color.prop(ptr, "sun_temperature", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    color.prop(ptr, "sun_color", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }
}

static void sky_panel_moon(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  layout.prop(ptr, "use_moon", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

  ui::Layout &body = layout.column(false);
  body.active_set(RNA_boolean_get(ptr, "use_moon"));

  ui::Layout &disc = body.column(true);
  disc.prop(ptr, "moon_size", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  disc.prop(ptr, "moon_intensity", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

  const bool can_drive = sky_can_be_driven(ptr);
  if (can_drive) {
    sky_source_toggle(body, ptr, "moon_source");
  }
  const bool driven = can_drive && RNA_enum_get(ptr, "moon_source") == SHD_SKY_SOURCE_DRIVEN;
  if (driven) {
    sky_object_row(body, C, "moon_object");
    ui::Layout &col = body.column(true);
    col.prop(ptr, "moon_elevation_offset", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    col.prop(ptr, "moon_rotation_offset", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

    if (std::optional<PointerRNA> eval = sky_evaluated_node(C, ptr)) {
      ui::Layout &angles = sky_driven_column(body);
      angles.prop(&*eval, "moon_elevation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      angles.prop(&*eval, "moon_rotation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      ui::Layout &color = sky_driven_column(body);
      color.prop(&*eval, "moon_temperature", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      color.prop(&*eval, "moon_color", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    }
  }
  else {
    ui::Layout &col = body.column(true);
    col.prop(ptr, "moon_elevation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    col.prop(ptr, "moon_rotation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    ui::Layout &color = body.column(true);
    color.prop(ptr, "moon_temperature", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    color.prop(ptr, "moon_color", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }

  /* Only worth saying when there is no light to say it about. The Moon's own glow lights the
   * scene softly and cannot cast a shadow, and a moonlit sky never gets bright enough for EEVEE
   * to pull a sun light out of it. */
  if (!driven) {
    body.label(IFACE_("Dim, casts no shadow"), ICON_INFO);
  }

  sky_source_toggle(body, ptr, "moon_phase_source");
  if (RNA_enum_get(ptr, "moon_phase_source") == SHD_SKY_PHASE_MANUAL) {
    body.prop(ptr, "moon_phase", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }
}

static void sky_panel_stars(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "use_stars", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

  ui::Layout &body = layout.column(false);
  body.active_set(RNA_boolean_get(ptr, "use_stars"));
  ui::Layout &col = body.column(true);
  col.prop(ptr, "star_density", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  col.prop(ptr, "star_brightness", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  col.prop(ptr, "star_color_variation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  col.prop(ptr, "milky_way", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  body.prop(ptr, "star_seed", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

  ui::Layout &sky_frame = body.column(true);
  sky_frame.prop(ptr, "star_latitude", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  sky_frame.prop(ptr, "star_longitude", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  sky_frame.prop(ptr, "star_rotation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void sky_panel_atmosphere(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  sky_source_toggle(layout, ptr, "altitude_source");
  if (RNA_enum_get(ptr, "altitude_source") == SHD_SKY_ALTITUDE_CAMERA) {
    layout.prop(ptr, "altitude_offset", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }
  else {
    layout.prop(ptr, "altitude", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }

  ui::Layout &col = layout.column(true);
  col.prop(ptr, "air_density", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  col.prop(ptr, "aerosol_density", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  col.prop(ptr, "ozone_density", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  col.prop(ptr, "aerosol_anisotropy", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

  ui::Layout &night = layout.column(true);
  night.prop(ptr, "airglow", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  night.prop(ptr, "night_exposure", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  night.prop(ptr, "night_adaptation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Color>("Color"_ustr).no_muted_links();
  b.add_default_layout();

  const bNode *node = b.node_or_null();
  if (node != nullptr && node->storage != nullptr &&
      static_cast<const NodeTexSky *>(node->storage)->sky_model == SHD_SKY_HILLAIRE)
  {
    b.add_panel("Sun"_ustr, 0).add_layout(sky_panel_sun);
    b.add_panel("Moon"_ustr, 1).default_closed(true).add_layout(sky_panel_moon);
    b.add_panel("Stars"_ustr, 2).default_closed(true).add_layout(sky_panel_stars);
    b.add_panel("Atmosphere"_ustr, 3).add_layout(sky_panel_atmosphere);
  }

  b.add_input<decl::Vector>("Vector"_ustr).hide_value();
}

static void node_shader_buts_tex_sky(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "sky_type", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);

  if (RNA_enum_get(ptr, "sky_type") == SHD_SKY_PREETHAM) {
    layout.prop(ptr, "sun_direction", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    layout.prop(ptr, "turbidity", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }
  else if (RNA_enum_get(ptr, "sky_type") == SHD_SKY_HOSEK) {
    layout.prop(ptr, "sun_direction", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    layout.prop(ptr, "turbidity", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    layout.prop(ptr, "ground_albedo", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }
  else if (RNA_enum_get(ptr, "sky_type") == SHD_SKY_HILLAIRE) {
    /* Drawn by the panels declared in node_declare(). */
  }
  else {
    layout.prop(ptr, "sun_disc", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

    if (RNA_boolean_get(ptr, "sun_disc")) {
      ui::Layout &col = layout.column(true);
      col.prop(ptr, "sun_size", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      col.prop(ptr, "sun_intensity", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    }

    {
      ui::Layout &col = layout.column(true);
      col.prop(ptr, "sun_elevation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      col.prop(ptr, "sun_rotation", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    }
    layout.prop(ptr, "altitude", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

    {
      ui::Layout &col = layout.column(true);
      col.prop(ptr, "air_density", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      col.prop(ptr, "aerosol_density", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
      col.prop(ptr, "ozone_density", ui::ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    }
  }
}

static void node_shader_init_tex_sky(bNodeTree * /*ntree*/, bNode *node)
{
  NodeTexSky *tex = MEM_new<NodeTexSky>("NodeTexSky");
  BKE_texture_mapping_default(&tex->base.tex_mapping, TEXMAP_TYPE_POINT);
  BKE_texture_colormapping_default(&tex->base.color_mapping);
  tex->sun_direction[0] = 0.0f;
  tex->sun_direction[1] = 0.0f;
  tex->sun_direction[2] = 1.0f;
  tex->turbidity = 2.2f;
  tex->ground_albedo = 0.3f;
  tex->sun_disc = true;
  tex->sun_size = DEG2RADF(0.545f);
  tex->sun_intensity = 1.0f;
  tex->sun_elevation = DEG2RADF(15.0f);
  tex->sun_rotation = 0.0f;
  tex->altitude = 100.0f;
  tex->air_density = 1.0f;
  tex->aerosol_density = 1.0f;
  tex->ozone_density = 1.0f;
  tex->sky_model = SHD_SKY_MULTIPLE_SCATTERING;
  /* Hillaire model. */
  tex->sun_temperature = SKY_SUN_TEMPERATURE;
  copy_v3_fl(tex->sun_color, 1.0f);
  tex->moon_size = DEG2RADF(0.52f);
  tex->moon_intensity = 1.0f;
  tex->moon_elevation = DEG2RADF(40.0f);
  tex->moon_rotation = DEG2RADF(180.0f);
  tex->moon_phase = 0.5f;
  tex->moon_temperature = SKY_MOON_TEMPERATURE;
  copy_v3_fl(tex->moon_color, 1.0f);
  tex->star_density = 1.0f;
  tex->star_brightness = 1.0f;
  tex->star_color_variation = 0.5f;
  tex->aerosol_anisotropy = 0.8f;
  tex->airglow = 1.0f;
  tex->milky_way = 1.0f;
  tex->milky_way_crowding = 0.35f;
  tex->milky_way_structure = 0.5f;
  tex->nebulae = 0.15f;
  tex->zodiacal_light = 1.0f;
  tex->night_exposure = 0.0f;
  tex->night_adaptation = 1.0f;
  tex->star_latitude = DEG2RADF(45.0f);
  tex->star_longitude = 0.0f;
  tex->star_rotation = 0.0f;
  node->storage = tex;
}

struct SkyModelPreetham {
  float config_Y[5], config_x[5], config_y[5]; /* named after xyY color space */
  float radiance[3];
};

static float sky_perez_function(const float *lam, float theta, float gamma)
{
  float ctheta = cosf(theta);
  float cgamma = cosf(gamma);

  return (1.0 + lam[0] * expf(lam[1] / ctheta)) *
         (1.0 + lam[2] * expf(lam[3] * gamma) + lam[4] * cgamma * cgamma);
}

static void sky_precompute_old(SkyModelPreetham *sunsky, const float sun_angles[], float turbidity)
{
  float theta = sun_angles[0];
  float theta2 = theta * theta;
  float theta3 = theta2 * theta;
  float T = turbidity;
  float T2 = T * T;
  float chi = (4.0f / 9.0f - T / 120.0f) * (M_PI - 2.0f * theta);

  sunsky->radiance[0] = (4.0453f * T - 4.9710f) * tanf(chi) - 0.2155f * T + 2.4192f;
  sunsky->radiance[0] *= 0.06f;

  sunsky->radiance[1] = (0.00166f * theta3 - 0.00375f * theta2 + 0.00209f * theta) * T2 +
                        (-0.02903f * theta3 + 0.06377f * theta2 - 0.03202f * theta + 0.00394f) *
                            T +
                        (0.11693f * theta3 - 0.21196f * theta2 + 0.06052f * theta + 0.25886f);

  sunsky->radiance[2] = (0.00275f * theta3 - 0.00610f * theta2 + 0.00317f * theta) * T2 +
                        (-0.04214f * theta3 + 0.08970f * theta2 - 0.04153f * theta + 0.00516f) *
                            T +
                        (0.15346f * theta3 - 0.26756f * theta2 + 0.06670f * theta + 0.26688f);

  sunsky->config_Y[0] = (0.1787f * T - 1.4630f);
  sunsky->config_Y[1] = (-0.3554f * T + 0.4275f);
  sunsky->config_Y[2] = (-0.0227f * T + 5.3251f);
  sunsky->config_Y[3] = (0.1206f * T - 2.5771f);
  sunsky->config_Y[4] = (-0.0670f * T + 0.3703f);

  sunsky->config_x[0] = (-0.0193f * T - 0.2592f);
  sunsky->config_x[1] = (-0.0665f * T + 0.0008f);
  sunsky->config_x[2] = (-0.0004f * T + 0.2125f);
  sunsky->config_x[3] = (-0.0641f * T - 0.8989f);
  sunsky->config_x[4] = (-0.0033f * T + 0.0452f);

  sunsky->config_y[0] = (-0.0167f * T - 0.2608f);
  sunsky->config_y[1] = (-0.0950f * T + 0.0092f);
  sunsky->config_y[2] = (-0.0079f * T + 0.2102f);
  sunsky->config_y[3] = (-0.0441f * T - 1.6537f);
  sunsky->config_y[4] = (-0.0109f * T + 0.0529f);

  sunsky->radiance[0] /= sky_perez_function(sunsky->config_Y, 0, theta);
  sunsky->radiance[1] /= sky_perez_function(sunsky->config_x, 0, theta);
  sunsky->radiance[2] /= sky_perez_function(sunsky->config_y, 0, theta);
}

/* Drain the color the eye could not find at this brightness, and slide what is left toward
 * blue. Thin wrapper so the float3/float[3] shuffle happens in one place. */
static float3 sky_adapt(const float3 color, const float luminance, const float strength)
{
  const float in[3] = {color[0], color[1], color[2]};
  float out[3];
  SKY_night_adaptation(in, luminance, strength, out);
  return float3(out);
}

/* The change the adaptation makes to airglow's own color, as a per-channel multiplier that can
 * be carried through the bake the same way an illuminant tint is. */
static float3 sky_airglow_tint(const float luminance, const float strength)
{
  float xyz[3];
  SKY_airglow_color(xyz);
  XYZ_to_RGB to_rgb;
  get_XYZ_to_RGB_for_gpu(&to_rgb);
  /* A near-monochromatic 560 nm emission sits outside the render's RGB gamut, so one channel of
   * its true color comes out slightly negative. A per-channel ratio against that would blow up,
   * so clamp into gamut first and leave any channel with nothing to scale alone. */
  const float3 physical(math::max(dot_v3v3(to_rgb.r, xyz), 0.0f),
                        math::max(dot_v3v3(to_rgb.g, xyz), 0.0f),
                        math::max(dot_v3v3(to_rgb.b, xyz), 0.0f));
  const float3 adapted = sky_adapt(physical, luminance, strength);
  float3 tint(1.0f, 1.0f, 1.0f);
  for (int i = 0; i < 3; i++) {
    tint[i] = (physical[i] > 1e-9f) ? adapted[i] / physical[i] : 1.0f;
  }
  return tint;
}

/* Light the Sun sends down: a blackbody at the given temperature, tinted by the color - the same
 * way a light object computes its own color. Divided through by the Sun's own reference
 * temperature, since the sky is already computed for a real solar spectrum and would otherwise
 * double up the Sun's color. The default temperature costs nothing; moving away from it shifts
 * the light the same way it would shift a lamp. */
static float3 sky_light_color(const float temperature, const float color[3])
{
  float blackbody[4];
  float reference[4];
  IMB_colormanagement_blackbody_temperature_to_rgb(blackbody, temperature);
  IMB_colormanagement_blackbody_temperature_to_rgb(reference, SKY_SUN_TEMPERATURE);

  float3 light(color);
  for (int i = 0; i < 3; i++) {
    light[i] *= (reference[i] > 0.0f) ? blackbody[i] / reference[i] : 1.0f;
  }
  return light;
}

/* Write a matrix out the way the sky library reads one: 3x3, row major. */
static void sky_matrix(const float3x3 &m, float r_matrix[9])
{
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      r_matrix[row * 3 + col] = m[col][row];
    }
  }
}

/* The same color written as a transform on XYZ, which is the space the sky texture is baked in.
 * Tinting is a per-channel multiply in the render's RGB space, so it has to be carried across
 * rather than applied to XYZ channels directly. Row major, to match the sky library. */
static void sky_light_color_transform(const float3 light, float r_transform[9])
{
  const float3x3 to_rgb = IMB_colormanagement_get_xyz_to_scene_linear();
  const float3x3 to_xyz = IMB_colormanagement_get_scene_linear_to_xyz();
  const float3x3 tint = math::from_scale<float3x3>(light);
  sky_matrix(to_xyz * tint * to_rgb, r_transform);
}

/* Thin float3 adapters around SKY_eclipse() and SKY_moon_illumination() (intern/sky), which speak
 * plain floats so the geometry itself - pure trig with no engine-specific dependency - can be
 * shared with the Cycles side rather than hand-duplicated. Only the render-time kernel code
 * genuinely needs one implementation per shading language; this doesn't. */
static void sky_eclipse(const bool use_moon,
                        const float sun_size,
                        const float moon_size,
                        const float sun_elevation,
                        const float sun_rotation,
                        const float moon_elevation,
                        const float moon_rotation,
                        float3 *r_sun_light,
                        float3 *r_moon_light)
{
  float sun_light[3] = {r_sun_light->x, r_sun_light->y, r_sun_light->z};
  float moon_light[3] = {r_moon_light->x, r_moon_light->y, r_moon_light->z};
  SKY_eclipse(use_moon,
              sun_size,
              moon_size,
              sun_elevation,
              sun_rotation,
              moon_elevation,
              moon_rotation,
              sun_light,
              moon_light);
  *r_sun_light = float3(sun_light[0], sun_light[1], sun_light[2]);
  *r_moon_light = float3(moon_light[0], moon_light[1], moon_light[2]);
}

static void sky_moon_illumination(const NodeTexSky *tex,
                                  const float moon_rotation,
                                  const float sun_elevation,
                                  const float sun_rotation,
                                  float3 *r_direction,
                                  float *r_phase_angle)
{
  float direction[3];
  SKY_moon_illumination(tex->moon_phase_source == SHD_SKY_PHASE_MANUAL,
                        tex->moon_phase,
                        tex->moon_elevation,
                        moon_rotation,
                        sun_elevation,
                        sun_rotation,
                        direction,
                        r_phase_angle);
  *r_direction = float3(direction[0], direction[1], direction[2]);
}

static void sky_simplify_multiscatter_elevation_rotation(float &sun_elevation, float &sun_rotation)
{
  /* Patch Sun position so users are able to animate the daylight cycle while keeping the shading
   * code simple. */
  float new_sun_elevation = sun_elevation;
  float new_sun_rotation = sun_rotation;

  /* Wrap `new_sun_elevation` into [-2PI..2PI] range. */
  new_sun_elevation = fmodf(new_sun_elevation, 2.0f * M_PI);
  /* Wrap `new_sun_elevation` into [-PI..PI] range. */
  if (fabsf(new_sun_elevation) >= M_PI) {
    new_sun_elevation -= copysignf(2.0f, new_sun_elevation) * M_PI;
  }
  /* Wrap `new_sun_elevation` into [-PI/2..PI/2] range while keeping the same absolute position.
   */
  if (new_sun_elevation >= M_PI / 2.0f || new_sun_elevation <= -M_PI / 2.0f) {
    new_sun_elevation = copysignf(M_PI, new_sun_elevation) - new_sun_elevation;
    new_sun_rotation += M_PI;
  }

  /* Wrap `new_sun_rotation` into [-2PI..2PI] range. */
  new_sun_rotation = fmodf(new_sun_rotation, 2.0f * M_PI);
  /* Wrap `new_sun_rotation` into [0..2PI] range. */
  if (new_sun_rotation < 0.0f) {
    new_sun_rotation += 2.0f * M_PI;
  }
  new_sun_rotation = 2.0f * M_PI - new_sun_rotation;

  sun_elevation = new_sun_elevation;
  sun_rotation = new_sun_rotation;
}

static int node_shader_gpu_tex_sky(GPUMaterial *mat,
                                   bNode *node,
                                   bNodeExecData * /*execdata*/,
                                   GPUNodeStack *in,
                                   GPUNodeStack *out)
{
  node_shader_gpu_default_tex_coord(mat, node, &in[0].link);
  node_shader_gpu_tex_mapping(mat, node, in, out);
  NodeTexSky *tex = static_cast<NodeTexSky *>(node->storage);
  float sun_angles[2]; /* [0]=theta=zenith angle  [1]=phi=azimuth */
  sun_angles[0] = acosf(tex->sun_direction[2]);
  sun_angles[1] = atan2f(tex->sun_direction[0], tex->sun_direction[1]);

  if (tex->sky_model == SHD_SKY_PREETHAM) {
    /* Preetham */
    SkyModelPreetham sunsky;
    sky_precompute_old(&sunsky, sun_angles, tex->turbidity);
    XYZ_to_RGB xyz_to_rgb;
    get_XYZ_to_RGB_for_gpu(&xyz_to_rgb);
    return GPU_stack_link(mat,
                          node,
                          "node_tex_sky_preetham",
                          in,
                          out,
                          /* Pass config_Y/x/y as 3x(vec4+float) */
                          GPU_uniform(&sunsky.config_Y[0]),
                          GPU_uniform(&sunsky.config_Y[4]),
                          GPU_uniform(&sunsky.config_x[0]),
                          GPU_uniform(&sunsky.config_x[4]),
                          GPU_uniform(&sunsky.config_y[0]),
                          GPU_uniform(&sunsky.config_y[4]),
                          GPU_uniform(sun_angles),
                          GPU_uniform(sunsky.radiance),
                          GPU_uniform(xyz_to_rgb.r),
                          GPU_uniform(xyz_to_rgb.g),
                          GPU_uniform(xyz_to_rgb.b));
  }
  if (tex->sky_model == SHD_SKY_HOSEK) {
    /* Hosek / Wilkie */
    sun_angles[0] = fmin(M_PI_2, sun_angles[0]); /* clamp to horizon */
    SKY_ArHosekSkyModelState *sky_state = SKY_arhosek_xyz_skymodelstate_alloc_init(
        tex->turbidity, tex->ground_albedo, fmax(0.0, M_PI_2 - sun_angles[0]));
    /* Pass sky_state->configs[3][9] as 3*(vec4+vec4)+vec3 */
    float config_x07[8], config_y07[8], config_z07[8], config_xyz8[3];
    for (int i = 0; i < 8; ++i) {
      config_x07[i] = float(sky_state->configs[0][i]);
      config_y07[i] = float(sky_state->configs[1][i]);
      config_z07[i] = float(sky_state->configs[2][i]);
    }
    for (int i = 0; i < 3; ++i) {
      config_xyz8[i] = float(sky_state->configs[i][8]);
    }
    float radiance[3];
    for (int i = 0; i < 3; i++) {
      radiance[i] = sky_state->radiances[i] * (2 * M_PI / 683);
    }
    SKY_arhosekskymodelstate_free(sky_state);
    XYZ_to_RGB xyz_to_rgb;
    get_XYZ_to_RGB_for_gpu(&xyz_to_rgb);
    return GPU_stack_link(mat,
                          node,
                          "node_tex_sky_hosekwilkie",
                          in,
                          out,
                          GPU_uniform(&config_x07[0]),
                          GPU_uniform(&config_x07[4]),
                          GPU_uniform(&config_y07[0]),
                          GPU_uniform(&config_y07[4]),
                          GPU_uniform(&config_z07[0]),
                          GPU_uniform(&config_z07[4]),
                          GPU_uniform(config_xyz8),
                          GPU_uniform(sun_angles),
                          GPU_uniform(radiance),
                          GPU_uniform(xyz_to_rgb.r),
                          GPU_uniform(xyz_to_rgb.g),
                          GPU_uniform(xyz_to_rgb.b));
  }

  /* Nishita */
  Array<float> pixels(4 * GPU_SKY_WIDTH * GPU_SKY_HEIGHT);

  /* The disc is divided through by its own size when its face is shaded, so a size of exactly
   * zero has to be kept away from. Cycles holds the same floor, in SkyTextureNode::get_sun_size(),
   * and the two have to agree or a hairline Sun renders differently in each. */
  const float sun_size = math::max(tex->sun_size, 0.0005f);
  float sun_rotation = tex->sun_rotation;
  float sun_elevation = tex->sun_elevation;
  /* Both rotations are measured the same way round for the user, and both are negated on the way
   * to the shaders, where they turn the sky rather than the body. The Sun's negation happens
   * inside sky_simplify_multiscatter_elevation_rotation() below. */
  const float moon_rotation = -tex->moon_rotation;
  /* Where the Moon is lit from. Worked out on the CPU so that the three shading backends are
   * handed the same answer rather than each deriving it again. */
  float3 moon_sun_dir(0.0f, 0.0f, 1.0f);
  float moon_phase_angle = 0.0f;
  /* The light each body sends toward the atmosphere, once its own color and whatever an eclipse
   * has taken away from it are both accounted for. Only the Hillaire model offers those colors,
   * so for the others they stay white. */
  float3 sun_light(1.0f, 1.0f, 1.0f);
  float3 moon_light(1.0f, 1.0f, 1.0f);
  float pixel_bottom[3];
  float pixel_top[3];
  if (tex->sky_model == SHD_SKY_SINGLE_SCATTERING) {
    SKY_single_scattering_precompute_texture(pixels.data(),
                                             4,
                                             GPU_SKY_WIDTH,
                                             GPU_SKY_HEIGHT,
                                             tex->sun_elevation,
                                             tex->altitude,
                                             tex->air_density,
                                             tex->aerosol_density,
                                             tex->ozone_density);
    SKY_single_scattering_precompute_sun(tex->sun_elevation,
                                         sun_size,
                                         tex->altitude,
                                         tex->air_density,
                                         tex->aerosol_density,
                                         pixel_bottom,
                                         pixel_top);

    /* The multi-scatter case takes care of rotation wrapping in the
     * sky_simplify_multiscatter_elevation_rotation(). */
    sun_rotation = fmodf(sun_rotation, 2.0f * M_PI);
    if (sun_rotation < 0.0f) {
      sun_rotation += 2.0f * M_PI;
    }
    sun_rotation = 2.0f * M_PI - sun_rotation;
  }
  else {
    /* Both multiple scattering models share the same parameterization. */
    sky_simplify_multiscatter_elevation_rotation(sun_elevation, sun_rotation);
    if (tex->sky_model == SHD_SKY_HILLAIRE) {
      SKY_hillaire_precompute_sun(sun_elevation,
                                  sun_size,
                                  tex->altitude,
                                  tex->air_density,
                                  tex->aerosol_density,
                                  tex->ozone_density,
                                  pixel_bottom,
                                  pixel_top);

      sky_moon_illumination(
          tex, moon_rotation, sun_elevation, sun_rotation, &moon_sun_dir, &moon_phase_angle);

      sun_light = sky_light_color(tex->sun_temperature, tex->sun_color);
      moon_light = sky_light_color(tex->moon_temperature, tex->moon_color);
      /* The Sun's disc keeps its full brightness where it still shows, since the Moon takes it
       * away by standing in front of it rather than by dimming it. Only the glow baked below
       * loses light. The Moon's disc is the thing that actually reddens in a lunar eclipse, so it
       * carries that. */
      float3 sun_glow = sun_light;
      sky_eclipse(tex->use_moon,
                  sun_size,
                  tex->moon_size,
                  sun_elevation,
                  sun_rotation,
                  tex->moon_elevation,
                  moon_rotation,
                  &sun_glow,
                  &moon_light);

      float sun_color_transform[9];
      sky_light_color_transform(sun_glow, sun_color_transform);
      /* The airglow is faint enough for the eye to lose color in, so it is adapted before the
       * bake, according to how bright it actually renders. It is also cut back into gamut as it
       * is added, which needs the trip into the render's RGB and the trip back out of it as two
       * separate steps. The tint rides on the way back. */
      const float3 airglow_tint = sky_airglow_tint(SKY_NOMINAL_LUMINANCE_AIRGLOW * tex->airglow *
                                                       exp2f(tex->night_exposure),
                                                   tex->night_adaptation);
      float airglow_to_rgb[9];
      float airglow_tinted_to_xyz[9];
      sky_matrix(IMB_colormanagement_get_xyz_to_scene_linear(), airglow_to_rgb);
      sky_matrix(IMB_colormanagement_get_scene_linear_to_xyz() *
                     math::from_scale<float3x3>(airglow_tint),
                 airglow_tinted_to_xyz);
      SKY_hillaire_precompute_texture(pixels.data(),
                                      4,
                                      GPU_SKY_WIDTH,
                                      GPU_SKY_HEIGHT,
                                      sun_elevation,
                                      tex->altitude,
                                      tex->air_density,
                                      tex->aerosol_density,
                                      tex->ozone_density,
                                      tex->aerosol_anisotropy,
                                      sun_color_transform);
      if (tex->use_moon) {
        /* Scattering is linear in the light that drives it, so the Moon's sky is the same bake
         * run again with the Moon as the illuminant, then turned to the right bearing and added.
         */
        const float moonlight = SKY_moonlight_scale(
            true, moon_phase_angle, tex->moon_intensity, tex->night_exposure);
        float moon_color_transform[9];
        sky_light_color_transform(sky_adapt(moon_light,
                                            SKY_NOMINAL_LUMINANCE_MOONGLOW * tex->moon_intensity *
                                                exp2f(tex->night_exposure),
                                            tex->night_adaptation),
                                  moon_color_transform);
        SKY_hillaire_add_moon(pixels.data(),
                              4,
                              GPU_SKY_WIDTH,
                              GPU_SKY_HEIGHT,
                              tex->moon_elevation,
                              sun_rotation - moon_rotation,
                              tex->altitude,
                              tex->air_density,
                              tex->aerosol_density,
                              tex->ozone_density,
                              tex->aerosol_anisotropy,
                              moonlight,
                              moon_color_transform);
      }
      /* Owes nothing to the Sun or the Moon, so it goes on last and on its own. */
      SKY_hillaire_add_airglow(pixels.data(),
                               4,
                               GPU_SKY_WIDTH,
                               GPU_SKY_HEIGHT,
                               tex->altitude,
                               tex->air_density,
                               tex->aerosol_density,
                               tex->ozone_density,
                               tex->airglow * exp2f(tex->night_exposure),
                               airglow_to_rgb,
                               airglow_tinted_to_xyz);
    }
    else {
      SKY_multiple_scattering_precompute_sun(sun_elevation,
                                             sun_size,
                                             tex->altitude,
                                             tex->air_density,
                                             tex->aerosol_density,
                                             tex->ozone_density,
                                             pixel_bottom,
                                             pixel_top);
      SKY_multiple_scattering_precompute_texture(pixels.data(),
                                                 4,
                                                 GPU_SKY_WIDTH,
                                                 GPU_SKY_HEIGHT,
                                                 sun_elevation,
                                                 tex->altitude,
                                                 tex->air_density,
                                                 tex->aerosol_density,
                                                 tex->ozone_density);
    }
  }

  XYZ_to_RGB xyz_to_rgb;
  get_XYZ_to_RGB_for_gpu(&xyz_to_rgb);

  /* To fix pole issue we clamp the v coordinate. */
  GPUSamplerState sampler = {GPU_SAMPLER_FILTERING_LINEAR,
                             GPU_SAMPLER_EXTEND_MODE_REPEAT,
                             GPU_SAMPLER_EXTEND_MODE_EXTEND};
  float layer;
  float sky_type = (tex->sky_model == SHD_SKY_SINGLE_SCATTERING) ? 0.0f : 1.0f;
  /* A negative angular diameter tells the shader that the disc is turned off.
   *
   * SKY_earth_intersection_angle() returns a positive angle. Negated, since standing at an
   * altitude above zero means the true horizon dips below the local horizontal - the visible sky
   * (and anything in it) extends down to a small *negative* elevation, not stops short of zero.
   * Matches the sign Cycles has always used for this same gate. */
  const float sun_params[4] = {sun_elevation,
                               tex->sun_disc ? sun_size : -1.0f,
                               tex->sun_intensity,
                               -SKY_earth_intersection_angle(tex->altitude)};
  /* The Moon belongs to the Hillaire model only. */
  const bool use_moon = (tex->sky_model == SHD_SKY_HILLAIRE) && tex->use_moon;
  /* The disc takes the same lift as the glow, the stars and the airglow. Left out, raising Night
   * Exposure brightens the whole night sky except the Moon, which sinks into its own halo. */
  const float moon_params[4] = {tex->moon_elevation,
                                moon_rotation,
                                use_moon ? math::max(tex->moon_size, 0.0005f) : -1.0f,
                                tex->moon_intensity * exp2f(tex->night_exposure)};

  const bool use_stars = (tex->sky_model == SHD_SKY_HILLAIRE) && tex->use_stars;
  const float star_params[4] = {use_stars ? tex->star_density : -1.0f,
                                tex->star_brightness * exp2f(tex->night_exposure),
                                tex->star_color_variation,
                                float(tex->star_seed)};

  float celestial[9];
  SKY_celestial_frame(tex->star_latitude, tex->star_longitude, tex->star_rotation, celestial);

  /* Star color is a temperature, like the Sun's and the Moon's. Both ends of the ramp are worked
   * out here rather than in the shader, because each backend has its own blackbody routine and
   * they would not agree; the middle of the ramp is the Sun's temperature, which
   * sky_light_color() normalizes to white. */
  const float white[3] = {1.0f, 1.0f, 1.0f};
  const float night_lift = exp2f(tex->night_exposure);
  const float star_luminance = SKY_NOMINAL_LUMINANCE_STARS * tex->star_brightness * night_lift;
  const float3 star_color_cool = sky_adapt(
      sky_light_color(SKY_STAR_TEMPERATURE_COOL, white), star_luminance, tex->night_adaptation);
  const float3 star_color_hot = sky_adapt(
      sky_light_color(SKY_STAR_TEMPERATURE_HOT, white), star_luminance, tex->night_adaptation);
  /* The night lift rides on the band's color rather than on its strength, since its strength also
   * sets how far the band thickens the star field and an exposure control must not change how
   * many stars there are. */
  const float milky_way = use_stars ? tex->milky_way : 0.0f;
  /* Physically the band is a distinctly yellow-brown thing, old cool stars reddened by their own
   * dust, and that is how it photographs. */
  const float milky_way_structure = use_stars ? tex->milky_way_structure : 0.0f;
  const float nebulae = use_stars ? tex->nebulae : 0.0f;
  const float zodiacal_light = (tex->sky_model == SHD_SKY_HILLAIRE) ?
                                   tex->zodiacal_light * night_lift :
                                   -1.0f;
  /* Scattered sunlight, so very nearly the Sun's own color, dulled a little by the dust. */
  const float3 zodiacal_tint = sky_adapt(float3(1.0f, 0.96f, 0.90f),
                                         SKY_NOMINAL_LUMINANCE_ZODIACAL * tex->zodiacal_light *
                                             night_lift,
                                         tex->night_adaptation);
  const float3 milky_way_tint = night_lift * sky_adapt(float3(1.0f, 0.93f, 0.82f),
                                                       SKY_NOMINAL_LUMINANCE_MILKY_WAY *
                                                           tex->milky_way * night_lift,
                                                       tex->night_adaptation);

  /* Straight up, where the least air stands in the way. The shader works out the rest. */
  float zenith_transmittance[3] = {1.0f, 1.0f, 1.0f};
  if (use_stars) {
    float dimmed[3];
    float clear[3];
    SKY_transmittance(float(M_PI_2),
                      tex->altitude,
                      tex->air_density,
                      tex->aerosol_density,
                      tex->ozone_density,
                      dimmed,
                      clear);
    XYZ_to_RGB to_rgb;
    get_XYZ_to_RGB_for_gpu(&to_rgb);
    const float dimmed_rgb[3] = {
        dot_v3v3(to_rgb.r, dimmed), dot_v3v3(to_rgb.g, dimmed), dot_v3v3(to_rgb.b, dimmed)};
    const float clear_rgb[3] = {
        dot_v3v3(to_rgb.r, clear), dot_v3v3(to_rgb.g, clear), dot_v3v3(to_rgb.b, clear)};
    for (int i = 0; i < 3; i++) {
      zenith_transmittance[i] = clamp_f(dimmed_rgb[i] / max_ff(clear_rgb[i], 1e-6f), 0.0f, 1.0f);
    }
  }

  /* Light reaching the Moon, then the viewer. Reusing the Sun precompute at the Moon's elevation
   * gives sunlight dimmed and reddened by whatever air lies along the line of sight to the Moon.
   * The Sun's angular diameter keeps the result as the Sun's own radiance, which is then turned
   * into an irradiance by its solid angle and spread over the hemisphere it scatters into. */
  float moon_illuminant[3] = {0.0f, 0.0f, 0.0f};
  if (use_moon) {
    float moon_bottom[3];
    float moon_top[3];
    SKY_hillaire_precompute_sun(tex->moon_elevation,
                                sun_size,
                                tex->altitude,
                                tex->air_density,
                                tex->aerosol_density,
                                tex->ozone_density,
                                moon_bottom,
                                moon_top);
    const float sun_solid_angle = 2.0f * M_PI * (1.0f - cosf(sun_size * 0.5f));
    const float scale = sun_solid_angle / M_PI;
    for (int i = 0; i < 3; i++) {
      moon_illuminant[i] = 0.5f * (moon_bottom[i] + moon_top[i]) * scale;
    }
  }
  /* Both discs carry the same light color the sky around them was baked with. */
  const bool hillaire = (tex->sky_model == SHD_SKY_HILLAIRE);
  /* The Sun's disc keeps its full brightness where it still shows, since the Moon takes it away
   * by standing in front of it rather than by dimming it. The Moon's disc is the thing that
   * actually reddens in a lunar eclipse, so it carries that. */
  const float3 sun_disc_color = hillaire ? sky_light_color(tex->sun_temperature, tex->sun_color) :
                                           float3(1.0f, 1.0f, 1.0f);
  const float3 moon_disc_color = hillaire ? moon_light : float3(1.0f, 1.0f, 1.0f);

  GPUNodeLink *sky_texture = GPU_image_sky(
      mat, GPU_SKY_WIDTH, GPU_SKY_HEIGHT, pixels.data(), &layer, sampler);
  return GPU_stack_link(mat,
                        node,
                        "node_tex_sky_nishita",
                        in,
                        out,
                        GPU_constant(&sky_type),
                        GPU_uniform(&sun_rotation),
                        GPU_uniform(xyz_to_rgb.r),
                        GPU_uniform(xyz_to_rgb.g),
                        GPU_uniform(xyz_to_rgb.b),
                        GPU_uniform(pixel_bottom),
                        GPU_uniform(pixel_top),
                        GPU_uniform(sun_params),
                        /* Each disc carries the same light color the sky around it was baked
                         * with. */
                        GPU_uniform(sun_disc_color),
                        GPU_uniform(moon_params),
                        GPU_uniform(moon_disc_color),
                        GPU_uniform(moon_sun_dir),
                        GPU_uniform(moon_illuminant),
                        GPU_uniform(star_params),
                        GPU_uniform(zenith_transmittance),
                        GPU_uniform(celestial),
                        GPU_uniform(celestial + 3),
                        GPU_uniform(celestial + 6),
                        GPU_uniform(star_color_cool),
                        GPU_uniform(star_color_hot),
                        GPU_uniform(milky_way_tint),
                        GPU_uniform(&milky_way),
                        GPU_uniform(&tex->milky_way_crowding),
                        GPU_uniform(&milky_way_structure),
                        GPU_uniform(&nebulae),
                        GPU_uniform(&zodiacal_light),
                        GPU_uniform(zodiacal_tint),
                        sky_texture,
                        GPU_constant(&layer));
}

static void node_shader_update_sky(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *sockVector = bke::node_find_socket(*node, SOCK_IN, "Vector"_ustr);

  NodeTexSky *tex = static_cast<NodeTexSky *>(node->storage);
  bke::node_set_socket_availability(*ntree,
                                    *sockVector,
                                    !(ELEM(tex->sky_model,
                                           SHD_SKY_SINGLE_SCATTERING,
                                           SHD_SKY_MULTIPLE_SCATTERING,
                                           SHD_SKY_HILLAIRE) &&
                                      tex->sun_disc == 1));
}

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const NodeDeclaration &declaration = *params.node_type().static_declaration;
  if (params.in_out() == SOCK_OUT) {
    search_link_ops_for_declarations(params, declaration.outputs);
    return;
  }
  if (params.node_tree().typeinfo->validate_link(params.other_socket().type, SOCK_FLOAT)) {
    params.add_item(IFACE_("Vector"), [](LinkSearchOpParams &params) {
      bNode &node = params.add_node("ShaderNodeTexSky"_ustr);
      NodeTexSky *tex = static_cast<NodeTexSky *>(node.storage);
      tex->sun_disc = false;
      params.update_and_connect_available_socket(node, "Vector"_ustr);
    });
  }
}

}  // namespace nodes::node_shader_tex_sky_cc

/* node type definition */
void register_node_type_sh_tex_sky()
{
  namespace file_ns = nodes::node_shader_tex_sky_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeTexSky"_ustr, SH_NODE_TEX_SKY);
  ntype.ui_name = "Sky Texture";
  ntype.ui_description = "Generate a procedural sky texture";
  ntype.enum_name_legacy = "TEX_SKY";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_tex_sky;
  ntype.default_width = bke::NodeWidth::_160;
  ntype.initfunc = file_ns::node_shader_init_tex_sky;
  bke::node_type_storage(
      ntype, "NodeTexSky", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::node_shader_gpu_tex_sky;
  ntype.updatefunc = file_ns::node_shader_update_sky;
  ntype.gather_link_search_ops = file_ns::node_gather_link_searches;

  bke::node_register_type(ntype);
}

}  // namespace blender
