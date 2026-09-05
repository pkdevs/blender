/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Functions and helpers shared between tree-display types or other tree related code.
 */

#include "BLI_listbase.hh"

#include "BKE_idtype.hh"

#include "DNA_anim_types.h"
#include "DNA_object_types.h"
#include "DNA_outliner_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "../outliner_intern.hh"
#include "common.hh"

namespace blender::ed::outliner {

/* -------------------------------------------------------------------- */
/** \name ID Helpers.
 * \{ */

const char *outliner_idcode_to_plural(short idcode)
{
  const char *propname = BKE_idtype_idcode_to_name_plural(idcode);
  PropertyRNA *prop = RNA_struct_type_find_property(RNA_BlendData, propname);
  return (prop) ? RNA_property_ui_name(prop) : "UNKNOWN";
}

/** \} */

void outliner_make_object_parent_hierarchy(Vector<TreeElement *> *tree)
{
  /* build hierarchy */
  /* XXX also, set extents here... */

  for (int index : tree->index_range()) {
    TreeElement *te = (*tree)[index];
    TreeStoreElem *tselem = TREESTORE(te);

    if ((tselem->type == TSE_SOME_ID) && te->idcode == ID_OB) {
      Object *ob = id_cast<Object *>(tselem->id);
      if (ob->parent && ob->parent->id.newid) {
        tree->remove(index);
        TreeElement *tep = reinterpret_cast<TreeElement *>(ob->parent->id.newid);
        tep->subtree.append(te);
        te->parent = tep;
      }
    }
  }
}

bool outliner_animdata_test(const AnimData *adt)
{
  if (adt) {
    return (adt->action || adt->drivers.first() || adt->nla_tracks.first());
  }
  return false;
}

}  // namespace blender::ed::outliner
