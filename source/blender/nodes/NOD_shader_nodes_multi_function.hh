/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "FN_user_data.hh"

#include "NOD_geometry_nodes_warning.hh"

namespace blender::nodes {

class ShaderNodesMultiFunctionUserData : public fn::UserData {
 public:
  struct Warning {
    NodeWarningType type;
    std::string message;
  };

  Vector<Warning> warnings;
};

}  // namespace blender::nodes
