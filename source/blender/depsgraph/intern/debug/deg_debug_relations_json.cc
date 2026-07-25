/* SPDX-FileCopyrightText: 2014 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 *
 * Implementation of tools for debugging the depsgraph
 */

#include <cstdarg>

#include "BLI_stack.hh"

#include "BKE_idtype.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_debug.hh"

#include "intern/depsgraph.hh"
#include "intern/depsgraph_relation.hh"

#include "intern/node/deg_node_component.hh"
#include "intern/node/deg_node_id.hh"
#include "intern/node/deg_node_operation.hh"
#include "intern/node/deg_node_time.hh"

#include <sstream>
#include <variant>

namespace blender {

/* ****************** */
/* Graphviz Debugging */

namespace deg {

class JSONStream {
 private:
  std::stringstream ss_;
  Stack<int> items_;

 public:
  std::string str() const
  {
    return ss_.str();
  }

  JSONStream &indent()
  {
    for ([[maybe_unused]] const int i : IndexRange(items_.size())) {
      ss_ << "  ";
    }
    return *this;
  }

  JSONStream &object_begin()
  {
    ss_ << "{" << std::endl;
    items_.push(0);
    return *this;
  }

  JSONStream &object_end()
  {
    ss_ << std::endl;
    items_.pop();
    this->indent();
    ss_ << "}";
    return *this;
  }

  JSONStream &object_key(const StringRef key)
  {
    BLI_assert(!items_.is_empty());
    if (items_.peek() > 0) {
      ss_ << "," << std::endl;
    }
    this->indent();
    ss_ << "\"" << key << "\"" << ": ";
    ++items_.peek();
    return *this;
  }

  JSONStream &array_begin()
  {
    ss_ << "[" << std::endl;
    this->indent();
    items_.push(0);
    return *this;
  }

  JSONStream &array_end()
  {
    ss_ << std::endl;
    items_.pop();
    this->indent();
    ss_ << "]";
    return *this;
  }

  JSONStream &array_item()
  {
    BLI_assert(!items_.is_empty());
    if (items_.peek() > 0) {
      ss_ << "," << std::endl;
    }
    this->indent();
    ++items_.peek();
    return *this;
  }

  JSONStream &value(const StringRef value)
  {
    /* No careful escaping here, we just expect names to not include quotes. */
    ss_ << "\"" << value << "\"";
    return *this;
  }

  JSONStream &value(const char *value)
  {
    return this->value(std::string(value));
  }

  JSONStream &value(const int value)
  {
    ss_ << std::to_string(value);
    return *this;
  }

  JSONStream &value(const unsigned int value)
  {
    ss_ << std::to_string(value);
    return *this;
  }

  JSONStream &value(const float value)
  {
    ss_ << std::to_string(value);
    return *this;
  }

  JSONStream &value(const bool value)
  {
    ss_ << (value ? "true" : "false");
    return *this;
  }
};

/* The variants of nodes handled by the code. */
using ConstNodeVariant = std::
    variant<const IDNode *, const TimeSourceNode *, const ComponentNode *, const OperationNode *>;

static ConstNodeVariant get_node_variant(const Node &node)
{
  switch (node.type) {
    case NodeType::ID_REF:
      return {static_cast<const IDNode *>(&node)};

    case NodeType::PARAMETERS:
    case NodeType::ANIMATION:
    case NodeType::TRANSFORM:
    case NodeType::GEOMETRY:
    case NodeType::SEQUENCER:
    case NodeType::COMPOSITOR:
    case NodeType::EVAL_POSE:
    case NodeType::BONE:
    case NodeType::SHADING:
    case NodeType::CACHE:
    case NodeType::POINT_CACHE:
    case NodeType::IMAGE_ANIMATION:
    case NodeType::LAYER_COLLECTIONS:
    case NodeType::PARTICLE_SYSTEM:
    case NodeType::PARTICLE_SETTINGS:
    case NodeType::COPY_ON_EVAL:
    case NodeType::OBJECT_FROM_LAYER:
    case NodeType::HIERARCHY:
    case NodeType::BATCH_CACHE:
    case NodeType::INSTANCING:
    case NodeType::SYNCHRONIZATION:
    case NodeType::AUDIO:
    case NodeType::ARMATURE:
    case NodeType::GENERIC_DATABLOCK:
    case NodeType::SCENE:
    case NodeType::VISIBILITY:
    case NodeType::NTREE_OUTPUT:
    case NodeType::NTREE_GEOMETRY_PREPROCESS:
      return {static_cast<const ComponentNode *>(&node)};

    case NodeType::OPERATION:
      return static_cast<const OperationNode *>(&node);

    case NodeType::UNDEFINED:
    case NodeType::TIMESOURCE:
      return static_cast<const TimeSourceNode *>(&node);

    case NodeType::NUM_TYPES:
      BLI_assert_unreachable();
      break;
  }
  BLI_assert_unreachable();
  return {};
}

static void deg_debug_json_node_recursive(JSONStream &json,
                                          const Node &node,
                                          const bool is_entry = false,
                                          const bool is_exit = false)
{
  const ConstNodeVariant node_variant = get_node_variant(node);

  json.object_begin();

  json.object_key("type");
  json.value(nodeTypeAsString(node.type));

  if (const auto *const *id_node = std::get_if<const IDNode *>(&node_variant)) {
    json.object_key("id_type");
    json.value(BKE_idtype_idcode_to_name(GS((*id_node)->id_orig->name)));
    json.object_key("id_name");
    json.value((*id_node)->id_orig->name + 2);
    json.object_key("uid");
    json.value((*id_node)->id_orig_session_uid);
    json.object_key("components");
    json.array_begin();
    for (const auto &item : (*id_node)->components.items()) {
      /* Note: name part of the key is just the node name, never used by component nodes. */
      json.array_item();
      deg_debug_json_node_recursive(json, *item.value);
    }
    json.array_end();
  }
  if (const auto *const *time_node = std::get_if<const TimeSourceNode *>(&node_variant)) {
  }
  if (const auto *const *comp_node = std::get_if<const ComponentNode *>(&node_variant)) {
    json.object_key("operations");
    json.array_begin();
    for (const auto *op_node : (*comp_node)->operations) {
      const bool is_entry = ((*comp_node)->entry_operation == op_node);
      const bool is_exit = ((*comp_node)->exit_operation == op_node);
      json.array_item();
      deg_debug_json_node_recursive(json, *op_node, is_entry, is_exit);
    }
    json.array_end();
  }
  if (const auto *const *op_node = std::get_if<const OperationNode *>(&node_variant)) {
    json.object_key("name");
    json.value(node.name);
    json.object_key("is_entry");
    json.value(is_entry);
    json.object_key("is_exit");
    json.value(is_exit);
    json.object_key("is_noop");
    json.value((*op_node)->is_noop());
    json.object_key("affects_visibility");
    json.value(bool((*op_node)->flag & DEPSOP_FLAG_AFFECTS_VISIBILITY));
  }

  json.object_end();
}

static void deg_debug_json_node_identifier(JSONStream &json, const Node &node)
{

  json.object_begin();
  json.object_key("type");
  json.value(nodeTypeAsString(node.type));

  const ConstNodeVariant node_variant = get_node_variant(node);
  if (const auto *const *id_node = std::get_if<const IDNode *>(&node_variant)) {
    json.object_key("uid");
    json.value((*id_node)->id_orig_session_uid);
  }
  if (const auto *const *time_node = std::get_if<const TimeSourceNode *>(&node_variant)) {
    json.object_key("name");
    json.value((*time_node)->name);
  }
  if (const auto *const *comp_node = std::get_if<const ComponentNode *>(&node_variant)) {
    json.object_key("owner_uid");
    json.value((*comp_node)->owner->id_orig_session_uid);
  }
  if (const auto *const *op_node = std::get_if<const OperationNode *>(&node_variant)) {
    json.object_key("name");
    json.value((*op_node)->name);
    json.object_key("owner_type");
    json.value(nodeTypeAsString((*op_node)->owner->type));
    json.object_key("owner_uid");
    json.value((*op_node)->owner->owner->id_orig_session_uid);
  }

  json.object_end();
}

static void deg_debug_json_node_relations(JSONStream &json, const Node &node)
{
  json.object_begin();
  json.object_key("dependent_node");
  deg_debug_json_node_identifier(json, node);

  json.object_key("depends_on");
  json.array_begin();
  for (const Relation *rel : node.inlinks) {
    json.array_item();
    json.object_begin();

    json.object_key("node");
    deg_debug_json_node_identifier(json, *rel->from);
    json.object_key("relation");
    json.value(rel->name);

    json.object_key("is_cyclic");
    json.value(bool(rel->flag & RELATION_FLAG_CYCLIC));
    json.object_key("no_flush");
    json.value(bool(rel->flag & RELATION_FLAG_NO_FLUSH));
    json.object_key("user_edit_only");
    json.value(bool(rel->flag & RELATION_FLAG_FLUSH_USER_EDIT_ONLY));
    json.object_key("god_mode");
    json.value(bool(rel->flag & RELATION_FLAG_GODMODE));
    json.object_key("no_visibility_change");
    json.value(bool(rel->flag & RELATION_NO_VISIBILITY_CHANGE));

    json.object_end();
  }
  json.array_end();

  json.object_end();
}

static void deg_debug_json_graph_nodes(JSONStream &json, const Depsgraph &graph)
{
  json.array_begin();
  for (const Node *node : graph.id_nodes) {
    json.array_item();
    deg_debug_json_node_recursive(json, *node);
  }
  const TimeSourceNode *time_source = graph.find_time_source();
  if (time_source != nullptr) {
    json.array_item();
    deg_debug_json_node_recursive(json, *time_source);
  }
  json.array_end();
}

static void deg_debug_json_graph_relations(JSONStream &json, const Depsgraph &graph)
{
  json.array_begin();
  for (const IDNode *id_node : graph.id_nodes) {
    for (const ComponentNode *comp_node : id_node->components.values()) {
      for (const OperationNode *op_node : comp_node->operations) {
        json.array_item();
        deg_debug_json_node_relations(json, *op_node);
      }
    }
  }

  const TimeSourceNode *time_source = graph.find_time_source();
  if (time_source != nullptr) {
    json.array_item();
    deg_debug_json_node_relations(json, *time_source);
  }
  json.array_end();
}

}  // namespace deg

std::string DEG_debug_graph_to_json(const Depsgraph &graph, const StringRef label)
{
  const deg::Depsgraph &deg_graph = reinterpret_cast<const deg::Depsgraph &>(graph);

  deg::JSONStream json;
  json.object_begin();
  json.object_key("label");
  json.value(label);
  json.object_key("nodes");
  deg::deg_debug_json_graph_nodes(json, deg_graph);
  json.object_key("relations");
  deg::deg_debug_json_graph_relations(json, deg_graph);
  json.object_end();

  return json.str();
}

}  // namespace blender
