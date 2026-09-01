/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "MEM_guardedalloc.h"

#include "NOD_materialx.hh"
#include "NOD_socket_search_link.hh"

#include "BKE_appdir.hh"
#include "BKE_context.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_path_utils.hh"
#include "BLI_string.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "CLG_log.h"

#include "UI_interface_layout.hh"

#include "node_shader_util.hh"
#include "node_util.hh"

#ifdef WITH_MATERIALX
#  include <MaterialXCore/Definition.h>
#  include <MaterialXCore/Document.h>
#  include <MaterialXCore/Interface.h>
#  include <MaterialXCore/Node.h>
#  include <MaterialXCore/Types.h>

#  include <MaterialXFormat/File.h>
#  include <MaterialXFormat/Util.h>

namespace mx = MaterialX;

namespace blender {

void load_materialx_libraries(mx::DocumentPtr &lib, CLG_LogRef LOG)
{
  char materialx_path[FILE_MAX];
#  ifdef __APPLE__
  BLI_path_join(materialx_path,
                sizeof(materialx_path),
                BKE_appdir_program_dir(),
                "..",
                "Resources",
                "lib",
                "materialx");
#  else
  BLI_path_join(
      materialx_path, sizeof(materialx_path), BKE_appdir_program_dir(), "lib", "materialx");
#  endif

  BLI_path_normalize_native(materialx_path);

  mx::FileSearchPath search_path((mx::FilePath(materialx_path)));

  mx::FilePathVec library_folders;
  library_folders.emplace_back("libraries");
  search_path.append(mx::getEnvironmentPath());  // MATERIALX_SEARCH_PATH

  try {
    mx::loadLibraries(library_folders, search_path, lib);
  }
  catch (const mx::ExceptionParseError &e) {
    CLOG_ERROR(&LOG, "MaterialX parse error: %s", e.what());
    return;
  }
  catch (const mx::Exception &e) {
    CLOG_ERROR(&LOG, "MaterialX error: %s", e.what());
    return;
  }
  catch (const std::exception &e) {
    CLOG_ERROR(&LOG, "Unexpected exception: %s", e.what());
    return;
  }
  catch (...) {
    CLOG_ERROR(&LOG, "Unknown exception while loading MaterialX libraries");
    return;
  }
}

static int socket_type_from_materialx(const std::string &type, int &r_subtype, CLG_LogRef LOG)
{
  r_subtype = PROP_NONE;
  if (type == "vector2") {
    return SOCK_VECTOR2;
  }
  if (type == "vector3") {
    return SOCK_VECTOR;
  }
  if (type == "vector4") {
    return SOCK_VECTOR4;
  }
  if (type == "color3") {
    return SOCK_RGB;
  }
  if (type == "color4") {
    return SOCK_RGBA;
  }
  if (type == "float") {
    return SOCK_FLOAT;
  }
  if (type == "integer") {
    return SOCK_INT;
  }
  if (type == "boolean") {
    return SOCK_BOOLEAN;
  }
  if (type == mx::FILENAME_TYPE_STRING) {
    return SOCK_FILENAME;
  }
  if (type == mx::STRING_TYPE_STRING) {
    return SOCK_STRING;
  }
  if (type == "matrix33") {
    return SOCK_MATRIX3X3;
  }
  if (type == "matrix44") {
    return SOCK_MATRIX;
  }
  if (type == mx::SURFACE_SHADER_TYPE_STRING) {
    return SOCK_SURF_SHADER;
  }
  if (type == mx::VOLUME_SHADER_TYPE_STRING) {
    return SOCK_VOL_SHADER;
  }
  if (type == mx::DISPLACEMENT_SHADER_TYPE_STRING) {
    return SOCK_DISP_SHADER;
  }
  if (type == mx::LIGHT_SHADER_TYPE_STRING) {
    return SOCK_LIGHT_SHADER;
  }
  if (type == mx::BSDF_TYPE_STRING) {
    return SOCK_BSDF;
  }
  if (type == mx::EDF_TYPE_STRING) {
    return SOCK_EDF;
  }
  if (type == mx::VDF_TYPE_STRING) {
    return SOCK_VDF;
  }
  // socket material ignored for now
  // Since "geomname" isn't used by any of the standard
  // library nodes, skipping as well
  if (type == mx::MATERIAL_TYPE_STRING || type == mx::GEOMNAME_TYPE_STRING) {
    return SOCK_CUSTOM;
  }

  CLOG_WARN(&LOG, "Unknown MaterialX type '%s'", type.c_str());

  return SOCK_CUSTOM;
}

static std::string get_materialx_attribute(const mx::Element &element, const char *name)
{
  std::string attr;
  if (element.hasAttribute(name)) {
    attr = element.getAttribute(name);
  }
  return attr;
}

static std::string get_materialx_attribute(const mx::ElementPtr &element, const char *name)
{
  std::string attr;
  if (element) {
    attr = get_materialx_attribute(*element, name);
  }
  return attr;
}

MaterialXSocketMeta materialx_socket_meta_from_port(const mx::ValueElement &port, CLG_LogRef LOG)
{
  MaterialXSocketMeta sm;
  int sub = PROP_NONE;
  sm.name = port.getName();
  sm.socket.socket_type = socket_type_from_materialx(port.getType(), sub, LOG);
  sm.socket.subtype = sub;

  sm.ui_name = get_materialx_attribute(port, "uiname");
  sm.ui_folder = get_materialx_attribute(port, "uifolder");
  sm.doc_string = port.getDocString();
  sm.colorspace = get_materialx_attribute(port, "colorspace");
  sm.unittype = get_materialx_attribute(port, "unittype");
  sm.unit = get_materialx_attribute(port, "unit");
  sm.default_geomprop = get_materialx_attribute(port, "defaultgeomprop");
  sm.enum_items = get_materialx_attribute(port, "enum");
  sm.enum_values = get_materialx_attribute(port, "enumvalues");
  sm.ui_min = get_materialx_attribute(port, "uimin");
  sm.ui_max = get_materialx_attribute(port, "uimax");
  sm.ui_soft_min = get_materialx_attribute(port, "uisoftmin");
  sm.ui_soft_max = get_materialx_attribute(port, "uisoftmax");
  sm.ui_step = get_materialx_attribute(port, "uistep");
  {
    const std::string uniform = get_materialx_attribute(port, "uniform");
    sm.uniform = (uniform == "true" || uniform == "1" || uniform == "TRUE" || uniform == "True");
  }

  if (sm.socket.socket_type == SOCK_FLOAT) {
    float min_val = 0.0f, max_val = 0.0f;
    const std::string &min_str = !sm.ui_min.empty() ? sm.ui_min : sm.ui_soft_min;
    const std::string &max_str = !sm.ui_max.empty() ? sm.ui_max : sm.ui_soft_max;
    if (parse_materialx_float(min_str, min_val) && parse_materialx_float(max_str, max_val)) {
      if (min_val == 0.0f && max_val == 1.0f) {
        sm.socket.subtype = PROP_FACTOR;
      }
    }
  }

  if (const mx::ValuePtr value = port.getValue()) {
    sm.defaults.has_default = true;
    switch (sm.socket.socket_type) {
      case SOCK_FLOAT:
        sm.defaults.default_float4[0] = value->asA<float>();
        break;
      case SOCK_INT:
        sm.defaults.default_int = value->asA<int>();
        break;
      case SOCK_BOOLEAN:
        sm.defaults.default_bool = value->asA<bool>();
        break;
      case SOCK_VECTOR2: {
        const mx::Vector2 v = value->asA<mx::Vector2>();
        sm.defaults.default_float4 = {v[0], v[1], 0.0f, 0.0f};
        break;
      }
      case SOCK_VECTOR: {
        const mx::Vector3 v = value->asA<mx::Vector3>();
        sm.defaults.default_float4 = {v[0], v[1], v[2], 0.0f};
        break;
      }
      case SOCK_VECTOR4: {
        const mx::Vector4 v = value->asA<mx::Vector4>();
        sm.defaults.default_float4 = {v[0], v[1], v[2], v[3]};
        break;
      }
      case SOCK_RGB: {
        const mx::Color3 v = value->asA<mx::Color3>();
        sm.defaults.default_float4 = {v[0], v[1], v[2], 0.0f};
        break;
      }
      case SOCK_RGBA: {
        const mx::Color4 v = value->asA<mx::Color4>();
        sm.defaults.default_float4 = {v[0], v[1], v[2], v[3]};
        break;
      }
      case SOCK_STRING:
      case SOCK_FILENAME:
        sm.defaults.default_string = value->getValueString();
        break;
      default:
        sm.defaults.has_default = false;
        break;
    }
  }
  return sm;
}

void apply_materialx_default_value(bNodeSocket &sock, const MaterialXSocketMeta &meta)
{
  if (!meta.defaults.has_default) {
    return;
  }

  switch (sock.type) {
    case SOCK_FLOAT: {
      static_cast<bNodeSocketValueFloat *>(sock.default_value)->value =
          meta.defaults.default_float4[0];
      break;
    }
    case SOCK_INT: {
      static_cast<bNodeSocketValueInt *>(sock.default_value)->value = meta.defaults.default_int;
      break;
    }
    case SOCK_BOOLEAN: {
      static_cast<bNodeSocketValueBoolean *>(sock.default_value)->value =
          meta.defaults.default_bool;
      break;
    }
    case SOCK_VECTOR:
    case SOCK_VECTOR2:
    case SOCK_VECTOR4: {
      bNodeSocketValueVector *value = static_cast<bNodeSocketValueVector *>(sock.default_value);
      const int dimensions = value->dimensions > 0 ? value->dimensions : 4;
      for (int i = 0; i < dimensions; i++) {
        value->value[i] = meta.defaults.default_float4[i];
      }
      break;
    }
    case SOCK_RGB: {
      bNodeSocketValueRGB *value = static_cast<bNodeSocketValueRGB *>(sock.default_value);
      value->value[0] = meta.defaults.default_float4[0];
      value->value[1] = meta.defaults.default_float4[1];
      value->value[2] = meta.defaults.default_float4[2];
      break;
    }
    case SOCK_RGBA: {
      bNodeSocketValueRGBA *value = static_cast<bNodeSocketValueRGBA *>(sock.default_value);
      value->value[0] = meta.defaults.default_float4[0];
      value->value[1] = meta.defaults.default_float4[1];
      value->value[2] = meta.defaults.default_float4[2];
      value->value[3] = meta.defaults.default_float4[3];
      break;
    }
    case SOCK_STRING: {
      bNodeSocketValueFilename *default_value = static_cast<bNodeSocketValueFilename *>(
          sock.default_value);
      STRNCPY(default_value->value, meta.defaults.default_string.c_str());
      break;
    }
    case SOCK_FILENAME: {
      bNodeSocketValueString *default_value = static_cast<bNodeSocketValueString *>(
          sock.default_value);
      STRNCPY(default_value->value, meta.defaults.default_string.c_str());
      break;
    }
    default:
      break;
  }
}

void apply_materialx_socket_metadata(bNodeSocket &sock,
                                     const MaterialXSocketMeta &meta,
                                     const bool apply_defaults)
{
  /* The declaration already sets the socket name to ui_name (or the
   * internal name when ui_name is absent), so no label override is needed.
   * Only copy the description which is not propagated by the declaration. */
  STRNCPY(sock.description, meta.doc_string.c_str());

  if (sock.type == SOCK_FLOAT) {
    bNodeSocketValueFloat *value = static_cast<bNodeSocketValueFloat *>(sock.default_value);
    float min_value = value->min;
    float max_value = value->max;
    if (parse_materialx_float(!meta.ui_min.empty() ? meta.ui_min : meta.ui_soft_min, min_value)) {
      value->min = min_value;
    }
    if (parse_materialx_float(!meta.ui_max.empty() ? meta.ui_max : meta.ui_soft_max, max_value)) {
      value->max = max_value;
    }
  }
  else if (sock.type == SOCK_INT) {
    bNodeSocketValueInt *value = static_cast<bNodeSocketValueInt *>(sock.default_value);
    int min_value = value->min;
    int max_value = value->max;
    if (parse_materialx_int(!meta.ui_min.empty() ? meta.ui_min : meta.ui_soft_min, min_value)) {
      value->min = min_value;
    }
    if (parse_materialx_int(!meta.ui_max.empty() ? meta.ui_max : meta.ui_soft_max, max_value)) {
      value->max = max_value;
    }
  }

  if (apply_defaults) {
    apply_materialx_default_value(sock, meta);
  }
}
namespace {

CLG_LogRef LOG = {"node.shader.materialx"};

struct MaterialXVariantMeta {
  std::string suffix;    /* e.g. "float", "vector3" */
  int sock_type;         /* primary output socket type */
  std::string nodegroup; /* MaterialX nodegroup for this variant */
  std::string nodedef_name;
};

struct MaterialXGroupMeta {
  std::string doc_string;
  std::string nodegroup;
  std::string ui_name;
  std::vector<MaterialXVariantMeta> variants;
  std::vector<MaterialXSocketMeta> inputs;
  std::vector<MaterialXSocketMeta> outputs;
};

std::map<std::string, MaterialXGroupMeta> g_group_meta;

int find_variant_index(const MaterialXGroupMeta &meta, const char *suffix)
{
  for (int i = 0; i < int(meta.variants.size()); i++) {
    if (meta.variants[i].suffix == suffix) {
      return i;
    }
  }
  return -1;
}

int node_class_from_nodegroup(const std::string &nodegroup, const std::string &name)
{
  if (name == mx::SURFACE_MATERIAL_NODE_STRING || name == mx::VOLUME_MATERIAL_NODE_STRING) {
    return NODE_CLASS_OUTPUT;
  }

  /* Map MaterialX nodegroups to Blender NODE_CLASS_* values so the node bars have colors. */
  if (nodegroup == "pbr" || nodegroup == "shader" || nodegroup == "material" ||
      nodegroup == "light" || nodegroup == "npr")
  {
    return NODE_CLASS_SHADER;
  }
  if (nodegroup == "texture2d" || nodegroup == "texture3d" || nodegroup == "procedural" ||
      nodegroup == "procedural2d" || nodegroup == "procedural3d")
  {
    return NODE_CLASS_TEXTURE;
  }
  if (nodegroup == "adjustment" || nodegroup == "colortransform" || nodegroup == "compositing") {
    return NODE_CLASS_OP_COLOR;
  }
  if (nodegroup == "geometric" || nodegroup == "application") {
    return NODE_CLASS_INPUT;
  }
  if (nodegroup == "convolution2d") {
    return NODE_CLASS_OP_FILTER;
  }
  return NODE_CLASS_CONVERTER;
}

std::string get_variant_suffix(const mx::NodeDefPtr &nd)
{
  const std::string &full = nd->getName();
  const std::string &node_string = nd->getNodeString();
  if (node_string.empty()) {
    return full;
  }
  const std::string prefix = "ND_" + node_string + "_";
  if (full.size() > prefix.size() && full.compare(0, prefix.size(), prefix) == 0) {
    return full.substr(prefix.size());
  }
  return full;
}

std::string get_node_name(const bNode *node)
{
  const NodeShaderMaterialXGroup *storage = static_cast<const NodeShaderMaterialXGroup *>(
      node->storage);
  if (!storage || storage->name[0] == '\0') {
    return {};
  }
  return storage->name;
}

void sh_node_materialx_label(const bNodeTree * /*ntree*/,
                             const bNode *node,
                             char *label,
                             int label_maxncpy)
{
  const std::string node_name = get_node_name(node);
  if (node_name.empty()) {
    BLI_strncpy(label, "MaterialX", label_maxncpy);
    return;
  }

  auto it = g_group_meta.find(node_name);
  if (it == g_group_meta.end()) {
    BLI_strncpy(label, node_name.c_str(), label_maxncpy);
    return;
  }

  const MaterialXGroupMeta &meta = it->second;
  if (meta.ui_name.empty()) {
    BLI_strncpy(label, node_name.c_str(), label_maxncpy);
    return;
  }
  BLI_strncpy(label, meta.ui_name.c_str(), label_maxncpy);
}

int sh_node_materialx_ui_class(const bNode *node)
{
  const std::string node_name = get_node_name(node);
  if (node_name.empty()) {
    return NODE_CLASS_CONVERTER;
  }
  auto it = g_group_meta.find(node_name);
  if (it == g_group_meta.end()) {
    return NODE_CLASS_CONVERTER;
  }
  const MaterialXGroupMeta &meta = it->second;

  /* Use the active variant's nodegroup when it differs across variants. */
  const NodeShaderMaterialXGroup *storage = static_cast<const NodeShaderMaterialXGroup *>(
      node->storage);
  if (storage && !meta.variants.empty()) {
    const int active_idx = find_variant_index(meta, storage->active_variant);
    if (active_idx != -1) {
      const std::string &variant_ng = meta.variants[active_idx].nodegroup;
      if (!variant_ng.empty()) {
        return node_class_from_nodegroup(variant_ng, node_name);
      }
    }
    else if (storage->active_variant[0] != '\0') {
      CLOG_WARN(&LOG,
                "Stored variant '%s' on node '%s' no longer matches any variant of nodedef "
                "'%s'; the MaterialX library may have changed",
                storage->active_variant,
                node->name,
                node_name.c_str());
    }
  }

  /* Fall back to the group-level nodegroup. */
  return node_class_from_nodegroup(meta.nodegroup, node_name);
}

// Node callbacks
void sh_node_materialx_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeShaderMaterialXGroup *data = MEM_new<NodeShaderMaterialXGroup>(__func__);
  data->active_variant[0] = '\0';
  data->name[0] = '\0';
  node->storage = data;
}

/* Stable identifier: internal name + socket type, used for link migration. */
/* Encoding the SOCK_* enum value in the identifier makes every slot distinct
 * and prevents BLI_uniquename_cb from silently mangling duplicates
 * ("in1" -> "in1_001"), which would break link migration on every tree update. */
std::string make_sock_id(const MaterialXSocketMeta &sm)
{
  return sm.name + "__" + std::to_string(sm.socket.socket_type);
}

void sh_node_materialx_declare(nodes::NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }

  const std::string node_name = get_node_name(node);
  if (node_name.empty()) {
    b.add_default_layout();
    return;
  }

  auto it = g_group_meta.find(node_name);
  if (it == g_group_meta.end()) {
    CLOG_WARN(&LOG,
              "No MaterialX group metadata found for node category '%s' on node '%s'",
              node_name.c_str(),
              node->name);
    b.add_default_layout();
    return;
  }

  const MaterialXGroupMeta &meta = it->second;

  const NodeShaderMaterialXGroup *storage = static_cast<const NodeShaderMaterialXGroup *>(
      node->storage);
  const int num_variants = int(meta.variants.size());
  int active_idx = 0;
  if (num_variants > 0 && storage) {
    const int found_idx = find_variant_index(meta, storage->active_variant);
    if (found_idx >= 0) {
      active_idx = found_idx;
    }
    else if (storage->active_variant[0] != '\0') {
      CLOG_WARN(&LOG,
                "Stored variant '%s' on node '%s' does not match nodedef '%s'; falling back "
                "to the first variant",
                storage->active_variant,
                node->name,
                node_name.c_str());
    }
  }

  auto sock_display_name = [](const MaterialXSocketMeta &sm) -> std::string {
    return !sm.ui_name.empty() ? sm.ui_name : sm.name;
  };

  auto sock_panel_name = [](const MaterialXSocketMeta &sm) -> std::string {
    if (sm.ui_folder.empty()) {
      return {};
    }
    const std::size_t sep = sm.ui_folder.find('/');
    return sep != std::string::npos ? sm.ui_folder.substr(0, sep) : sm.ui_folder;
  };

  for (const MaterialXSocketMeta &sm : meta.outputs) {
    const bool in_variant = std::ranges::any_of(sm.socket.variant_indices.begin(),
                                                sm.socket.variant_indices.end(),
                                                [&](int vi) { return vi == active_idx; });
    b.add_output(eNodeSocketDatatype(sm.socket.socket_type),
                 UString(sock_display_name(sm)),
                 UString(make_sock_id(sm)))
        .description(sm.doc_string)
        .available(in_variant);
  }

  b.add_default_layout();

  auto add_input_for_socket = [&](auto &owner, const MaterialXSocketMeta &sm, bool in_variant) {
    const EnumPropertyItem *choices = sm.enum_display_data.enum_rna_items.empty() ?
                                          nullptr :
                                          sm.enum_display_data.enum_rna_items.data();
    const bool has_geomprop_default = !sm.default_geomprop.empty();
    const std::string name = sock_display_name(sm);
    const std::string id = make_sock_id(sm);

    switch (sm.socket.socket_type) {
      case SOCK_FLOAT: {
        auto &builder = owner.template add_input<nodes::decl::Float>(UString(name), UString(id));
        builder.description(sm.doc_string).available(in_variant);
        if (sm.defaults.has_default) {
          builder.default_value(sm.defaults.default_float4[0]);
        }
        if (choices) {
          builder.enum_choices(choices);
        }
        else if (sm.socket.subtype == PROP_FACTOR) {
          float ui_min_float = 0.0f, ui_max_float = 1.0f;
          parse_materialx_float(sm.ui_min, ui_min_float);
          parse_materialx_float(sm.ui_max, ui_max_float);
          builder.subtype(PROP_FACTOR).min(ui_min_float).max(ui_max_float);
        }
        if (has_geomprop_default) {
          builder.hide_value();
        }
        break;
      }
      case SOCK_INT: {
        auto &builder = owner.template add_input<nodes::decl::Int>(UString(name), UString(id));
        builder.description(sm.doc_string).available(in_variant);
        if (sm.defaults.has_default) {
          builder.default_value(sm.defaults.default_int);
        }
        if (has_geomprop_default) {
          builder.hide_value();
        }
        break;
      }
      case SOCK_BOOLEAN: {
        auto &builder = owner.template add_input<nodes::decl::Bool>(UString(name), UString(id));
        builder.description(sm.doc_string).available(in_variant);
        if (sm.defaults.has_default) {
          builder.default_value(sm.defaults.default_bool);
        }
        break;
      }
      case SOCK_VECTOR2: {
        auto &builder = owner.template add_input<nodes::decl::Vector2>(UString(name), UString(id));
        builder.description(sm.doc_string).available(in_variant);
        if (sm.defaults.has_default) {
          builder.default_value(
              {sm.defaults.default_float4[0], sm.defaults.default_float4[1]});  // x,y
        }
        if (has_geomprop_default) {
          builder.hide_value();
        }
        break;
      }
      case SOCK_VECTOR: {
        auto &builder = owner.template add_input<nodes::decl::Vector>(UString(name), UString(id));
        builder.description(sm.doc_string).available(in_variant);
        if (sm.defaults.has_default) {
          builder.default_value({sm.defaults.default_float4[0],
                                 sm.defaults.default_float4[1],
                                 sm.defaults.default_float4[2]});  // x,y,z
        }
        if (has_geomprop_default) {
          builder.hide_value();
        }
        break;
      }
      case SOCK_VECTOR4: {
        auto &builder = owner.template add_input<nodes::decl::Vector4>(UString(name), UString(id));
        builder.description(sm.doc_string).available(in_variant);
        if (sm.defaults.has_default) {
          builder.default_value({sm.defaults.default_float4[0],
                                 sm.defaults.default_float4[1],
                                 sm.defaults.default_float4[2],
                                 sm.defaults.default_float4[3]});  // x,y,z,w
        }
        if (has_geomprop_default) {
          builder.hide_value();
        }
        break;
      }
      case SOCK_RGB: {
        auto &builder = owner.template add_input<nodes::decl::Color3>(UString(name), UString(id));
        builder.description(sm.doc_string).available(in_variant);
        if (sm.defaults.has_default) {
          builder.default_value({sm.defaults.default_float4[0],
                                 sm.defaults.default_float4[1],
                                 sm.defaults.default_float4[2],
                                 1.0f});  // r,g,b
        }
        if (has_geomprop_default) {
          builder.hide_value();
        }
        break;
      }
      case SOCK_RGBA: {
        auto &builder = owner.template add_input<nodes::decl::Color>(UString(name), UString(id));
        builder.description(sm.doc_string).available(in_variant);
        if (sm.defaults.has_default) {
          builder.default_value({sm.defaults.default_float4[0],
                                 sm.defaults.default_float4[1],
                                 sm.defaults.default_float4[2],
                                 sm.defaults.default_float4[3]});  // r,g,b,a
        }
        if (has_geomprop_default) {
          builder.hide_value();
        }
        break;
      }
      case SOCK_STRING: {
        auto &builder = owner.template add_input<nodes::decl::String>(UString(name), UString(id));
        builder.description(sm.doc_string).available(in_variant);
        if (sm.defaults.has_default) {
          builder.default_value(sm.defaults.default_string);
        }
        if (choices) {
          builder.enum_choices(choices);
        }
        break;
      }
      case SOCK_FILENAME: {
        auto &builder = owner.template add_input<nodes::decl::Filename>(UString(name),
                                                                        UString(id));
        builder.description(sm.doc_string).available(in_variant);
        if (sm.defaults.has_default) {
          builder.default_value(sm.defaults.default_string);
        }
        if (choices) {
          builder.enum_choices(choices);
        }
        break;
      }
      default: {
        // Shader/BSDF/EDF/VDF/matrix types have no scalar default to seed.
        auto &builder = owner.add_input(
            eNodeSocketDatatype(sm.socket.socket_type), UString(name), UString(id));
        builder.description(sm.doc_string).available(in_variant);
        break;
      }
    }
  };

  for (const MaterialXSocketMeta &sm : meta.inputs) {
    if (!sock_panel_name(sm).empty()) {
      continue; /* handled below, inside panels */
    }
    const bool in_variant = std::ranges::any_of(sm.socket.variant_indices.begin(),
                                                sm.socket.variant_indices.end(),
                                                [&](int vi) { return vi == active_idx; });
    add_input_for_socket(b, sm, in_variant);
  }

  std::vector<std::string> panel_order;
  {
    std::set<std::string> seen;
    for (const MaterialXSocketMeta &sm : meta.inputs) {
      const std::string panel = sock_panel_name(sm);
      if (panel.empty()) {
        continue;
      }
      if (seen.insert(panel).second) {
        panel_order.push_back(panel);
      }
    }
  }

  for (const std::string &panel_name : panel_order) {
    nodes::PanelDeclarationBuilder &pb = b.add_panel(UString(panel_name)).default_closed(true);

    for (const MaterialXSocketMeta &sm : meta.inputs) {
      const std::string panel = sock_panel_name(sm);
      if (panel != panel_name) {
        continue;
      }
      const bool in_variant = std::ranges::any_of(sm.socket.variant_indices.begin(),
                                                  sm.socket.variant_indices.end(),
                                                  [&](int vi) { return vi == active_idx; });
      add_input_for_socket(pb, sm, in_variant);
    }
  }
}

void sh_node_materialx_update(bNodeTree *ntree, bNode *node)
{
  const std::string node_name = get_node_name(node);
  if (node_name.empty()) {
    return;
  }
  auto it = g_group_meta.find(node_name);
  if (it == g_group_meta.end()) {
    return;
  }
  const MaterialXGroupMeta &meta = it->second;

  NodeShaderMaterialXGroup *storage = static_cast<NodeShaderMaterialXGroup *>(node->storage);
  if (storage && !meta.variants.empty() && find_variant_index(meta, storage->active_variant) < 0) {
    CLOG_WARN(&LOG,
              "Node '%s' (nodedef '%s') had an invalid/empty active variant ('%s'); "
              "resetting to '%s'. This is expected once for files saved before the "
              "active_variant field became a string.",
              node->name,
              node_name.c_str(),
              storage->active_variant,
              meta.variants.front().suffix.c_str());
    STRNCPY(storage->active_variant, meta.variants.front().suffix.c_str());
  }

  /* Sever links connected to sockets not in the active variant. */
  bNodeLink *next_link = nullptr;
  for (bNodeLink *link = static_cast<bNodeLink *>(ntree->links.first()); link; link = next_link) {
    next_link = link->next;
    const bool from_unavail = (link->fromnode == node) && (link->fromsock->flag & SOCK_UNAVAIL);
    const bool to_unavail = (link->tonode == node) && (link->tosock->flag & SOCK_UNAVAIL);
    if (from_unavail || to_unavail) {
      bke::node_remove_link(ntree, *link);
    }
  }

  for (bNodeSocket *sock = static_cast<bNodeSocket *>(node->inputs.first()); sock != nullptr;
       sock = sock->next)
  {
    bool found = false;
    for (const MaterialXSocketMeta &sm : meta.inputs) {
      const std::string id = make_sock_id(sm);
      if (id == sock->identifier) {
        apply_materialx_socket_metadata(*sock, sm, false);
        found = true;
        break;
      }
    }
    if (!found) {
      CLOG_WARN(&LOG,
                "Socket '%s' on node '%s' has no matching input in nodedef '%s'",
                sock->identifier,
                node->name,
                node_name.c_str());
    }
  }
}

void sh_node_materialx_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  const bNode *node = ptr->data_as<bNode>();

  /* For testing ONLY */
  // layout.prop(ptr, "materialx_name", UI_ITEM_NONE, "Node", PROP_NONE);

  const std::string node_name = get_node_name(node);
  if (node_name.empty()) {
    return;
  }

  /* Only shown when the nodedef has multiple type variants. */
  auto it = g_group_meta.find(node_name);
  if (it == g_group_meta.end() || it->second.variants.size() <= 1) {
    return;
  }

  layout.prop(ptr, "active_variant", UI_ITEM_NONE, "Type", PROP_NONE);
}

bool materialx_node_poll(const bke::bNodeType * /*ntype*/,
                         const bNodeTree *ntree,
                         const char **r_disabled_hint)
{
  if (!STREQ(ntree->idname, "ShaderNodeTree")) {
    *r_disabled_hint = RPT_("Not a shader node tree");
    return false;
  }
  return true;
}

/* RNA get/set/update
 * active_variant: the nodedef type suffix
 * materialx_name: the nodedef base name stored in storage->name. */

int rna_materialx_active_variant_get(PointerRNA *ptr, PropertyRNA * /*prop*/)
{
  const bNode *node = ptr->data_as<bNode>();
  const NodeShaderMaterialXGroup *storage = static_cast<const NodeShaderMaterialXGroup *>(
      node->storage);
  if (!storage) {
    return 0;
  }
  auto it = g_group_meta.find(get_node_name(node));
  if (it == g_group_meta.end()) {
    return 0;
  }
  const int idx = find_variant_index(it->second, storage->active_variant);
  return idx >= 0 ? idx : 0;
}

void rna_materialx_active_variant_set(PointerRNA *ptr, PropertyRNA * /*prop*/, int value)
{
  bNode *node = ptr->data_as<bNode>();
  NodeShaderMaterialXGroup *storage = static_cast<NodeShaderMaterialXGroup *>(node->storage);
  if (!storage) {
    return;
  }
  auto it = g_group_meta.find(get_node_name(node));
  if (it == g_group_meta.end() || value < 0 || value >= int(it->second.variants.size())) {
    return;
  }
  STRNCPY(storage->active_variant, it->second.variants[value].suffix.c_str());
}

std::string rna_materialx_name_get(PointerRNA *ptr, PropertyRNA * /*prop*/)
{
  const bNode *node = ptr->data_as<bNode>();
  const NodeShaderMaterialXGroup *storage = static_cast<const NodeShaderMaterialXGroup *>(
      node->storage);
  return storage ? std::string(storage->name) : std::string();
}

int rna_materialx_name_length(PointerRNA *ptr, PropertyRNA * /*prop*/)
{
  const bNode *node = ptr->data_as<bNode>();
  const NodeShaderMaterialXGroup *storage = static_cast<const NodeShaderMaterialXGroup *>(
      node->storage);
  return storage ? int(strlen(storage->name)) : 0;
}

void rna_materialx_name_set(PointerRNA *ptr, PropertyRNA * /*prop*/, const std::string &value)
{
  bNode *node = ptr->data_as<bNode>();
  NodeShaderMaterialXGroup *storage = static_cast<NodeShaderMaterialXGroup *>(node->storage);
  if (!storage) {
    return;
  }
  STRNCPY(storage->name, value.c_str());

  auto it = g_group_meta.find(storage->name);
  if (it == g_group_meta.end()) {
    CLOG_WARN(&LOG, "No MaterialX nodedef found for '%s'", storage->name);
    storage->active_variant[0] = '\0';
    return;
  }

  /* Reset to the nodedef's first variant when the nodedef itself changes. */
  if (!it->second.variants.empty()) {
    STRNCPY(storage->active_variant, it->second.variants.front().suffix.c_str());
  }
  else {
    storage->active_variant[0] = '\0';
  }

  for (bNodeSocket *sock = static_cast<bNodeSocket *>(node->inputs.first()); sock != nullptr;
       sock = sock->next)
  {
    bool found = false;
    for (const MaterialXSocketMeta &sm : it->second.inputs) {
      const std::string id = make_sock_id(sm);
      if (id == sock->identifier) {
        apply_materialx_socket_metadata(*sock, sm, false);
        found = true;
        break;
      }
    }
    if (!found) {
      CLOG_WARN(&LOG,
                "Socket '%s' on node '%s' has no matching input in nodedef '%s'",
                sock->identifier,
                node->name,
                storage->name);
    }
  }
}

void rna_materialx_update(Main *bmain, Scene * /*scene*/, PointerRNA *ptr)
{
  bNodeTree *ntree = reinterpret_cast<bNodeTree *>(ptr->owner_id);
  bNode *node = ptr->data_as<bNode>();

  sh_node_materialx_update(ntree, node);

  BKE_ntree_update_tag_node_property(ntree, node);
  BKE_main_ensure_invariants(*bmain, ntree->id);
}

/* Returns the variants for whatever nodedef is currently
 * set on this node. RNA calls this at draw time to populate the dropdown. */
const EnumPropertyItem *rna_materialx_variant_itemf(bContext * /*C*/,
                                                    PointerRNA *ptr,
                                                    PropertyRNA * /*prop*/,
                                                    bool *r_free)
{
  *r_free = false;

  static const EnumPropertyItem fallback[] = {
      {0, "NONE", 0, "None", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  const bNode *node = ptr->data_as<bNode>();
  const std::string node_name = get_node_name(node);
  if (node_name.empty()) {
    return fallback;
  }

  auto it = g_group_meta.find(node_name);
  if (it == g_group_meta.end() || it->second.variants.empty()) {
    return fallback;
  }

  /* Build a heap-allocated array for this call. RNA will free it because
   * we set *r_free = true. */
  const MaterialXGroupMeta &meta = it->second;
  const int n = int(meta.variants.size());

  EnumPropertyItem *items = static_cast<EnumPropertyItem *>(
      MEM_new_array_zeroed(n + 1, sizeof(EnumPropertyItem), __func__));

  for (int i = 0; i < n; i++) {
    items[i].value = i;
    items[i].identifier = meta.variants[i].suffix.c_str();
    items[i].icon = 0;
    items[i].name = meta.variants[i].suffix.c_str();
    items[i].description = "";
  }
  /* Sentinel. */
  items[n] = {0, nullptr, 0, nullptr, nullptr};

  *r_free = true;
  return items;
}

void refine_factor_subtype(MaterialXSocketMeta &sm)
{
  if (sm.socket.socket_type != SOCK_FLOAT) {
    return;
  }
  float min_val = 0.0f, max_val = 0.0f;
  const std::string &min_str = !sm.ui_min.empty() ? sm.ui_min : sm.ui_soft_min;
  const std::string &max_str = !sm.ui_max.empty() ? sm.ui_max : sm.ui_soft_max;
  if (parse_materialx_float(min_str, min_val) && parse_materialx_float(max_str, max_val)) {
    if (min_val == 0.0f && max_val == 1.0f) {
      sm.socket.subtype = PROP_FACTOR;
    }
  }
}

void merge_materialx_socket_meta(MaterialXSocketMeta &dst, const MaterialXSocketMeta &src)
{
  if (dst.ui_name.empty()) {
    dst.ui_name = src.ui_name;
  }
  if (dst.ui_folder.empty()) {
    dst.ui_folder = src.ui_folder;
  }
  if (dst.doc_string.empty()) {
    dst.doc_string = src.doc_string;
  }
  if (dst.colorspace.empty()) {
    dst.colorspace = src.colorspace;
  }
  if (dst.unittype.empty()) {
    dst.unittype = src.unittype;
  }
  if (dst.unit.empty()) {
    dst.unit = src.unit;
  }
  if (dst.default_geomprop.empty()) {
    dst.default_geomprop = src.default_geomprop;
  }
  if (dst.enum_items.empty()) {
    dst.enum_items = src.enum_items;
  }
  if (dst.enum_values.empty()) {
    dst.enum_values = src.enum_values;
  }
  if (dst.ui_min.empty()) {
    dst.ui_min = src.ui_min;
  }
  if (dst.ui_max.empty()) {
    dst.ui_max = src.ui_max;
  }
  if (dst.ui_soft_min.empty()) {
    dst.ui_soft_min = src.ui_soft_min;
  }
  if (dst.ui_soft_max.empty()) {
    dst.ui_soft_max = src.ui_soft_max;
  }
  if (dst.ui_step.empty()) {
    dst.ui_step = src.ui_step;
  }
  if (!dst.uniform) {
    dst.uniform = src.uniform;
  }
  if (src.defaults.has_default) {
    dst.defaults = src.defaults;
  }
  refine_factor_subtype(dst);
}

void build_group_meta(const mx::DocumentPtr &lib)
{
  struct NodeGroup {
    std::string doc_string;
    std::string nodegroup;
    std::string ui_name;
    std::map<std::string, mx::NodeDefPtr> variants; /* suffix -> nodedef */
  };
  std::map<std::string, NodeGroup> grouped;

  for (const mx::NodeDefPtr &nd : lib->getNodeDefs()) {
    const std::string &node_string = nd->getNodeString();
    const std::string base = node_string.empty() ? nd->getName() : node_string;

    const std::string version = nd->getVersionString();

    if (!version.empty() && !nd->getDefaultVersion()) {
      continue;
    }

    NodeGroup &g = grouped[base];
    if (g.doc_string.empty()) {
      g.doc_string = nd->getDocString();
    }
    if (g.nodegroup.empty()) {
      g.nodegroup = get_materialx_attribute(nd, "nodegroup");
    }
    if (g.ui_name.empty()) {
      g.ui_name = get_materialx_attribute(nd, "uiname");
    }
    g.variants[get_variant_suffix(nd)] = nd;
  }

  for (auto &[base_name, group] : grouped) {
    MaterialXGroupMeta meta;
    meta.doc_string = group.doc_string;
    meta.nodegroup = group.nodegroup;
    meta.ui_name = group.ui_name;

    int variant_idx = 0;
    for (const auto &[suffix, nd] : group.variants) {
      int subtype = PROP_NONE;
      int primary_type = SOCK_FLOAT;
      if (!nd->getActiveOutputs().empty()) {
        primary_type = socket_type_from_materialx(
            nd->getActiveOutputs()[0]->getType(), subtype, LOG);
      }
      const std::string variant_nodegroup = get_materialx_attribute(nd, "nodegroup");
      meta.variants.push_back({suffix, primary_type, variant_nodegroup, nd->getName()});

      /* One socket slot per (name, type) pair per variant. */
      for (const mx::InputPtr &inp : nd->getActiveInputs()) {
        MaterialXSocketMeta port_meta = materialx_socket_meta_from_port(*inp, LOG);
        if (port_meta.socket.socket_type == SOCK_CUSTOM) {
          continue;
        }
        bool found = false;
        for (MaterialXSocketMeta &sm : meta.inputs) {
          if (sm.socket.socket_type == port_meta.socket.socket_type && sm.name == port_meta.name) {
            sm.socket.variant_indices.push_back(variant_idx);
            merge_materialx_socket_meta(sm, port_meta);
            found = true;
            break;
          }
        }
        if (!found) {
          port_meta.socket.variant_indices.push_back(variant_idx);
          meta.inputs.push_back(std::move(port_meta));
        }
      }

      for (const mx::OutputPtr &out : nd->getActiveOutputs()) {
        MaterialXSocketMeta port_meta = materialx_socket_meta_from_port(*out, LOG);
        if (port_meta.socket.socket_type == SOCK_CUSTOM) {
          continue;
        }
        bool found = false;
        for (MaterialXSocketMeta &sm : meta.outputs) {
          if (sm.socket.socket_type == port_meta.socket.socket_type && sm.name == port_meta.name) {
            sm.socket.variant_indices.push_back(variant_idx);
            merge_materialx_socket_meta(sm, port_meta);
            found = true;
            break;
          }
        }
        if (!found) {
          port_meta.socket.variant_indices.push_back(variant_idx);
          meta.outputs.push_back(std::move(port_meta));
        }
      }

      variant_idx++;
    }

    /* Build EnumPropertyItem arrays for inputs that have an "enum" attribute. */
    for (MaterialXSocketMeta &sm : meta.inputs) {
      if (sm.enum_items.empty()) {
        continue;
      }
      sm.enum_display_data.enum_display_names = split_materialx_list(sm.enum_items);
      const bool is_string_type = (sm.socket.socket_type == SOCK_STRING ||
                                   sm.socket.socket_type == SOCK_FILENAME);
      if (!is_string_type && !sm.enum_values.empty()) {
        sm.enum_display_data.enum_typed_values = split_materialx_list(sm.enum_values);
      }
      else {
        sm.enum_display_data.enum_typed_values = sm.enum_display_data.enum_display_names;
      }

      /* Pad typed_values to same length as display_names if necessary. */
      while (sm.enum_display_data.enum_typed_values.size() <
             sm.enum_display_data.enum_display_names.size())
      {
        sm.enum_display_data.enum_typed_values.push_back(
            sm.enum_display_data
                .enum_display_names[sm.enum_display_data.enum_typed_values.size()]);
      }

      sm.enum_display_data.enum_rna_items.reserve(sm.enum_display_data.enum_display_names.size() +
                                                  1);
      for (size_t i = 0; i < sm.enum_display_data.enum_display_names.size(); i++) {
        EnumPropertyItem item{};
        item.value = int(i);
        item.identifier = sm.enum_display_data.enum_typed_values[i].c_str();
        item.name = sm.enum_display_data.enum_display_names[i].c_str();
        item.description = "";
        sm.enum_display_data.enum_rna_items.push_back(item);
      }
      /* Null sentinel. */
      sm.enum_display_data.enum_rna_items.push_back({0, nullptr, 0, nullptr, nullptr});
    }

    g_group_meta[base_name] = std::move(meta);
  }
}

}  // namespace

void register_node_type_sh_materialx()
{
  using namespace nodes;

  mx::DocumentPtr lib = mx::createDocument();
  load_materialx_libraries(lib, LOG);
  build_group_meta(lib);

  static bke::bNodeType node_type;

  bke::node_type_base_custom(
      node_type, "ShaderNodeMaterialX", "MaterialX", "MaterialX", NODE_CLASS_CONVERTER);

  sh_node_type_base(&node_type, "ShaderNodeMaterialX"_ustr, SH_NODE_MATERIALX);

  node_type.rna_ext.srna = RNA_def_struct_ptr(
      &RNA_blender_rna_get(), "ShaderNodeMaterialX", RNA_Node);
  RNA_struct_blender_type_set(node_type.rna_ext.srna, &node_type);

  { /* active_variant: which type suffix is selected */
    PropertyRNA *prop = RNA_def_enum(node_type.rna_ext.srna,
                                     "active_variant",
                                     rna_enum_dummy_DEFAULT_items, /* overridden by itemf */
                                     0,
                                     "Type",
                                     "Active type variant for this MaterialX node");
    RNA_def_property_enum_funcs_runtime(prop,
                                        rna_materialx_active_variant_get,
                                        rna_materialx_active_variant_set,
                                        rna_materialx_variant_itemf,
                                        nullptr,
                                        nullptr);
    RNA_def_property_update_runtime(prop, rna_materialx_update);
  }

  {
    PropertyRNA *prop = RNA_def_string(node_type.rna_ext.srna,
                                       "materialx_name",
                                       nullptr,
                                       64, /* matches NodeShaderMaterialXGroup::name[64] */
                                       "MaterialX Node",
                                       "Name of the MaterialX nodedef this node represents");
    RNA_def_property_string_funcs_runtime(prop,
                                          rna_materialx_name_get,
                                          rna_materialx_name_length,
                                          rna_materialx_name_set,
                                          nullptr,
                                          nullptr);
    RNA_def_property_update_runtime(prop, rna_materialx_update);
  }

  node_type.ui_name = "MaterialX";
  node_type.ui_description = "MaterialX standard library node";
  node_type.nclass = NODE_CLASS_CONVERTER;
  node_type.poll = materialx_node_poll;
  node_type.labelfunc = sh_node_materialx_label;
  node_type.ui_class = sh_node_materialx_ui_class;
  node_type.insert_link = node_insert_link_default;
  node_type.gather_link_search_ops = nodes::search_link_ops_for_basic_node;
  node_type.enum_name_legacy = nullptr;

  bke::node_type_storage(node_type,
                         "NodeShaderMaterialXGroup",
                         node_free_standard_storage,
                         node_copy_standard_storage);

  node_type.initfunc = sh_node_materialx_init;
  node_type.updatefunc = sh_node_materialx_update;
  node_type.draw_buttons = sh_node_materialx_layout;
  node_type.declare = sh_node_materialx_declare;

  bke::node_register_type(node_type);
}

bool node_materialx_variant_for_type(const std::string &node_name,
                                     const mx::NodePtr &mx_node,
                                     const mx::DocumentPtr &lib,
                                     std::string &active_variant)
{
  const mx::NodeDefPtr nd = mx_node->getNodeDef("", true);
  if (!nd) {
    CLOG_WARN(&LOG, "MaterialX node '%s' has no nodedef", node_name.c_str());
    return false;
  }

  mx::InterfaceElementPtr impl = nd->getImplementation("");

  if (impl && impl->isA<mx::NodeGraph>()) {
    mx::GraphElementPtr graph_impl_in_lib = lib->getNodeGraph(impl->getName());
    mx::GraphElementPtr graph_impl_in_doc = mx_node->getDocument()->getNodeGraph(impl->getName());
    if (!graph_impl_in_lib && graph_impl_in_doc) {
      return false;
    }
  }

  auto it = g_group_meta.find(node_name);
  if (it == g_group_meta.end()) {
    CLOG_WARN(&LOG, "No MaterialX nodedef family found for '%s'", node_name.c_str());
    return false;
  }

  const std::vector<MaterialXVariantMeta> &variants = it->second.variants;
  const std::string suffix = get_variant_suffix(nd);

  for (const MaterialXVariantMeta &variant : variants) {
    if (variant.suffix == suffix) {
      active_variant = variant.suffix;
      return true;
    }
  }

  CLOG_WARN(&LOG,
            "No MaterialX variant of '%s' matches type '%s'",
            node_name.c_str(),
            nd->getType().c_str());
  return false;
}

bool node_materialx_type_for_variant(const std::string &base_name,
                                     const std::string &active_variant,
                                     std::string &mtlx_nodedef)
{
  auto it = g_group_meta.find(base_name);
  if (it == g_group_meta.end()) {
    CLOG_WARN(&LOG, "No MaterialX nodedef family found for '%s'", base_name.c_str());
    return false;
  }
  const std::vector<MaterialXVariantMeta> &variants = it->second.variants;

  for (const MaterialXVariantMeta &variant : variants) {
    if (variant.suffix == active_variant) {
      mtlx_nodedef = variant.nodedef_name;
      return true;
    }
  }

  if (variants.empty()) {
    CLOG_WARN(&LOG, "Nodedef family '%s' has no variants", base_name.c_str());
    return false;
  }

  if (!active_variant.empty()) {
    CLOG_WARN(&LOG,
              "No MaterialX variant '%s' found for '%s'; defaulting to '%s'",
              active_variant.c_str(),
              base_name.c_str(),
              variants.front().suffix.c_str());
  }
  else {
    CLOG_WARN(&LOG,
              "Node '%s' has no active variant set; defaulting to '%s'",
              base_name.c_str(),
              variants.front().suffix.c_str());
  }

  mtlx_nodedef = variants.front().nodedef_name;
  return true;
}

}  // namespace blender
#endif

namespace blender {

#ifndef WITH_MATERIALX
void register_node_type_sh_materialx()
{
  static bke::bNodeType node_type;

  bke::node_type_base_custom(
      node_type, "ShaderNodeMaterialX", "MaterialX", "MaterialX", NODE_CLASS_CONVERTER);

  sh_node_type_base(&node_type, "ShaderNodeMaterialX"_ustr, SH_NODE_MATERIALX);

  node_type.rna_ext.srna = RNA_def_struct_ptr(
      &RNA_blender_rna_get(), "ShaderNodeMaterialX", RNA_Node);
  RNA_struct_blender_type_set(node_type.rna_ext.srna, &node_type);

  node_type.ui_name = "MaterialX";
  node_type.ui_description = "MaterialX standard library node";
  node_type.nclass = NODE_CLASS_CONVERTER;
  node_type.insert_link = node_insert_link_default;
  node_type.gather_link_search_ops = nodes::search_link_ops_for_basic_node;
  node_type.enum_name_legacy = nullptr;

  bke::node_type_storage(node_type,
                         "NodeShaderMaterialXGroup",
                         node_free_standard_storage,
                         node_copy_standard_storage);

  bke::node_register_type(node_type);
}
#endif
bool parse_materialx_float(const std::string &value, float &r_value)
{
  if (value.empty()) {
    return false;
  }
  char *end = nullptr;
  const float parsed = std::strtof(value.c_str(), &end);
  if (end == value.c_str() || (end != nullptr && *end != '\0')) {
    return false;
  }
  r_value = parsed;
  return true;
}

bool parse_materialx_int(const std::string &value, int &r_value)
{
  if (value.empty()) {
    return false;
  }
  char *end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || (end != nullptr && *end != '\0')) {
    return false;
  }
  r_value = int(parsed);
  return true;
}

std::string format_materialx_float(float value)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", value);
  return buf;
}

std::vector<std::string> split_materialx_list(const std::string &value)
{
  std::vector<std::string> result;
  size_t start = 0;
  while (start <= value.size()) {
    const size_t comma = value.find(',', start);
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

bool is_materialx_output_node(const bNode &node)
{
  if (!node.is_type("ShaderNodeMaterialX"_ustr)) {
    return false;
  }
  const NodeShaderMaterialXGroup *storage = static_cast<const NodeShaderMaterialXGroup *>(
      node.storage);
  if (!storage) {
    return false;
  }
  bool is_surface = STREQ(storage->name, "surfacematerial");
  bool is_volume = STREQ(storage->name, "volumematerial");

  return is_surface || is_volume;
}

}  // namespace blender
