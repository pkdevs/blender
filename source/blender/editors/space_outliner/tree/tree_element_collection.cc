/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "DNA_outliner_types.h"
#include "DNA_scene_types.h"

#include "BLT_translation.hh"

#include "../outliner_intern.hh"

#include "tree_element_collection.hh"

#include "WM_types.hh"
#include "WM_api.hh"

namespace blender::ed::outliner {

TreeElementCollectionBase::TreeElementCollectionBase(TreeElement &legacy_te, Scene &scene)
    : AbstractTreeElement(legacy_te), scene_(scene)
{
  BLI_assert(legacy_te.store_elem->type == TSE_SCENE_COLLECTION_BASE);
  legacy_te.name = IFACE_("Scene Collection");
}

void TreeElementCollectionBase::expand(SpaceOutliner &space_outliner) const
{
  outliner_add_collection_recursive(&space_outliner, scene_.master_collection, &legacy_te_);
}

void TreeElementCollectionBase::expand_context_menu(bContext *C)
{
  WM_menu_name_call(C, "OUTLINER_MT_collection_new", wm::OpCallContext::InvokeRegionWin);
}

}  // namespace blender::ed::outliner
