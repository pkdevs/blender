/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "DNA_node_types.h"
#include "RNA_types.hh"
#include "node_shader_util.hh"

#include "RNA_access.hh"

#include "MEM_guardedalloc.h"

#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_report.hh"

#include "BKE_main.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.hh"

#include "CLG_log.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#ifdef WITH_OSL
#  include <OSL/oslquery.h>
#endif

namespace blender {
namespace nodes::node_shader_script_cc {

static CLG_LogRef LOG = {"node.shader.script"};

namespace {

struct OSLParamMeta {
  std::string identifier;
  std::string label;
  std::string doc_string;
  std::string page;
  bool is_output = false;
  bool hide_value = false;
  int socket_type = SOCK_CUSTOM;
  int is_slider = false;
  bool is_filename = false;

  bool has_min = false;
  float ui_min_float = 0.0f;
  int ui_min_int = 0;

  bool has_max = false;
  float ui_max_float = 0.0f;
  int ui_max_int = 0;

  std::vector<std::string> enum_string_choices;
  std::vector<EnumPropertyItem> enum_choices;

  bool has_default = false;
  float default_float4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  int default_int = 0;
  bool default_bool = false;
  std::string default_string;
};

std::map<const bNode *, std::vector<OSLParamMeta>> g_osl_declared_params;

const std::vector<OSLParamMeta> &store_declared_params(const bNode *node,
                                                       std::vector<OSLParamMeta> params)
{
  return g_osl_declared_params[node] = std::move(params);
}

void clear_declared_params(const bNode *node)
{
  g_osl_declared_params.erase(node);
}

}  // namespace

#ifdef WITH_OSL

namespace {

int socket_type_from_osl(const OSL::TypeDesc &type, bool is_bool_widget)
{
  if (type.vecsemantics == OSL::TypeDesc::COLOR) {
    return SOCK_RGBA;
  }
  if (type.vecsemantics == OSL::TypeDesc::POINT || type.vecsemantics == OSL::TypeDesc::VECTOR ||
      type.vecsemantics == OSL::TypeDesc::NORMAL)
  {
    return SOCK_VECTOR;
  }
  if (type.aggregate == OSL::TypeDesc::SCALAR) {
    if (type.basetype == OSL::TypeDesc::INT) {
      return is_bool_widget ? SOCK_BOOLEAN : SOCK_INT;
    }
    if (type.basetype == OSL::TypeDesc::FLOAT) {
      return SOCK_FLOAT;
    }
    if (type.basetype == OSL::TypeDesc::STRING) {
      return SOCK_STRING;
    }
  }
  if (type.aggregate == OSL::TypeDesc::MATRIX44) {
    return SOCK_MATRIX;
  }
  return SOCK_CUSTOM;
}

std::string get_osl_metadata_string(const OSL::OSLQuery::Parameter &param, const char *name)
{
  for (const OSL::OSLQuery::Parameter &meta : param.metadata) {
    if (meta.name == name && !meta.sdefault.empty()) {
      return meta.sdefault[0].string();
    }
  }
  return {};
}

bool get_osl_metadata_int(const OSL::OSLQuery::Parameter &param, const char *name, int &r_value)
{
  for (const OSL::OSLQuery::Parameter &meta : param.metadata) {
    if (meta.name != name) {
      continue;
    }
    if (!meta.idefault.empty()) {
      r_value = meta.idefault[0];
      return true;
    }
  }
  return false;
}

bool get_osl_metadata_float(const OSL::OSLQuery::Parameter &param,
                            const char *name,
                            float &r_value)
{
  for (const OSL::OSLQuery::Parameter &meta : param.metadata) {
    if (meta.name != name) {
      continue;
    }
    if (!meta.fdefault.empty()) {
      r_value = meta.fdefault[0];
      return true;
    }
  }
  return false;
}

std::vector<std::string> split_list(const std::string &value)
{
  std::vector<std::string> result;
  size_t start = 0;
  while (start <= value.size()) {
    const size_t comma = value.find('|', start);
    const size_t end = (comma == std::string::npos) ? value.size() : comma;
    std::string token = value.substr(start, end - start);
    const size_t first = token.find_first_not_of(" \t");
    token = (first == std::string::npos) ?
                "" :
                token.substr(first, token.find_last_not_of(" \t") - first + 1);
    result.push_back(std::move(token));
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return result;
}

void populate_enum_choices(const std::string &raw_options_string, OSLParamMeta &param)
{
  param.enum_string_choices = split_list(raw_options_string);

  for (int i = 0; i < param.enum_string_choices.size(); ++i) {
    EnumPropertyItem item{};
    item.value = int(i);
    item.identifier = param.enum_string_choices[i].c_str();
    item.name = param.enum_string_choices[i].c_str();
    item.description = "";
    param.enum_choices.push_back(item);
  }
  /* Null sentinel. */
  param.enum_choices.push_back({0, nullptr, 0, nullptr, nullptr});
}

OSLParamMeta osl_param_meta_from_param(const OSL::OSLQuery::Parameter &param)
{
  OSLParamMeta pm;
  pm.identifier = param.name.string();
  pm.is_output = param.isoutput;
  pm.doc_string = get_osl_metadata_string(param, "help");
  pm.page = get_osl_metadata_string(param, "page");

  const std::string widget = get_osl_metadata_string(param, "widget");
  const bool is_bool = (widget == "boolean" || widget == "checkBox");
  const std::string label = get_osl_metadata_string(param, "label");
  pm.label = label.empty() ? pm.identifier : label;
  pm.hide_value = (widget == "null");
  pm.is_filename = (widget == "filename");

  get_osl_metadata_int(param, "slider", pm.is_slider);

  if (param.isclosure) {
    pm.socket_type = SOCK_SHADER;
  }
  else {
    pm.socket_type = socket_type_from_osl(param.type, is_bool);
  }

  if (pm.socket_type == SOCK_INT) {
    pm.has_min = get_osl_metadata_int(param, "min", pm.ui_min_int);
    pm.has_max = get_osl_metadata_int(param, "max", pm.ui_max_int);
  }
  else {
    pm.has_min = get_osl_metadata_float(param, "min", pm.ui_min_float);
    pm.has_max = get_osl_metadata_float(param, "max", pm.ui_max_float);
  }

  if (widget == "popup") {
    const std::string options = get_osl_metadata_string(param, "options");
    populate_enum_choices(options, pm);
  }

  switch (pm.socket_type) {
    case SOCK_FLOAT:
      if (!param.fdefault.empty()) {
        pm.has_default = true;
        pm.default_float4[0] = param.fdefault[0];
      }
      break;
    case SOCK_INT:
    case SOCK_BOOLEAN:
      if (!param.idefault.empty()) {
        pm.has_default = true;
        pm.default_int = param.idefault[0];
        pm.default_bool = param.idefault[0] != 0;
      }
      break;
    case SOCK_VECTOR:
      if (param.fdefault.size() >= 3) {
        pm.has_default = true;
        pm.default_float4[0] = param.fdefault[0];
        pm.default_float4[1] = param.fdefault[1];
        pm.default_float4[2] = param.fdefault[2];
      }
      break;
    case SOCK_RGBA:
      if (param.fdefault.size() >= 3) {
        pm.has_default = true;
        pm.default_float4[0] = param.fdefault[0];
        pm.default_float4[1] = param.fdefault[1];
        pm.default_float4[2] = param.fdefault[2];
        pm.default_float4[3] = 1.0f;
      }
      break;
    case SOCK_STRING:
      if (!param.sdefault.empty()) {
        pm.has_default = true;
        pm.default_string = param.sdefault[0].string();
      }
      break;
    default:
      break;
  }

  return pm;
}

/* Shared by all query entry points below: pull params out of an
 * already-opened OSLQuery. */
void extract_params_from_query(OSL::OSLQuery &query, std::vector<OSLParamMeta> &r_params)
{
  r_params.clear();
  r_params.reserve(query.nparams());
  for (size_t i = 0; i < query.nparams(); i++) {
    const OSL::OSLQuery::Parameter *param = query.getparam(i);
    /* For now, skip params we can't represent as a single typed socket. */
    if (!param || param->varlenarray || param->type.arraylen > 1 || param->isstruct) {
      continue;
    }
    r_params.push_back(osl_param_meta_from_param(*param));
  }
}

bool read_text_file(const std::string &path, std::string &r_content)
{
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  std::ostringstream buffer_stream;
  buffer_stream << file.rdbuf();
  r_content = buffer_stream.str();
  return !r_content.empty();
}

bool query_osl_params_from_bytecode(const std::string &bytecode,
                                    std::vector<OSLParamMeta> &r_params)
{
  OSL::OSLQuery query;
  if (!query.open_bytecode(bytecode)) {
    return false;
  }
  extract_params_from_query(query, r_params);
  return true;
}

}  // namespace

#endif /* WITH_OSL */

static void node_shader_script_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  if (!node) {
    b.add_default_layout();
    return;
  }

#ifdef WITH_OSL
  const NodeShaderScript *nss = static_cast<const NodeShaderScript *>(node->storage);
  std::vector<OSLParamMeta> params;
  if (!nss || !nss->bytecode || nss->bytecode[0] == '\0' ||
      !query_osl_params_from_bytecode(nss->bytecode, params))
  {
    clear_declared_params(node);
    b.add_default_layout();
    return;
  }
  const std::vector<OSLParamMeta> &stored_params = store_declared_params(node, std::move(params));
#else
  b.add_default_layout();
  return;
#endif

  b.use_custom_socket_order();

  auto add_input_for_param = [&](auto &owner, const OSLParamMeta &p) {
    const EnumPropertyItem *choices = p.enum_choices.empty() ? nullptr : p.enum_choices.data();

    switch (p.socket_type) {
      case SOCK_FLOAT: {
        auto &builder = owner.template add_input<decl::Float>(UString(p.label),
                                                              UString(p.identifier));
        builder.description(p.doc_string);
        if (p.has_default) {
          builder.default_value(p.default_float4[0]);
        }
        if (p.hide_value) {
          builder.hide_value();
        }
        if (p.has_min) {
          builder.min(p.ui_min_float);
        }
        if (p.has_max) {
          builder.max(p.ui_max_float);
        }
        if (p.is_slider) {
          builder.subtype(PROP_FACTOR);
        }
        break;
      }
      case SOCK_INT: {
        auto &builder = owner.template add_input<decl::Int>(UString(p.label),
                                                            UString(p.identifier));
        builder.description(p.doc_string);
        if (p.has_default) {
          builder.default_value(p.default_int);
        }
        if (p.hide_value) {
          builder.hide_value();
        }
        if (p.has_min) {
          builder.min(int(p.ui_min_int));
        }
        if (p.has_max) {
          builder.max(int(p.ui_max_int));
        }
        if (p.is_slider) {
          builder.subtype(PROP_FACTOR);
        }
        break;
      }
      case SOCK_BOOLEAN: {
        auto &builder = owner.template add_input<decl::Bool>(UString(p.label),
                                                             UString(p.identifier));
        builder.description(p.doc_string);
        if (p.hide_value) {
          builder.hide_value();
        }
        if (p.has_default) {
          builder.default_value(p.default_bool);
        }
        break;
      }
      case SOCK_VECTOR: {
        auto &builder = owner.template add_input<decl::Vector>(UString(p.label),
                                                               UString(p.identifier));
        builder.description(p.doc_string);
        if (p.has_default) {
          builder.default_value({p.default_float4[0], p.default_float4[1], p.default_float4[2]});
        }
        if (p.hide_value) {
          builder.hide_value();
        }
        if (p.has_min) {
          builder.min(p.ui_min_float);
        }
        if (p.has_max) {
          builder.max(p.ui_max_float);
        }
        if (p.is_slider) {
          builder.subtype(PROP_FACTOR);
        }
        break;
      }
      case SOCK_RGBA: {
        auto &builder = owner.template add_input<decl::Color>(UString(p.label),
                                                              UString(p.identifier));
        builder.description(p.doc_string);
        if (p.hide_value) {
          builder.hide_value();
        }
        if (p.has_default) {
          builder.default_value({p.default_float4[0],
                                 p.default_float4[1],
                                 p.default_float4[2],
                                 p.default_float4[3]});
        }
        break;
      }
      case SOCK_STRING: {
        auto &builder = owner.template add_input<decl::String>(UString(p.label),
                                                               UString(p.identifier));
        builder.description(p.doc_string);
        if (p.has_default) {
          builder.default_value(p.default_string);
        }
        if (p.hide_value) {
          builder.hide_value();
        }
        if (choices) {
          builder.enum_choices(choices);
        }
        if (p.is_filename) {
          builder.subtype(PROP_FILEPATH);
        }
        break;
      }
      case SOCK_MATRIX: {
        auto &builder = owner.template add_input<decl::Matrix>(UString(p.label),
                                                               UString(p.identifier));
        builder.description(p.doc_string);
        break;
      }
      default: {
        owner
            .add_input(eNodeSocketDatatype(p.socket_type), UString(p.label), UString(p.identifier))
            .description(p.doc_string);
        break;
      }
    }
  };

  /* Inputs are bucketed by page: unpaged ones stay flat, paged
   * ones are grouped into one panel per page, in the order each
   * page first appears. */
  std::vector<const OSLParamMeta *> flat_inputs;
  std::vector<std::string> page_order;
  std::map<std::string, std::vector<const OSLParamMeta *>> paged_inputs;

  for (const OSLParamMeta &p : stored_params) {
    if (p.is_output) {
      b.add_output(eNodeSocketDatatype(p.socket_type), UString(p.label), UString(p.identifier))
          .description(p.doc_string);
      continue;
    }
    if (p.page.empty()) {
      flat_inputs.push_back(&p);
    }
    else {
      if (paged_inputs.find(p.page) == paged_inputs.end()) {
        page_order.push_back(p.page);
      }
      paged_inputs[p.page].push_back(&p);
    }
  }

  b.add_default_layout();

  for (const OSLParamMeta *p : flat_inputs) {
    add_input_for_param(b, *p);
  }

  for (const std::string &page : page_order) {
    PanelDeclarationBuilder &pb = b.add_panel(UString(page)).default_closed(true);
    for (const OSLParamMeta *p : paged_inputs[page]) {
      add_input_for_param(pb, *p);
    }
  }
}

static void node_shader_buts_script(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  {
    ui::Layout &row = layout.row(false);
    row.prop(
        ptr, "mode", ui::ITEM_R_SPLIT_EMPTY_NAME | ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  }

  {
    ui::Layout &row = layout.row(true);
    if (RNA_enum_get(ptr, "mode") == NODE_SCRIPT_INTERNAL) {
      row.prop(ptr, "script", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    }
    else {
      row.prop(ptr, "filepath", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
    }
    row.op("node.shader_script_update", "", ICON_FILE_REFRESH);
  }
}

static void node_shader_buts_script_ex(ui::Layout &layout, bContext *C, PointerRNA *ptr)
{
  layout.separator();

  node_shader_buts_script(layout, C, ptr);
}

static void init(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderScript *nss = MEM_new<NodeShaderScript>("shader script node");
  node->storage = nss;
}

static void node_free_script(bNode *node)
{
  NodeShaderScript *nss = static_cast<NodeShaderScript *>(node->storage);

  if (nss) {
    if (nss->bytecode) {
      MEM_delete(nss->bytecode);
    }

    MEM_delete(nss);
  }
  clear_declared_params(node);
}

static void node_copy_script(bNodeTree * /*dst_ntree*/, bNode *dest_node, const bNode *src_node)
{
  NodeShaderScript *src_nss = static_cast<NodeShaderScript *>(src_node->storage);
  NodeShaderScript *dest_nss = MEM_dupalloc(src_nss);

  if (src_nss->bytecode) {
    dest_nss->bytecode = MEM_dupalloc(src_nss->bytecode);
  }

  dest_node->storage = dest_nss;
}

/* Public entry point for the updates. */
bool node_shader_script_rebuild_sockets(
    Main &bmain, bNodeTree &ntree, bNode &node, const char *external_oso_path, ReportList *reports)
{
#ifndef WITH_OSL
  BKE_report(reports, RPT_ERROR, "Blender was compiled without OSL support");
  return false;
#else
  NodeShaderScript *nss = static_cast<NodeShaderScript *>(node.storage);

  if (!nss) {
    BKE_report(reports, RPT_ERROR, "No OSL bytecode available");
    return false;
  }

  std::vector<OSLParamMeta> params;
  bool ok = false;

  if (nss->mode == NODE_SCRIPT_INTERNAL) {
    if (!nss->bytecode || nss->bytecode[0] == '\0') {
      BKE_report(reports, RPT_ERROR, "No OSL bytecode available");
      return false;
    }
    ok = query_osl_params_from_bytecode(nss->bytecode, params);
  }
  else {
    if (!external_oso_path || external_oso_path[0] == '\0') {
      BKE_report(reports, RPT_ERROR, "No OSL shader file available");
      return false;
    }
    std::string bytecode;
    if (!read_text_file(external_oso_path, bytecode)) {
      BKE_report(reports, RPT_ERROR, "Could not read OSL shader file");
      return false;
    }
    if (nss->bytecode) {
      MEM_delete(nss->bytecode);
    }
    nss->bytecode = BLI_strdupn(bytecode.c_str(), bytecode.size());
    ok = query_osl_params_from_bytecode(nss->bytecode, params);
  }

  if (!ok) {
    BKE_report(reports, RPT_ERROR, "OSL query failed");
    clear_declared_params(&node);
    BKE_ntree_update_tag_node_property(&ntree, &node);
    BKE_main_ensure_invariants(bmain, ntree.id);
    return false;
  }

  store_declared_params(&node, std::move(params));
  BKE_ntree_update_tag_node_property(&ntree, &node);
  BKE_main_ensure_invariants(bmain, ntree.id);
  return true;
#endif
}

}  // namespace nodes::node_shader_script_cc

void register_node_type_sh_script()
{
  namespace file_ns = nodes::node_shader_script_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeScript"_ustr, SH_NODE_SCRIPT);
  ntype.ui_name = "Script";
  ntype.ui_description =
      "Generate an OSL shader from a file or text data-block.\nNote: OSL shaders are not "
      "supported on all GPU backends";
  ntype.enum_name_legacy = "SCRIPT";
  ntype.nclass = NODE_CLASS_SCRIPT;
  ntype.draw_buttons = file_ns::node_shader_buts_script;
  ntype.draw_buttons_ex = file_ns::node_shader_buts_script_ex;
  ntype.initfunc = file_ns::init;
  ntype.declare = file_ns::node_shader_script_declare;
  bke::node_type_storage(
      ntype, "NodeShaderScript", file_ns::node_free_script, file_ns::node_copy_script);

  bke::node_register_type(ntype);
}

}  // namespace blender
