/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string>
#include <vector>

#include "DNA_node_types.h"
#include "RNA_types.hh"

struct CLG_LogRef;

#ifdef WITH_MATERIALX
#  include <MaterialXCore/Document.h>
#  include <MaterialXCore/Node.h>

namespace blender {
namespace mx = MaterialX;

void load_materialx_libraries(mx::DocumentPtr &lib, CLG_LogRef LOG);

struct MaterialXSocketMeta {
  struct MaterialXDefault {
    std::string default_string;
    std::array<float, 4> default_float4;
    int default_int;
    bool default_bool = false;
    bool has_default = false;
  };

  struct MaterialXEnumDisplayData {
    std::vector<std::string> enum_display_names;
    std::vector<std::string> enum_typed_values;
    std::vector<EnumPropertyItem> enum_rna_items;
  };

  struct RNASocketData {
    int socket_type; /* e.g. SOCK_VECTOR */
    int subtype;     /* e.g. PROP_NONE */
    std::vector<int> variant_indices;
  };

  std::string name;
  std::string ui_name;
  std::string ui_folder;
  std::string doc_string;
  std::string colorspace;
  std::string unittype;
  std::string unit;
  std::string default_geomprop;
  std::string enum_items;
  std::string enum_values;
  std::string ui_min;
  std::string ui_max;
  std::string ui_soft_min;
  std::string ui_soft_max;
  std::string ui_step;
  bool uniform = false;

  MaterialXDefault defaults;
  RNASocketData socket;
  MaterialXEnumDisplayData enum_display_data;
};

MaterialXSocketMeta materialx_socket_meta_from_port(const mx::ValueElement &port, CLG_LogRef LOG);

void apply_materialx_default_value(bNodeSocket &sock, const MaterialXSocketMeta &meta);

void apply_materialx_socket_metadata(bNodeSocket &sock,
                                     const MaterialXSocketMeta &meta,
                                     const bool apply_defaults);

bool node_materialx_variant_for_type(const std::string &node_name,
                                     const mx::NodePtr &mx_node,
                                     const mx::DocumentPtr &lib,
                                     std::string &active_variant);

bool node_materialx_type_for_variant(const std::string &base_name,
                                     const std::string &active_variant,
                                     std::string &mtlx_nodedef);

}  // namespace blender
#endif

namespace blender {

bool parse_materialx_float(const std::string &value, float &r_value);
bool parse_materialx_int(const std::string &value, int &r_value);
std::string format_materialx_float(float value);
std::vector<std::string> split_materialx_list(const std::string &value);

bool is_materialx_output_node(const bNode &node);

}  // namespace blender
