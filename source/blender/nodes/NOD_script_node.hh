/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_node_types.h"

#include "RNA_access.hh"

#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_report.hh"

#ifdef WITH_OSL
#  include <OSL/oslquery.h>
#endif

namespace blender::nodes::node_shader_script_cc {

bool node_shader_script_rebuild_sockets(
    Main &bmain, bNodeTree &ntree, bNode &node, const char *oso_path, ReportList *reports);

}  // namespace blender::nodes::node_shader_script_cc
