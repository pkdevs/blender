/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <atomic>

#include "BKE_geometry_fields.hh"
#include "BKE_node_runtime.hh"

#include "GEO_foreach_geometry.hh"

#include "FN_field_gpu.hh"

#include "NOD_rna_define.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

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

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int16_t(AttrDomain::Point);
}

static bool is_pure_field_node(const bNode &node)
{
  if (node.is_muted() || node.is_group() || node.is_group_input() || node.is_group_output()) {
    return false;
  }
  /* Field inputs may evaluate their dependencies in a different context. */
  if (!node.is_reroute() && node.typeinfo->build_multi_function == nullptr) {
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

static bool can_retain_gpu_field_result(const bNode &producer)
{
  const bNodeSocket *field_output = producer.output_by_identifier("Field"_ustr);
  if (producer.is_muted() || field_output == nullptr) {
    return false;
  }

  Vector<const bNode *, 2> consumers;
  Vector<const bNode *> active_nodes;
  Vector<const bNode *> completed_nodes;
  const auto walk = [&](const auto &self, const bNodeSocket &output) -> bool {
    if (output.directly_linked_links().is_empty()) {
      return false;
    }
    for (const bNodeLink *link : output.directly_linked_links()) {
      if (link->tonode == nullptr || link->fromsock == nullptr || link->tosock == nullptr ||
          !link->is_used() || !(link->flag & NODE_LINK_VALID))
      {
        return false;
      }
      const bNode &node = *link->tonode;
      if (node.type_legacy == GEO_NODE_VECTOR_FIELD_GPU) {
        if (node.is_muted() || node.custom1 != producer.custom1 ||
            link->tosock->identifier_ustr() != "Field"_ustr)
        {
          return false;
        }
        if (!consumers.contains(&node)) {
          if (consumers.size() == 2) {
            return false;
          }
          consumers.append(&node);
        }
        continue;
      }
      if (!is_pure_field_node(node) || active_nodes.contains(&node)) {
        return false;
      }
      if (completed_nodes.contains(&node)) {
        continue;
      }
      active_nodes.append(&node);
      bool has_linked_output = false;
      for (const bNodeSocket *candidate : node.output_sockets()) {
        if (candidate->directly_linked_links().is_empty()) {
          continue;
        }
        if (candidate->type != SOCK_VECTOR || !self(self, *candidate)) {
          return false;
        }
        has_linked_output = true;
      }
      if (!has_linked_output) {
        return false;
      }
      active_nodes.remove_last();
      completed_nodes.append(&node);
    }
    return true;
  };
  if (!walk(walk, *field_output) || consumers.is_empty()) {
    return false;
  }

  /* Geometry branching would break the retained result's identity matching. */
  const bNode *geometry_node = &producer;
  for (int i = 0; i < 2; i++) {
    const bNodeSocket *output = geometry_node->output_by_identifier("Geometry"_ustr);
    if (output == nullptr || output->directly_linked_links().size() != 1) {
      return false;
    }
    const bNodeLink *link = output->directly_linked_links().first();
    if (link->tonode == nullptr || link->fromsock == nullptr || link->tosock == nullptr ||
        !link->is_used() || !(link->flag & NODE_LINK_VALID) ||
        link->tosock->identifier_ustr() != "Geometry"_ustr ||
        link->tonode->type_legacy != GEO_NODE_VECTOR_FIELD_GPU || link->tonode->is_muted() ||
        link->tonode->custom1 != producer.custom1)
    {
      return false;
    }
    geometry_node = link->tonode;
    if (consumers.contains(geometry_node)) {
      consumers.remove_first_occurrence_and_reorder(geometry_node);
    }
    if (consumers.is_empty()) {
      return true;
    }
  }
  return false;
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
                                        const AttrDomain domain,
                                        const fn::GField &field,
                                        fn::GPUFieldChainScope *chain_scope)
{
  /* Grease Pencil requires a separate context for each layer. */
  if (chain_scope == nullptr || component.type() == GeometryComponent::Type::GreasePencil) {
    return false;
  }

  bke::MutableAttributeAccessor attributes = *component.attributes_for_write();
  const int domain_size = attributes.domain_size(domain);
  if (domain_size == 0) {
    return false;
  }

  const bke::GeometryFieldContext context{component, domain};
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
  const AttrDomain domain = AttrDomain(params.node().custom1);
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
    const bool can_retain_result = can_retain_gpu_field_result(params.node());

    const auto capture_on = [&](GeometryComponent &component) {
      if (component.type() == GeometryComponent::Type::GreasePencil) {
        if (domain != AttrDomain::Point) {
          return;
        }
      }
      else if (!component.attributes()->domain_supported(domain)) {
        return;
      }
      if (can_retain_result &&
          try_retain_field_gpu_result(component, *attribute_id, domain, field, chain_scope))
      {
        return;
      }
      if (!bke::try_capture_fields_on_geometry(component,
                                               {*attribute_id},
                                               domain,
                                               fields,
                                               fn::FieldEvaluationMode::GPU_EXPERIMENTAL,
                                               chain_scope))
      {
        success.store(false, std::memory_order_relaxed);
      }
    };

    if (domain == AttrDomain::Instance) {
      if (geometry.has_instances()) {
        capture_on(geometry.get_component_for_write(GeometryComponent::Type::Instance));
      }
    }
    else {
      geometry::foreach_real_geometry(geometry, [&](GeometrySet &instance_geometry) {
        for (const GeometryComponent::Type component_type :
             {GeometryComponent::Type::Mesh,
              GeometryComponent::Type::PointCloud,
              GeometryComponent::Type::Curve,
              GeometryComponent::Type::GreasePencil})
        {
          if (instance_geometry.has(component_type)) {
            capture_on(instance_geometry.get_component_for_write(component_type));
          }
        }
      });
    }

    if (!success.load(std::memory_order_relaxed)) {
      params.error_message_add(NodeWarningType::Warning,
                               "The vector field could not be materialized on every component");
    }

    /* The lazy-function wrapper has already created the anonymous attribute field for this
     * output. Keep that field and only materialize the corresponding attribute here. */
  }

  params.set_output("Geometry"_ustr, std::move(geometry));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "domain",
      "Domain",
      "Domain on which to evaluate the field",
      rna_enum_attribute_domain_items,
      NOD_inline_enum_accessors(custom1),
      int(AttrDomain::Point),
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(rna_enum_attribute_domain_items,
                                 [](const EnumPropertyItem &item) {
                                   return ELEM(AttrDomain(item.value),
                                               AttrDomain::Point,
                                               AttrDomain::Edge,
                                               AttrDomain::Face,
                                               AttrDomain::Corner,
                                               AttrDomain::Curve,
                                               AttrDomain::Instance);
                                 });
      });
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
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_vector_field_gpu_cc
