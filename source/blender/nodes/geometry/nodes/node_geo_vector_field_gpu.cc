/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <atomic>

#include "BKE_geometry_fields.hh"
#include "BKE_node_runtime.hh"

#include "GEO_foreach_geometry.hh"

#include "FN_field_gpu.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_vector_field_gpu_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .description("Geometry that provides the field evaluation context");
  b.add_input<decl::Vector>("Field"_ustr)
      .default_value({0.0f, 0.0f, 0.0f})
      .evaluated_geometry_field({0});
  b.add_output<decl::Geometry>("Geometry"_ustr).propagate_all_geometry();
  b.add_output<decl::Vector>("Field"_ustr)
      .description("The materialized vector field")
      .anonymous_attribute_output({0});
}

static bool is_pure_field_node(const bNode &node)
{
  if (node.is_muted() || node.is_group() || node.is_group_input() || node.is_group_output()) {
    return false;
  }
  for (const bNodeSocket *socket : node.input_sockets()) {
    if (socket->type == SOCK_GEOMETRY) {
      return false;
    }
  }
  for (const bNodeSocket *socket : node.output_sockets()) {
    if (socket->type == SOCK_GEOMETRY) {
      return false;
    }
  }
  return true;
}

/**
 * Match a single field path and a direct geometry link to another Vector Field GPU node.
 * Fan-out and nodes with geometry sockets require CPU materialization.
 */
static const bNode *find_gpu_field_consumer(const bNode &producer)
{
  const bNodeSocket *field_output = producer.output_by_identifier("Field"_ustr);
  const bNodeSocket *geometry_output = producer.output_by_identifier("Geometry"_ustr);
  if (field_output == nullptr || geometry_output == nullptr ||
      field_output->directly_linked_links().size() != 1 ||
      geometry_output->directly_linked_links().size() != 1)
  {
    return nullptr;
  }

  const bNodeLink *geometry_link = geometry_output->directly_linked_links().first();
  if (geometry_link->tonode == nullptr || geometry_link->fromsock == nullptr ||
      geometry_link->tosock == nullptr || !geometry_link->is_used() ||
      !(geometry_link->flag & NODE_LINK_VALID) ||
      geometry_link->tosock->identifier_ustr() != "Geometry"_ustr)
  {
    return nullptr;
  }

  const bNode *consumer = nullptr;
  Vector<const bNode *> visited_nodes;
  const auto walk = [&](const auto &self, const bNodeSocket &output) -> bool {
    if (output.directly_linked_links().size() != 1) {
      return false;
    }
    const bNodeLink *link = output.directly_linked_links().first();
    if (link->tonode == nullptr || link->fromsock == nullptr || link->tosock == nullptr ||
        !link->is_used() || !(link->flag & NODE_LINK_VALID))
    {
      return false;
    }

    const bNode &node = *link->tonode;
    if (node.type_legacy == GEO_NODE_VECTOR_FIELD_GPU) {
      if (node.is_muted() || link->tosock->identifier_ustr() != "Field"_ustr ||
          (consumer != nullptr && consumer != &node) || geometry_link->tonode != &node)
      {
        return false;
      }
      consumer = &node;
      return true;
    }
    if (!is_pure_field_node(node) || visited_nodes.contains(&node)) {
      return false;
    }
    visited_nodes.append(&node);

    const bNodeSocket *next_output = nullptr;
    for (const bNodeSocket *candidate : node.output_sockets()) {
      const Span<const bNodeLink *> links = candidate->directly_linked_links();
      if (links.is_empty()) {
        continue;
      }
      if (candidate->type != SOCK_VECTOR || links.size() != 1 || next_output != nullptr) {
        return false;
      }
      next_output = candidate;
    }
    return next_output != nullptr && self(self, *next_output);
  };

  if (!walk(walk, *field_output)) {
    return nullptr;
  }
  return consumer;
}

static fn::GPUFieldContextKey gpu_field_context_key(const bke::GeometryFieldContext &context)
{
  fn::GPUFieldContextKey key;
  key.geometry = context.geometry();
  key.component_type = int(context.type());
  key.domain = int(context.domain());
  return key;
}

static bool try_retain_field_gpu_result(GeometryComponent &component,
                                        const StringRef result_name,
                                        const fn::GField &field,
                                        fn::GPUFieldChainScope *chain_scope)
{
  /* Grease Pencil requires a separate context for each layer. */
  if (chain_scope == nullptr || component.type() == GeometryComponent::Type::GreasePencil) {
    return false;
  }

  bke::MutableAttributeAccessor attributes = *component.attributes_for_write();
  const int domain_size = attributes.domain_size(bke::AttrDomain::Point);
  if (domain_size == 0) {
    return false;
  }

  const bke::GeometryFieldContext context{component, bke::AttrDomain::Point};
  const fn::GPUFieldContextKey context_key = gpu_field_context_key(context);
  const IndexMask mask{IndexRange(domain_size)};
  if (!fn::try_evaluate_field_gpu_to_transient_result(
          field, context, mask, context_key, result_name, *chain_scope))
  {
    return false;
  }

  /* A stale anonymous attribute must not shadow the transient result if this geometry is being
   * evaluated repeatedly. The existing field wrapper remains the logical output value. */
  attributes.remove(result_name);
  return true;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Geometry"_ustr);
  const GField field = params.extract_input<GField>("Field"_ustr);
  const std::optional<std::string> attribute_id =
      params.get_output_anonymous_attribute_id_if_needed("Field"_ustr);

  if (!attribute_id) {
    params.set_output("Geometry"_ustr, std::move(geometry));
    params.set_default_remaining_outputs();
    return;
  }

  {
    const Vector<GField> fields{field};
    std::atomic<bool> success = true;
    fn::GPUFieldChainScope *chain_scope = nullptr;
    if (GeoNodesUserData *user_data = params.user_data()) {
      chain_scope = user_data->gpu_field_chain_scope;
    }
    const bool can_retain_result = find_gpu_field_consumer(params.node()) != nullptr;

    geometry::foreach_real_geometry(geometry, [&](GeometrySet &instance_geometry) {
      for (const GeometryComponent::Type component_type : {GeometryComponent::Type::Mesh,
                                                           GeometryComponent::Type::PointCloud,
                                                           GeometryComponent::Type::Curve,
                                                           GeometryComponent::Type::GreasePencil})
      {
        if (!instance_geometry.has(component_type)) {
          continue;
        }
        GeometryComponent &component = instance_geometry.get_component_for_write(component_type);
        if (can_retain_result &&
            try_retain_field_gpu_result(component, *attribute_id, field, chain_scope))
        {
          continue;
        }
        if (!bke::try_capture_fields_on_geometry(component,
                                                 {*attribute_id},
                                                 bke::AttrDomain::Point,
                                                 fields,
                                                 fn::FieldEvaluationMode::GPU_EXPERIMENTAL,
                                                 chain_scope))
        {
          success.store(false, std::memory_order_relaxed);
        }
      }
    });

    if (!success.load(std::memory_order_relaxed)) {
      params.error_message_add(NodeWarningType::Warning,
                               "The vector field could not be materialized on every component");
    }

    /* The lazy-function wrapper has already created the anonymous attribute field for this
     * output. Keep that field and only materialize the corresponding attribute here. */
  }

  params.set_output("Geometry"_ustr, std::move(geometry));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeVectorFieldGPU"_ustr, GEO_NODE_VECTOR_FIELD_GPU);
  ntype.ui_name = "Vector Field GPU";
  ntype.ui_description =
      "Materialize a supported vector field through the experimental GPU field evaluator";
  ntype.enum_name_legacy = "VECTOR_FIELD_GPU";
  ntype.nclass = NODE_CLASS_ATTRIBUTE;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_vector_field_gpu_cc
