/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_volume_grid.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

#ifdef WITH_OPENVDB
#  include "openvdb/tools/PoissonSolver.h"
#endif

namespace blender::nodes::node_geo_grid_solve_poisson_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Grid"_ustr)
      .hide_value()
      .structure_type(StructureType::Grid)
      .description("Right-hand side of the Poisson equation");
  b.add_input<decl::Int>("Iterations"_ustr)
      .default_value(100)
      .min(1)
      .max(10000)
      .description("Maximum number of solver iterations");
  b.add_input<decl::Float>("Tolerance"_ustr)
      .default_value(1e-3f)
      .min(0.0f)
      .description("Convergence tolerance for the solver");
  b.add_input<decl::Bool>("Staggered"_ustr)
      .default_value(false)
      .description("Use staggered grid discretization for the Laplacian operator");
  b.add_output<decl::Float>("Solution"_ustr)
      .structure_type(StructureType::Grid)
      .description("Solution to the Poisson equation");
  b.add_output<decl::Bool>("Success"_ustr).description("If the solver converged successfully");
  b.add_output<decl::Int>("Iterations"_ustr).description("Number of iterations performed");
  b.add_output<decl::Float>("Absolute Error"_ustr)
      .description("Final absolute error of the solution");
  b.add_output<decl::Float>("Relative Error"_ustr)
      .description("Final relative error of the solution");
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  const bke::VolumeGrid<float> source_grid = params.extract_input<bke::VolumeGrid<float>>(
      "Grid"_ustr);
  if (!source_grid) {
    params.set_default_remaining_outputs();
    return;
  }

  const int max_iterations = params.extract_input<int>("Iterations"_ustr);
  const float tolerance = params.extract_input<float>("Tolerance"_ustr);
  const bool staggered = params.extract_input<bool>("Staggered"_ustr);

  bke::VolumeTreeAccessToken tree_token;
  const openvdb::FloatGrid &source_vdb_grid = source_grid.grid(tree_token);

  openvdb::math::pcg::State solver_state;
  solver_state.iterations = max_iterations;
  solver_state.relativeError = double(tolerance);
  solver_state.absoluteError = double(tolerance);

  openvdb::FloatTree::Ptr solution_tree = openvdb::tools::poisson::solve(
      source_vdb_grid.tree(), solver_state, staggered);

  openvdb::FloatGrid::Ptr solution_grid = openvdb::FloatGrid::create(std::move(solution_tree));
  solution_grid->setTransform(source_vdb_grid.transform().copy());

  params.set_output("Solution"_ustr, bke::VolumeGrid<float>(std::move(solution_grid)));
  params.set_output("Iterations"_ustr, solver_state.iterations);
  params.set_output("Success"_ustr, solver_state.success);
  params.set_output("Absolute Error"_ustr, float(solver_state.absoluteError));
  params.set_output("Relative Error"_ustr, float(solver_state.relativeError));

  if (!solver_state.success) {
    params.error_message_add(
        NodeWarningType::Warning,
        "Poisson solver failed to converge within the iteration limit. Try increasing "
        "the maximum iterations or tolerance.");
  }
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeGridSolvePoisson"_ustr);
  ntype.ui_name = "Grid Solve Poisson";
  ntype.ui_description =
      "Solve the Poisson equation for a scalar field. Computes a grid whose Laplacian equals the "
      "input scalar grid.";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grid_solve_poisson_cc
