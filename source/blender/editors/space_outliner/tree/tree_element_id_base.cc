/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "DNA_ID.h"
#include "DNA_outliner_types.h"
#include "../outliner_intern.hh"
#include "tree_element_id_base.hh"

namespace blender::ed::outliner {

TreeElementIDBase::TreeElementIDBase(TreeElement &legacy_te, void *data)
    : AbstractTreeElement(legacy_te), data_(data)
{
  BLI_assert(legacy_te.store_elem->type == TSE_ID_BASE);
  legacy_te.directdata = data_;
  legacy_te.name = "";
}
} // namespace blender::ed::outliner