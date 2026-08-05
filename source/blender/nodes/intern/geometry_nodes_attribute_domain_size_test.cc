/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <array>
#include <memory>
#include <tuple>
#include <utility>

#include "testing/testing.h"

#include "FN_lazy_function_execute.hh"

#include "BKE_compute_contexts.hh"
#include "BKE_geometry_set.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_socket_value.hh"
#include "BKE_node_tree_update.hh"

#include "NOD_geometry_nodes_lazy_function.hh"

#include "DNA_node_types.h"

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

TEST_F(DomainSizeTest, EmptyComponentNodeExecution)
{
  struct TestCase {
    const char *name;
    bke::GeometryComponent::Type component_type;
    const char *output_name;
  };
  const std::array test_cases = {
      TestCase{"Mesh", bke::GeometryComponent::Type::Mesh, "Point Count"},
      TestCase{"Curve", bke::GeometryComponent::Type::Curve, "Point Count"},
      TestCase{"Point Cloud", bke::GeometryComponent::Type::PointCloud, "Point Count"},
      TestCase{"Instances", bke::GeometryComponent::Type::Instance, "Instance Count"},
      TestCase{"Grease Pencil", bke::GeometryComponent::Type::GreasePencil, "Layer Count"},
  };

  TestData data;
  for (const TestCase &test_case : test_cases) {
    SCOPED_TRACE(test_case.name);
    bke::GeometrySet input_geometry;
    bke::GeometryComponent &component = input_geometry.get_component_for_write(
        test_case.component_type);
    /* Keep the component in the set even though it has no underlying data. */
    ASSERT_TRUE(component.is_empty());

    bNodeTree &tree = *bke::node_tree_add_tree(data.bmain, "Domain Size Test", "GeometryNodeTree");
    tree.tree_interface.add_socket(
        "Geometry", "", "NodeSocketGeometry", NODE_INTERFACE_SOCKET_INPUT, nullptr);
    tree.tree_interface.add_socket(
        "Count", "", "NodeSocketInt", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);

    bNode &group_input = *bke::node_add_node(nullptr, tree, "NodeGroupInput"_ustr);
    bNode &group_output = *bke::node_add_node(nullptr, tree, "NodeGroupOutput"_ustr);
    bNode &domain_size = *bke::node_add_node(
        nullptr, tree, "GeometryNodeAttributeDomainSize"_ustr);
    domain_size.custom1 = int(test_case.component_type);

    BKE_ntree_update_after_single_tree_change(*data.bmain, tree);

    bke::node_add_link(tree,
                       group_input,
                       *bke::node_find_enabled_socket(group_input, SOCK_OUT, "Geometry"),
                       domain_size,
                       *bke::node_find_enabled_socket(domain_size, SOCK_IN, "Geometry"));
    bke::node_add_link(
        tree,
        domain_size,
        *bke::node_find_enabled_socket(domain_size, SOCK_OUT, test_case.output_name),
        group_output,
        *bke::node_find_enabled_socket(group_output, SOCK_IN, "Count"));
    BKE_ntree_update_after_single_tree_change(*data.bmain, tree);

    const std::shared_ptr<const GeometryNodesLazyFunctionGraphInfo> &graph_info =
        ensure_geometry_nodes_lazy_function_graph(tree);
    ASSERT_NE(graph_info, nullptr);

    bke::SocketValueVariant input_value = bke::SocketValueVariant::From(std::move(input_geometry));
    const bool output_used = true;
    bke::SocketValueVariant output_value;
    bool input_used = false;

    GeoNodesCallData call_data;
    call_data.root_ntree = &tree;
    bke::DataBlockComputeContext compute_context(nullptr, tree.id);
    GeoNodesUserData user_data;
    user_data.call_data = &call_data;
    user_data.compute_context = &compute_context;
    GeoNodesLocalUserData local_user_data(user_data);

    const fn::lazy_function::LazyFunction &function = *graph_info->function.function;
    fn::lazy_function::execute_lazy_function_eagerly(
        function,
        &user_data,
        &local_user_data,
        std::make_tuple(std::move(input_value), output_used),
        std::make_tuple(&output_value, &input_used));

    EXPECT_EQ(output_value.extract<int>(), 0);
  }
}

}  // namespace blender::nodes::tests
