/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#pragma once

#include "tree_element_id.hh"

namespace blender {

struct Object;
struct Base;

namespace ed::outliner {

class TreeElementIDObject final : public TreeElementID {
  Object &object_;

 public:
  TreeElementIDObject(TreeElement &legacy_te, Object &object);

  void expand(SpaceOutliner & /*soops*/) const override;

  void *data() const override
  {
    return base_;
  }
  void set_data(void *data) override
  {
    base_ = static_cast<Base *>(data);
  }

 private:
  Base *base_;
  void expand_data() const;
  void expand_pose() const;
  void expand_materials() const;
  void expand_constraints() const;
  void expand_modifiers() const;
  void expand_gpencil_modifiers() const;
  void expand_gpencil_effects() const;
  void expand_vertex_groups() const;
  void expand_duplicated_group() const;
};

}  // namespace ed::outliner
}  // namespace blender
