/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "DNA_armature_types.h"
#include "DNA_object_types.h"
#include "DNA_outliner_types.h"

#include "BLI_listbase.hh"

#include "BLT_translation.hh"

#include "../outliner_intern.hh"

#include "tree_element_pose.hh"

namespace blender {

struct bConstraint;

namespace ed::outliner {

TreeElementPoseBase::TreeElementPoseBase(TreeElement &legacy_te, Object &object)
    : AbstractTreeElement(legacy_te), object_(object)
{
  BLI_assert(legacy_te.store_elem->type == TSE_POSE_BASE);
  legacy_te.name = IFACE_("Pose");
}

void TreeElementPoseBase::expand(SpaceOutliner & /*space_outliner*/) const
{
  bArmature *arm = id_cast<bArmature *>(object_.data);

  /* channels undefined in editmode, but we want the 'tenla' pose icon itself */
  if ((arm->edbo == nullptr) && (object_.mode & OB_MODE_POSE)) {
    int const_index = 1000; /* ensure unique id for bone constraints */

    for (const auto [a, pchan] : object_.pose->chanbase.enumerate()) {
      TreeElement *ten = add_element(
          &legacy_te_.subtree, &object_.id, &pchan, &legacy_te_, TSE_POSE_CHANNEL, a);
      pchan.temp = static_cast<void *>(ten);

      if (!pchan.constraints.is_empty()) {
        // Object *target;
        TreeElement *tenla1 = add_element(
            &ten->subtree, &object_.id, nullptr, ten, TSE_CONSTRAINT_BASE, 0);
        // char *str;

        for (bConstraint &con : pchan.constraints) {
          add_element(&tenla1->subtree, &object_.id, &con, tenla1, TSE_CONSTRAINT, const_index);
          /* possible add all other types links? */
        }
        const_index++;
      }
    }
    /* make hierarchy */
    for (TreeElement *ten : legacy_te_.subtree) {
      TreeElement *par;
      TreeStoreElem *tselem = TREESTORE(ten);
      if (tselem->type == TSE_POSE_CHANNEL) {
        bPoseChannel *pchan = static_cast<bPoseChannel *>(ten->directdata);
        if (pchan->parent) {
          const int index = legacy_te_.subtree.first_index_of(ten);
          legacy_te_.subtree.remove(index);
          par = static_cast<TreeElement *>(pchan->parent->temp);
          par->subtree.append(ten);
          ten->parent = par;
        }
      }
    }
  }
}

/* -------------------------------------------------------------------- */

TreeElementPoseChannel::TreeElementPoseChannel(TreeElement &legacy_te,
                                               Object & /*object*/,
                                               bPoseChannel &pchan)
    : AbstractTreeElement(legacy_te), /* object_(object), */ pchan_(pchan)
{
  BLI_assert(legacy_te.store_elem->type == TSE_POSE_CHANNEL);
  legacy_te.name = pchan_.name;
  legacy_te.directdata = &pchan_;
}

}  // namespace ed::outliner
}  // namespace blender
