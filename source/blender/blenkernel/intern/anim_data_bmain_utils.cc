/* SPDX-FileCopyrightText: 2025 Blender Authors, Joshua Leung. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* `Scene->nodetree` is deprecated but still relevant for backward compatibility. */
#define DNA_DEPRECATED_ALLOW

/** \file
 * \ingroup bke
 */

#include "BKE_anim_data.hh"
#include "BKE_animsys.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"

#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"
#include "DNA_world_types.h"

namespace blender {

/* Whole Database Ops -------------------------------------------- */

void BKE_animdata_main_cb(Main *bmain, const FunctionRef<void(ID *, AnimData *)> func)
{
  ID *id;

/* standard data version */
#define ANIMDATA_IDS_CB(first) \
  for (id = static_cast<ID *>(first); id; id = static_cast<ID *>(id->next)) { \
    AnimData *adt = BKE_animdata_from_id(id); \
    if (adt) { \
      func(id, adt); \
    } \
  } \
  (void)0

/* "embedded" nodetree cases (i.e. scene/material/texture->nodetree) */
#define ANIMDATA_NODETREE_IDS_CB(first, NtId_Type) \
  for (id = static_cast<ID *>(first); id; id = static_cast<ID *>(id->next)) { \
    AnimData *adt = BKE_animdata_from_id(id); \
    NtId_Type *ntp = (NtId_Type *)id; \
    if (ntp->nodetree) { \
      AnimData *adt2 = BKE_animdata_from_id((ID *)ntp->nodetree); \
      if (adt2) { \
        func((ID *)ntp->nodetree, adt2); \
      } \
    } \
    if (adt) { \
      func(id, adt); \
    } \
  } \
  (void)0

  /* nodes */
  ANIMDATA_IDS_CB(bmain->nodetrees.first_);

  /* textures */
  ANIMDATA_NODETREE_IDS_CB(bmain->textures.first_, Tex);

  /* lights */
  ANIMDATA_NODETREE_IDS_CB(bmain->lights.first_, Light);

  /* materials */
  ANIMDATA_NODETREE_IDS_CB(bmain->materials.first_, Material);

  /* cameras */
  ANIMDATA_IDS_CB(bmain->cameras.first_);

  /* shapekeys */
  ANIMDATA_IDS_CB(bmain->shapekeys.first_);

  /* metaballs */
  ANIMDATA_IDS_CB(bmain->metaballs.first_);

  /* curves */
  ANIMDATA_IDS_CB(bmain->curves.first_);

  /* armatures */
  ANIMDATA_IDS_CB(bmain->armatures.first_);

  /* lattices */
  ANIMDATA_IDS_CB(bmain->lattices.first_);

  /* meshes */
  ANIMDATA_IDS_CB(bmain->meshes.first_);

  /* particles */
  ANIMDATA_IDS_CB(bmain->particles.first_);

  /* speakers */
  ANIMDATA_IDS_CB(bmain->speakers.first_);

  /* movie clips */
  ANIMDATA_IDS_CB(bmain->movieclips.first_);

  /* objects */
  ANIMDATA_IDS_CB(bmain->objects.first_);

  /* masks */
  ANIMDATA_IDS_CB(bmain->masks.first_);

  /* worlds */
  ANIMDATA_NODETREE_IDS_CB(bmain->worlds.first_, World);

  /* scenes */
  ANIMDATA_NODETREE_IDS_CB(bmain->scenes.first_, Scene);

  /* line styles */
  ANIMDATA_IDS_CB(bmain->linestyles.first_);

  /* grease pencil */
  ANIMDATA_IDS_CB(bmain->gpencils.first_);

  /* grease pencil */
  ANIMDATA_IDS_CB(bmain->grease_pencils.first_);

  /* palettes */
  ANIMDATA_IDS_CB(bmain->palettes.first_);

  /* cache files */
  ANIMDATA_IDS_CB(bmain->cachefiles.first_);

  /* Hair Curves. */
  ANIMDATA_IDS_CB(bmain->hair_curves.first_);

  /* pointclouds */
  ANIMDATA_IDS_CB(bmain->pointclouds.first_);

  /* volumes */
  ANIMDATA_IDS_CB(bmain->volumes.first_);
}

}  // namespace blender
