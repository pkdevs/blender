/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_volume_grid_process.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

#ifdef WITH_OPENVDB
#  include "openvdb/tools/PoissonSolver.h"
#endif

namespace blender::nodes::node_geo_grid_solve_poisson_cc {

enum class Boundary {
  /** Dirichlet boundary condition, using the background value for all exterior boundary voxels. */
  Background,
  /** Dirichlet boundary condition, specify the solution value for all exterior boundary voxels. */
  Fixed,
  /** Neumann boundary condition, specify normal derivative for all exterior boundary voxels. */
  Normal,
  /** Select between \a Fixed and \a Normal boundary condition. */
  Mixed,
};

static const EnumPropertyItem boundary_items[] = {
    {int(Boundary::Background),
     "BACKGROUND",
     0,
     "Background",
     "Dirichlet boundary condition, using the background value for all exterior boundary voxels"},
    {int(Boundary::Fixed),
     "FIXED",
     0,
     "Fixed",
     "Dirichlet boundary condition, specify the solution value for all exterior boundary voxels"},
    {int(Boundary::Normal),
     "NORMAL",
     0,
     "Normal",
     "Neumann boundary condition, specify normal derivative for all exterior boundary voxels"},
    {int(Boundary::Mixed),
     "MIXED",
     0,
     "Mixed",
     "Select between fixed and normal boundary condition"},
    {0, nullptr, 0, nullptr, nullptr},
};

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

  b.add_input<decl::Menu>("Boundary"_ustr)
      .static_items(boundary_items)
      .default_value(Boundary::Background)
      .description("Boundary conditions for the solution between active and inactive voxels");
  b.add_input<decl::Float>("Boundary Value"_ustr)
      .structure_type(StructureType::Field)
      .hide_value()
      .usage_by_menu("Boundary"_ustr, {int(Boundary::Fixed), int(Boundary::Mixed)});
  b.add_input<decl::Vector>("Boundary Gradient"_ustr)
      .structure_type(StructureType::Field)
      .hide_value()
      .usage_by_menu("Boundary"_ustr, {int(Boundary::Normal), int(Boundary::Mixed)});
  b.add_input<decl::Float>("Boundary Factor"_ustr)
      .subtype(PROP_FACTOR)
      .min(0.0f)
      .max(1.0f)
      .structure_type(StructureType::Field)
      .usage_by_menu("Boundary"_ustr, int(Boundary::Mixed))
      .description("Mix factor between boundary conditions (0=value, 1=gradient)");

  b.add_output<decl::Float>("Solution"_ustr)
      .structure_type(StructureType::Grid)
      .description("Solution to the Poisson equation");
  b.add_output<decl::Bool>("Success"_ustr).description("If the solver converged successfully");
  b.add_output<decl::Int>("Iterations"_ustr).description("Number of iterations performed");
  b.add_output<decl::Float>("Absolute Error"_ustr)
      .description("Final absolute error of the solution");
  b.add_output<decl::Float>("Relative Error"_ustr)
      .description("Final relative error of the solution");

  auto &p = b.add_panel("Debug"_ustr);
  p.add_output<decl::Float>("Debug Boundary Value"_ustr).structure_type(StructureType::Grid);
  p.add_output<decl::Vector>("Debug Boundary Gradient"_ustr).structure_type(StructureType::Grid);
  p.add_output<decl::Float>("Debug Boundary Factor"_ustr).structure_type(StructureType::Grid);
}

struct DirichletBoundaryOp {
  const openvdb::FloatGrid::ConstAccessor value_accessor_;
  const float voxel_volume_;

  DirichletBoundaryOp(const openvdb::FloatGrid &value_grid)
      : value_accessor_(value_grid.getConstAccessor()),
        voxel_volume_(value_grid.voxelSize().product())
  {
  }

  void operator()(const openvdb::Coord &ijk,
                  const openvdb::Coord & /*neighbor*/,
                  double &source,
                  double &diagonal) const
  {
    const float value = value_accessor_.getValue(ijk);
    diagonal -= 1.0;
    source += value * voxel_volume_;
  };
};

struct NeumannBoundaryOp {
  const openvdb::Vec3SGrid::ConstAccessor gradient_accessor_;
  const openvdb::math::Vec3s voxel_size_;

  NeumannBoundaryOp(const openvdb::Vec3SGrid &gradient_grid)
      : gradient_accessor_(gradient_grid.getConstAccessor()),
        voxel_size_(gradient_grid.voxelSize())
  {
  }

  void operator()(const openvdb::Coord &ijk,
                  const openvdb::Coord &neighbor,
                  double &source,
                  double & /*diagonal*/) const
  {
    const openvdb::math::Vec3s normal = ijk.asVec3s() - neighbor.asVec3s();
    const openvdb::math::Vec3s gradient = gradient_accessor_.getValue(ijk);
    source += (gradient * voxel_size_).dot(normal);
  };
};

struct MixedBoundaryOp {
  const openvdb::FloatGrid::ConstAccessor value_accessor_;
  const openvdb::Vec3SGrid::ConstAccessor gradient_accessor_;
  const openvdb::FloatGrid::ConstAccessor factor_accessor_;
  const openvdb::math::Vec3s voxel_size_;
  const float voxel_volume_;

  MixedBoundaryOp(const openvdb::FloatGrid &value_grid,
                  const openvdb::Vec3SGrid &gradient_grid,
                  const openvdb::FloatGrid &factor_grid)
      : value_accessor_(value_grid.getConstAccessor()),
        gradient_accessor_(gradient_grid.getConstAccessor()),
        factor_accessor_(factor_grid.getConstAccessor()),
        voxel_size_(gradient_grid.voxelSize()),
        voxel_volume_(value_grid.voxelSize().product())
  {
  }

  void operator()(const openvdb::Coord &ijk,
                  const openvdb::Coord &neighbor,
                  double &source,
                  double &diagonal) const
  {
    const openvdb::math::Vec3s normal = ijk.asVec3s() - neighbor.asVec3s();
    const float value = value_accessor_.getValue(neighbor);
    const openvdb::math::Vec3s gradient = gradient_accessor_.getValue(neighbor);
    const float factor = factor_accessor_.getValue(neighbor);
    diagonal -= 1.0 * (1.0f - factor);
    source += math::interpolate(
        value * voxel_volume_, (gradient * voxel_size_).dot(normal), factor);
    // std::cout << "A" << ijk << "=" << diagonal << ", b=" << source << std::endl;
  };
};

/* Extract the outermost layer of active voxels on which to evaluate boundary conditions. */
static openvdb::MaskTree::Ptr get_boundary_mask(const openvdb::FloatTree &tree)
{
  using MaskTreeT = openvdb::MaskTree;
  typename MaskTreeT::Ptr interior_mask(new MaskTreeT(tree, false, openvdb::TopologyCopy()));
  typename MaskTreeT::Ptr boundary_mask(new MaskTreeT(tree, false, openvdb::TopologyCopy()));

  //   openvdb::tools::erodeActiveValues(
  //       *interior_mask, /*iterations=*/1, openvdb::tools::NN_FACE,
  //       openvdb::tools::IGNORE_TILES);
  // openvdb::tools::pruneInactive(*interior_mask);
  openvdb::tools::dilateActiveValues(
      *boundary_mask, /*iterations=*/1, openvdb::tools::NN_FACE, openvdb::tools::IGNORE_TILES);
  boundary_mask->topologyDifference(*interior_mask);
  return boundary_mask;
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
  const Boundary boundary = params.extract_input<Boundary>("Boundary"_ustr);

  bke::VolumeTreeAccessToken tree_token;
  const openvdb::FloatGrid &source_vdb_grid = source_grid.grid(tree_token);
  const openvdb::math::Transform &grid_transform = source_vdb_grid.transform();

  openvdb::math::pcg::State solver_state;
  solver_state.iterations = max_iterations;
  solver_state.relativeError = double(tolerance);
  solver_state.absoluteError = double(tolerance);

  openvdb::FloatTree::Ptr solution_tree;
  using PreconditionerType =
      openvdb::math::pcg::JacobiPreconditioner<openvdb::tools::poisson::LaplacianMatrix>;
  openvdb::util::NullInterrupter interrupter;
  switch (boundary) {
    case Boundary::Background:
      solution_tree = openvdb::tools::poisson::solve(
          source_vdb_grid.tree(), solver_state, staggered);
      break;

    case Boundary::Fixed: {
      const Field<float> boundary_value_field = params.extract_input<Field<float>>(
          "Boundary Value"_ustr);
      const openvdb::MaskTree::Ptr mask_tree = get_boundary_mask(source_vdb_grid.tree());
      Vector<bke::GVolumeGrid> grids(1);
      bke::volume_grid::evaluate_fields_to_grid(
          *mask_tree, grid_transform, {boundary_value_field}, grids);

      bke::VolumeTreeAccessToken grid_token;
      DirichletBoundaryOp boundary_op(grids[0].typed<float>().grid(grid_token));
      solution_tree = openvdb::tools::poisson::solveWithBoundaryConditionsAndPreconditioner<
          PreconditionerType>(
          source_vdb_grid.tree(), boundary_op, solver_state, interrupter, staggered);
      break;
    }

    case Boundary::Normal: {
      const Field<float3> boundary_gradient_field = params.extract_input<Field<float3>>(
          "Boundary Gradient"_ustr);
      const openvdb::MaskTree::Ptr mask_tree = get_boundary_mask(source_vdb_grid.tree());
      Vector<bke::GVolumeGrid> grids(1);
      bke::volume_grid::evaluate_fields_to_grid(
          *mask_tree, grid_transform, {boundary_gradient_field}, grids);

      bke::VolumeTreeAccessToken grid_token;
      NeumannBoundaryOp boundary_op(grids[0].typed<float3>().grid(grid_token));
      solution_tree = openvdb::tools::poisson::solveWithBoundaryConditionsAndPreconditioner<
          PreconditionerType>(
          source_vdb_grid.tree(), boundary_op, solver_state, interrupter, staggered);
      break;
    }

    case Boundary::Mixed: {
      const Field<float> boundary_value_field = params.extract_input<Field<float>>(
          "Boundary Value"_ustr);
      const Field<float3> boundary_gradient_field = params.extract_input<Field<float3>>(
          "Boundary Gradient"_ustr);
      const Field<float> boundary_factor_field = params.extract_input<Field<float>>(
          "Boundary Factor"_ustr);
      const openvdb::MaskTree::Ptr mask_tree = get_boundary_mask(source_vdb_grid.tree());
      Vector<bke::GVolumeGrid> grids(3);
      bke::volume_grid::evaluate_fields_to_grid(
          *mask_tree,
          grid_transform,
          {boundary_value_field, boundary_gradient_field, boundary_factor_field},
          grids);

      bke::VolumeTreeAccessToken grid_tokens[3];
      MixedBoundaryOp boundary_op(grids[0].typed<float>().grid(grid_tokens[0]),
                                  grids[1].typed<float3>().grid(grid_tokens[1]),
                                  grids[2].typed<float>().grid(grid_tokens[2]));
      solution_tree = openvdb::tools::poisson::solveWithBoundaryConditionsAndPreconditioner<
          PreconditionerType>(
          source_vdb_grid.tree(), boundary_op, solver_state, interrupter, staggered);

      if (params.output_is_required("Debug Boundary Value"_ustr)) {
        params.set_output("Debug Boundary Value"_ustr, grids[0]);
      }
      if (params.output_is_required("Debug Boundary Gradient"_ustr)) {
        params.set_output("Debug Boundary Gradient"_ustr, grids[1]);
      }
      if (params.output_is_required("Debug Boundary Factor"_ustr)) {
        params.set_output("Debug Boundary Factor"_ustr, grids[2]);
      }
      break;
    }

    default:
      BLI_assert_unreachable();
      solution_tree = std::make_shared<openvdb::FloatTree>();
      break;
  }

  openvdb::FloatGrid::Ptr solution_grid = openvdb::FloatGrid::create(std::move(solution_tree));
  solution_grid->setTransform(grid_transform.copy());

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
