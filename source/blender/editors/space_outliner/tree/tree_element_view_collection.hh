/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#pragma once

#include "tree_element.hh"

namespace blender {

struct Scene;

namespace ed::outliner {

class TreeElementViewCollectionBase final : public AbstractTreeElement {
  /* Not needed right now, avoid unused member variable warning. */
  // Scene &scene_;

 public:
  TreeElementViewCollectionBase(TreeElement &legacy_te, Scene &scene);
  void expand_context_menu(bContext *C) override;
};

}  // namespace ed::outliner
}  // namespace blender
