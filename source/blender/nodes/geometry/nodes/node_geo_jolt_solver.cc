/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_JOLT

#  include <Jolt/Jolt.h>

#  include <Jolt/Core/JobSystemSingleThreaded.h>
#  include <Jolt/Physics/Body/BodyActivationListener.h>
#  include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#  include <Jolt/Physics/Collision/ContactListener.h>
#  include <Jolt/Physics/Collision/ObjectLayer.h>
#  include <Jolt/Physics/PhysicsSystem.h>

#endif

#include "BLI_math_matrix.hh"

#include "NOD_geo_tag_filter.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_simulation.hh"

#include "node_geometry_util.hh"

#include "GEO_jolt.hh"

namespace blender::nodes::node_geo_jolt_solver_cc {

using simulation::DataKey;
using simulation::GeometrySetData;

namespace attribute_names = simulation::attribute_names;

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Bundle>("World"_ustr)
      .evaluated_geometry_field()
      .structure_type(StructureType::Single)
      .description("World state that is updated by the solver");
  b.add_output<decl::Bundle>("World"_ustr).pass_through_input_index(0).align_with_previous();

  b.add_input<decl::Float>("Delta Time"_ustr)
      .min(0)
      .default_value(1 / 25.0f)
      .subtype(PROP_TIME_ABSOLUTE);
  b.add_input<decl::String>("Filter"_ustr)
      .optional_label()
      .description("Filters the geometry sets to process based on their tags");
  b.add_input<decl::Matrix>("Simulation to World"_ustr);
  {
    auto &solver_panel = b.add_panel("Solver"_ustr).default_closed(true);
    solver_panel.add_input<decl::Int>("Substeps"_ustr).default_value(1).min(1);
    solver_panel.add_input<decl::String>("Solver Path"_ustr);
  }
  {
    auto &p = b.add_panel("Interpolation Range"_ustr).default_closed(true);
    p.add_input<decl::Float>("Begin"_ustr).default_value(0.0).min(0.0);
    p.add_input<decl::Float>("End"_ustr).default_value(1.0).min(0.0);
  }
}

#ifdef WITH_JOLT

static JPH::Vec3 convert_vec3(const float3 &v)
{
  return JPH::Vec3(v.x, v.y, v.z);
}
static float3 convert_vec3(const JPH::Vec3 &v)
{
  return float3(v.GetX(), v.GetY(), v.GetZ());
}
static JPH::Quat convert_quat(const math::Quaternion &q)
{
  return JPH::Quat(q.x, q.y, q.z, q.w);
}
static math::Quaternion convert_quat(const JPH::Quat &q)
{
  return math::Quaternion(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
}

namespace ObjectLayers {
static constexpr JPH::ObjectLayer non_moving(0);
static constexpr JPH::ObjectLayer moving(1);
static constexpr int num_layers = 2;
}  // namespace ObjectLayers

namespace BroadPhaseLayers {
static constexpr JPH::BroadPhaseLayer non_moving(0);
static constexpr JPH::BroadPhaseLayer moving(1);
static constexpr int num_layers = 2;
}  // namespace BroadPhaseLayers

class BroadPhaseLayerInterfaceImpl : public JPH::BroadPhaseLayerInterface {
 private:
  std::array<JPH::BroadPhaseLayer, ObjectLayers::num_layers> object_to_broad_phase_;

 public:
  BroadPhaseLayerInterfaceImpl()
  {
    object_to_broad_phase_[ObjectLayers::non_moving] = BroadPhaseLayers::non_moving;
    object_to_broad_phase_[ObjectLayers::moving] = BroadPhaseLayers::moving;
  }

  uint GetNumBroadPhaseLayers() const override
  {
    return BroadPhaseLayers::num_layers;
  }

  JPH::BroadPhaseLayer GetBroadPhaseLayer(const JPH::ObjectLayer object_layer) const override
  {
    BLI_assert(object_layer >= 0 && object_layer < ObjectLayers::num_layers);
    return object_to_broad_phase_[object_layer];
  }
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(const JPH::ObjectLayer a, const JPH::ObjectLayer b) const override
  {
    if (a == ObjectLayers::moving || b == ObjectLayers::moving) {
      return true;
    }
    return false;
  }
};

class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(const JPH::ObjectLayer object_layer,
                     const JPH::BroadPhaseLayer broad_phase_layer) const override
  {
    if (object_layer == ObjectLayers::moving || broad_phase_layer == BroadPhaseLayers::moving) {
      return true;
    }
    return false;
  }
};

class ContactListenerImpl : public JPH::ContactListener {
 public:
  virtual JPH::ValidateResult OnContactValidate(
      const JPH::Body & /*body_a*/,
      const JPH::Body & /*body_b*/,
      JPH::RVec3Arg /*in_base_offset*/,
      const JPH::CollideShapeResult & /*in_collision_result*/) override
  {
    return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
  }

  void OnContactAdded(const JPH::Body & /*body_a*/,
                      const JPH::Body & /*body_b*/,
                      const JPH::ContactManifold & /*manifold*/,
                      JPH::ContactSettings & /*settings*/) override
  {
  }

  void OnContactPersisted(const JPH::Body & /*body_a*/,
                          const JPH::Body & /*body_b*/,
                          const JPH::ContactManifold & /*manifold*/,
                          JPH::ContactSettings & /*settings*/) override
  {
  }

  void OnContactRemoved(const JPH::SubShapeIDPair & /*sub_shape_pair*/) override {}
};

class BodyActivationListenerImpl : public JPH::BodyActivationListener {
 public:
  void OnBodyActivated(const JPH::BodyID & /*body_id*/, uint64_t /*body_user_data*/) override {}

  void OnBodyDeactivated(const JPH::BodyID & /*body_id*/, uint64_t /*body_user_data*/) override {}
};

struct JoltState {
  bool is_initialized = false;

  BroadPhaseLayerInterfaceImpl broad_phase_layer_interface;
  ObjectVsBroadPhaseLayerFilterImpl object_vs_broad_phase_layer_filter;
  ObjectLayerPairFilterImpl object_layer_pair_filter;
  BodyActivationListenerImpl body_activation_listener;
  ContactListenerImpl contact_listener;
  JPH::PhysicsSystem system;

  // Map<std::string, JoltRigidBodies> rigid_bodies_by_path;
  // CollisionShapeCache collision_shape_cache;
};

class JoltStateOwner : public BundleItemInternalValueMixin {
 public:
  mutable Mutex mutex;
  mutable JoltState state;

  void delete_self() override
  {
    MEM_delete(this);
  }

  StringRefNull type_name() const override
  {
    return TIP_("Jolt Physics State");
  }
};
using JoltStateOwnerPtr = ImplicitSharingPtr<JoltStateOwner>;

// class JoltSolverDataBundle {
//  public:
//   static constexpr StringRefNull name = "Blender.JoltSolverData";
//   static const FlatBundleTypePtr &get_bundle_type();
// };

// const FlatBundleTypePtr &JoltSolverDataBundle::get_bundle_type()
// {
//   static const FlatBundleTypePtr bundle_type = []() {
//     FlatBundleTypeBuilder b(JoltSolverDataBundle::name);
//     const FlatBundleTypePtr bundle_type = b.build();
//     BundleTypeRegistry::register_type(bundle_type);
//     return bundle_type;
//   }();
//   return bundle_type;
// }

#endif

struct GeometryData {
  bke::MutableAttributeAccessor attributes;
  AttrDomain domain;
  int size;

  bke::SpanAttributeWriter<float3> position_attr;
  bke::SpanAttributeWriter<float3> velocity_attr;
  bke::SpanAttributeWriter<math::Quaternion> rotation_attr;
  bke::SpanAttributeWriter<float3> angular_velocity_attr;

  GeometryData(const bke::MutableAttributeAccessor attributes) : attributes(attributes) {}
};

struct Geometries {
  Vector<simulation::GeometrySetData> geometry_sets;

  VectorSet<DataKey> data_keys;
  Vector<GeometryData *> data;
};

class JoltSolverStep {
 private:
  struct TLS {
    ResourceScope scope;
    LinearAllocator<> &allocator;

    TLS() : scope(1024), allocator(scope.allocator()) {}
  };

  threading::EnumerableThreadSpecific<TLS> tls_;

  /* Internal state of the Jolt simulation. */
  JoltState &state_;

  /** The simulation world that is being modified. */
  Bundle &world_;

  const float delta_time_;
  const int substeps_;

  const float interpolation_begin_;
  const float interpolation_end_;

  const float4x4 world_to_simulation_;

  std::string geometry_tag_filter_;

  Geometries geometries_;

 public:
  JoltSolverStep(JoltState &state,
                 Bundle &world,
                 const float total_delta_time,
                 const int substeps,
                 const float interpolation_begin,
                 const float interpolation_end,
                 const StringRef geometry_tag_filter,
                 const float4x4 &simulation_to_world)
      : state_(state),
        world_(world),
        delta_time_(total_delta_time),
        substeps_(substeps),
        interpolation_begin_(interpolation_begin),
        interpolation_end_(interpolation_end),
        world_to_simulation_(math::invert(simulation_to_world)),
        geometry_tag_filter_(geometry_tag_filter)
  {
  }

  void do_step()
  {
#ifdef WITH_JOLT
    if (!state_.is_initialized) {
      /* Defaults taken from Jolt's Hello World example, may need to be tweaked/dynamic over
       * time.*/
      const int max_bodies = 65536;
      const int num_body_mutexes = 0;
      const int max_body_pairs = 65536;
      const int max_contact_constraints = 10240;
      state_.system.Init(max_bodies,
                         num_body_mutexes,
                         max_body_pairs,
                         max_contact_constraints,
                         state_.broad_phase_layer_interface,
                         state_.object_vs_broad_phase_layer_filter,
                         state_.object_layer_pair_filter);
      state_.system.SetBodyActivationListener(&state_.body_activation_listener);
      state_.system.SetContactListener(&state_.contact_listener);
      state_.is_initialized = true;
    }

    // this->gather_nested_bundle_paths();
    this->gather_from_world__geometries();
    // this->prepare_geometry_chunks();
    // this->prepare_inverse_masses();
    // this->prepare_inverse_moments_of_inertia();

    // this->gather_from_world__mesh_colliders();
    // this->gather_from_world__stretch_shear_constraints();
    // this->gather_from_world__bend_twist_constraints();
    // this->gather_from_world__edge_length_constraints();
    // this->gather_from_world__cross_edge_length_constraints();
    // this->gather_from_world__damping();
    // this->gather_from_world__pin_positions();
    // this->gather_from_world__pin_rotations();
    // this->gather_from_world__collision_contacts();

    // this->parallel_for_each_chunk(32, [&](const int chunk_i) {
    //   TLS &tls = tls_.local();
    //   this->create_chunk_constraints__rod_stretch_shear(tls, chunk_i);
    //   this->create_chunk_constraints__rod_bend_twist(tls, chunk_i);
    //   this->create_chunk_constraints__damping(tls, chunk_i);
    //   this->create_chunk_constraints__pin_positions(tls, chunk_i);
    //   this->create_chunk_constraints__pin_rotations(tls, chunk_i);
    // });

    // result_ = this->do_simulation();

    JPH::TempAllocatorImpl temp_allocator(10 * 1024 * 1024);
    JPH::JobSystemSingleThreaded job_system;
    state_.system.Update(delta_time_, substeps_, &temp_allocator, &job_system);

    // this->write_back__pin_positions();
    // this->write_back__rod_stretch_shear();

    this->finish_common_attribute_writers();
    // this->write_back_geometries_to_world();

    // this->write_back__contacts();
#endif
  }

 private:
  void gather_from_world__geometries()
  {
    geometries_.geometry_sets = simulation::gather_geometry_set_data(world_, geometry_tag_filter_);

    TLS &tls = tls_.local();

    /* Gather individual components that should be simulated. There may be more than geometry sets
     * because each geometry set could contain e.g. a mesh and curves. */
    for (const int geo_bundle_i : geometries_.geometry_sets.index_range()) {
      GeometrySetData &geo_set_data = geometries_.geometry_sets[geo_bundle_i];
      GeometrySet &geometry = geo_set_data.geometry;
      for (bke::GeometryComponent::Type type : {bke::GeometryComponent::Type::Instance}) {
        if (!geometry.has(type)) {
          continue;
        }
        bke::GeometryComponent &component = geometry.get_component_for_write(type);
        geometries_.data_keys.add_new({geo_bundle_i, type, std::nullopt});
        GeometryData &geo_data = tls.scope.construct<GeometryData>(
            *component.attributes_for_write());
        geo_data.domain = AttrDomain::Instance;
        geometries_.data.append(&geo_data);
      }
    }

    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = *geometries_.data[data_key_i];
      const AttrDomain domain = geo_data.domain;
      geo_data.size = geo_data.attributes.domain_size(domain);
      // geo_data.temp_positions.reinitialize(geo_data.size);
      // geo_data.temp_rotations.reinitialize(geo_data.size);
      geo_data.position_attr = simulation::ensure_attribute<float3>(
          geo_data.attributes, attribute_names::position, domain);
      geo_data.velocity_attr = simulation::ensure_attribute<float3>(
          geo_data.attributes, attribute_names::velocity, domain);
      // geo_data.external_force_attr = this->lookup_attribute_default<float3>(
      //     data_key_i, attribute_names::external_force, domain, float3(0, 0, 0));
      // geo_data.masses = this->lookup_attribute_default<float>(
      //     data_key_i, attribute_names::mass, domain, 1.0f);

      geo_data.rotation_attr = simulation::ensure_attribute<math::Quaternion>(
          geo_data.attributes, attribute_names::rotation, domain);
      geo_data.angular_velocity_attr = simulation::ensure_attribute<float3>(
          geo_data.attributes, attribute_names::angular_velocity, domain);
      // geo_data.external_torque_attr = this->lookup_attribute_default<float3>(
      //     data_key_i, attribute_names::external_torque, domain, float3(0, 0, 0));
      // geo_data.moments_of_inertia = this->lookup_attribute_default<float3>(
      //     data_key_i, attribute_names::moment_of_inertia, domain, float3(1.0f));
    }
  }

  void finish_common_attribute_writers()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = *geometries_.data[data_key_i];

      geo_data.position_attr.finish();
      geo_data.velocity_attr.finish();
      geo_data.rotation_attr.finish();
      geo_data.angular_velocity_attr.finish();
    }
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_JOLT
  BundlePtr world_ptr = params.get_input<BundlePtr>("World"_ustr);
  if (!world_ptr) {
    params.set_default_remaining_outputs();
    return;
  }
  Bundle &world = world_ptr.ensure_mutable_inplace();

  geometry::jolt::ensure_initialization();

  const float delta_time = params.extract_input<float>("Delta Time"_ustr);
  const int substeps = params.extract_input<int>("Substeps"_ustr);
  const std::string geometry_tag_filter = params.extract_input<std::string>("Filter"_ustr);
  const float4x4 simulation_to_world = params.extract_input<float4x4>("Simulation to World"_ustr);
  const float interpolation_begin = params.extract_input<float>("Begin"_ustr);
  const float interpolation_end = params.extract_input<float>("End"_ustr);
  const std::string solver_path = params.extract_input<std::string>("Solver Path"_ustr);

  BundlePtr solver_data_ptr;
  if (!solver_path.empty()) {
    if (std::optional<BundlePtr> item = world.lookup_path<BundlePtr>(solver_path)) {
      solver_data_ptr = *item;
    }
  }
  else {
    params.error_message_add(NodeWarningType::Error, "Jolt solver requires valid data path");
  }
  if (!solver_data_ptr) {
    solver_data_ptr = Bundle::create();
  }
  Bundle &solver_data = solver_data_ptr.ensure_mutable_inplace();

  JoltStateOwnerPtr jolt_state_owner;
  if (std::optional item = solver_data.lookup_path<JoltStateOwnerPtr>("_state")) {
    jolt_state_owner = *item;
  }
  if (!jolt_state_owner) {
    jolt_state_owner = JoltStateOwnerPtr{MEM_new<JoltStateOwner>(__func__)};
  }

  if (jolt_state_owner->mutex.try_lock()) {
    JoltSolverStep jolt_step(jolt_state_owner->state,
                             world,
                             delta_time,
                             substeps,
                             interpolation_begin,
                             interpolation_end,
                             geometry_tag_filter,
                             simulation_to_world);
    jolt_state_owner->mutex.unlock();
  }
  else {
    params.error_message_add(NodeWarningType::Error,
                             TIP_("Jolt physics state cannot be used by multiple nodes"));
  }

  if (!solver_path.empty()) {
    // solver_data.add(Bundle::type_item_name, std::string(XPBDSolverDataBundle::name));
    // solver_data.add_path("residual_error", step.result().total_residual_error);
    solver_data.add_path("_state", jolt_state_owner);
    world.add_path_override(solver_path, std::move(solver_data_ptr));
  }

  params.set_output("World"_ustr, std::move(world_ptr));
#else
  params.set_default_remaining_outputs();
#endif
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeJoltSolver"_ustr);
  ntype.ui_name = "Jolt Solver";
  ntype.ui_description = "Physics simulation using the jolt solver";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.default_width = bke::NodeWidth::_200;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_jolt_solver_cc
