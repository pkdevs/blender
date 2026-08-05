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
struct ViewLayer;

namespace ed::outliner {

class TreeElementViewLayerBase final : public AbstractTreeElement {
  Scene &scene_;

 public:
  TreeElementViewLayerBase(TreeElement &legacy_te, Scene &scene);

  void expand(SpaceOutliner & /*soops*/) const override;

  std::optional<BIFIconID> get_icon() const override
  {
    return ICON_RENDERLAYERS;
  }
};

class TreeElementViewLayer final : public AbstractTreeElement {
  /* Not needed right now, avoid unused member variable warning. */
  // Scene &scene_;
  ViewLayer &view_layer_;

 public:
  TreeElementViewLayer(TreeElement &legacy_te, Scene &scene, ViewLayer &view_layer);

  std::optional<BIFIconID> get_icon() const override
  {
    return ICON_RENDER_RESULT;
  }

  void *data() const override
  {
    return &view_layer_;
  }

  void set_data(void *data) override
  {
    view_layer_ = *static_cast<ViewLayer *>(data);
  }
};

}  // namespace ed::outliner
}  // namespace blender
