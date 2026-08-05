/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <optional>
#include <utility>

#include "testing/testing.h"

#include "BLI_bounds.hh"

#include "BKE_compute_contexts.hh"
#include "BKE_geometry_set.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_object.hh"

#include "NOD_geometry_nodes_execute.hh"
#include "NOD_geometry_nodes_lazy_function.hh"

#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"
#include "RNA_types.hh"

namespace blender::nodes::tests {

class DomainSizeTest : public bke::BlenderGTestBase {};

class TestData {
 public:
  Main *bmain = nullptr;

  TestData()
  {
    bmain = BKE_main_new();
    G.main = bmain;
  }

  ~TestData()
  {
    BKE_main_free(bmain);
    G.main = nullptr;
  }
};

TEST_F(DomainSizeTest, EmptyComponents)
{
  TestData data;
  const auto test_component = [&](const StringRef component_name,
                                  const bke::GeometryComponent::Type component_type,
                                  const StringRef output_name) {
    SCOPED_TRACE(component_name);
    bNodeTree &tree = *bke::node_tree_add_tree(data.bmain, "Domain Size Test", "GeometryNodeTree");
    tree.tree_interface.add_socket(
        "Geometry", "", "NodeSocketGeometry", NODE_INTERFACE_SOCKET_INPUT, nullptr);
    tree.tree_interface.add_socket(
        "Geometry", "", "NodeSocketGeometry", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);

    bNode &group_input = *bke::node_add_node(nullptr, tree, "NodeGroupInput"_ustr);
    bNode &group_output = *bke::node_add_node(nullptr, tree, "NodeGroupOutput"_ustr);
    bNode &domain_size = *bke::node_add_node(
        nullptr, tree, "GeometryNodeAttributeDomainSize"_ustr);
    bNode &combine = *bke::node_add_node(nullptr, tree, "ShaderNodeCombineXYZ"_ustr);
    bNode &cube = *bke::node_add_node(nullptr, tree, "GeometryNodeMeshCube"_ustr);
    /* Pass the integer output through Combine XYZ into the cube's X size because execution
     * returns geometry, allowing the value to be checked through the resulting mesh bounds. */
    domain_size.custom1 = int(component_type);

    BKE_ntree_update_after_single_tree_change(*data.bmain, tree);

    bke::node_add_link(tree,
                       group_input,
                       *reinterpret_cast<bNodeSocket *>(group_input.outputs.first),
                       domain_size,
                       *bke::node_find_enabled_socket(domain_size, SOCK_IN, "Geometry"));
    bke::node_add_link(tree,
                       domain_size,
                       *bke::node_find_enabled_socket(domain_size, SOCK_OUT, output_name),
                       combine,
                       *bke::node_find_enabled_socket(combine, SOCK_IN, "X"));
    bke::node_add_link(tree,
                       combine,
                       *bke::node_find_enabled_socket(combine, SOCK_OUT, "Vector"),
                       cube,
                       *bke::node_find_enabled_socket(cube, SOCK_IN, "Size"));
    bke::node_add_link(tree,
                       cube,
                       *bke::node_find_enabled_socket(cube, SOCK_OUT, "Mesh"),
                       group_output,
                       *reinterpret_cast<bNodeSocket *>(group_output.inputs.first));
    BKE_ntree_update_after_single_tree_change(*data.bmain, tree);

    bke::DataBlockComputeContext compute_context(nullptr, tree.id);
    Object *object = BKE_object_add_only_object(data.bmain, OB_EMPTY, "Domain Size Test Object");
    auto *modifier = reinterpret_cast<NodesModifierData *>(BKE_modifier_new(eModifierType_Nodes));
    BKE_modifiers_add_at_end_if_possible(object, &modifier->modifier);
    modifier->node_group = &tree;
    PointerRNA modifier_ptr = RNA_pointer_create_discrete(
        &object->id, RNA_NodesModifier, modifier);
    PointerRNA properties_ptr = RNA_pointer_get(&modifier_ptr, "properties");
    GeoNodesCallData call_data;
    bke::GeometrySet input_geometry;
    input_geometry.get_component_for_write(component_type);
    const bke::GeometryComponent *component = input_geometry.get_component(component_type);
    ASSERT_NE(component, nullptr);
    ASSERT_TRUE(component->is_empty());

    bke::GeometrySet result = execute_geometry_nodes_on_geometry(
        tree, properties_ptr, compute_context, call_data, std::move(input_geometry));
    const Mesh *mesh = result.get_mesh();
    ASSERT_NE(mesh, nullptr);
    const std::optional<Bounds<float3>> bounds = mesh->bounds_min_max();
    ASSERT_TRUE(bounds.has_value());
    EXPECT_EQ(bounds->min.x, 0.0f);
    EXPECT_EQ(bounds->max.x, 0.0f);
  };

  test_component("Mesh", bke::GeometryComponent::Type::Mesh, "Point Count");
  test_component("Curve", bke::GeometryComponent::Type::Curve, "Point Count");
  test_component("Point Cloud", bke::GeometryComponent::Type::PointCloud, "Point Count");
  test_component("Instances", bke::GeometryComponent::Type::Instance, "Instance Count");
  test_component("Grease Pencil", bke::GeometryComponent::Type::GreasePencil, "Layer Count");
}

}  // namespace blender::nodes::tests
