/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_rotation.hh"

#include "BKE_instances.hh"

#include "NOD_geo_tag_filter.hh"
#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_simulation.hh"

#include "node_geometry_util.hh"

#include "GEO_jolt.hh"

#include <fmt/format.h>

#ifdef WITH_JOLT

#  include <Jolt/Jolt.h>

#  include <Jolt/Core/JobSystemSingleThreaded.h>
#  include <Jolt/Physics/Body/BodyActivationListener.h>
#  include <Jolt/Physics/Body/BodyCreationSettings.h>
#  include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#  include <Jolt/Physics/Collision/ContactListener.h>
#  include <Jolt/Physics/Collision/ObjectLayer.h>
#  include <Jolt/Physics/PhysicsSystem.h>

#endif

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

struct JoltRigidBody {
  JPH::Body *body;
};

struct JoltRigidBodies {
  Map<int, JoltRigidBody> bodies_by_id;
};

struct JoltState {
  bool is_initialized = false;

  BroadPhaseLayerInterfaceImpl broad_phase_layer_interface;
  ObjectVsBroadPhaseLayerFilterImpl object_vs_broad_phase_layer_filter;
  ObjectLayerPairFilterImpl object_layer_pair_filter;
  BodyActivationListenerImpl body_activation_listener;
  ContactListenerImpl contact_listener;
  JPH::PhysicsSystem system;

  Map<DataKey, JoltRigidBodies> rigid_bodies_by_key;
  geometry::jolt::CollisionShapeCache collision_shape_cache;

  ~JoltState() = default;

  void ensure_initialize()
  {
    if (this->is_initialized) {
      return;
    }

    /* Defaults taken from Jolt's Hello World example, may need to be tweaked/dynamic over
     * time.*/
    const int max_bodies = 65536;
    const int num_body_mutexes = 0;
    const int max_body_pairs = 65536;
    const int max_contact_constraints = 10240;
    this->system.Init(max_bodies,
                      num_body_mutexes,
                      max_body_pairs,
                      max_contact_constraints,
                      this->broad_phase_layer_interface,
                      this->object_vs_broad_phase_layer_filter,
                      this->object_layer_pair_filter);
    this->system.SetBodyActivationListener(&this->body_activation_listener);
    this->system.SetContactListener(&this->contact_listener);
    this->is_initialized = true;
  }
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

  Span<bke::InstanceReference> references;
  Span<int> reference_handles;
  Span<int> instance_ids;

  bke::SpanAttributeWriter<float4x4> transform_attr;
  bke::SpanAttributeWriter<float3> velocity_attr;
  bke::SpanAttributeWriter<float3> angular_velocity_attr;

  VArray<int> collision_shape_types;
  VArray<int> motion_types;
  VArray<float> frictions;
  VArray<float> bouncinesses;
  VArray<float> densities;

  GeometryData(const bke::MutableAttributeAccessor attributes) : attributes(attributes) {}
};

struct Geometries {
  Vector<simulation::GeometrySetData> geometry_sets;

  /* Unique geometry components used by the simulation. */
  VectorSet<DataKey> data_keys;
  /* Data for each data key. */
  Vector<GeometryData *> data;
};

enum class CollisionShapeType {
  Box,
  Sphere,
  ConvexHull,
};

static std::optional<CollisionShapeType> parse_collision_shape_type(const int type)
{
  switch (type) {
    case 0:
      return CollisionShapeType::Box;
    case 1:
      return CollisionShapeType::Sphere;
    case 2:
      return CollisionShapeType::ConvexHull;
  }
  return std::nullopt;
}

static std::optional<JPH::EMotionType> parse_motion_type(const int type)
{
  switch (type) {
    case 0:
      return JPH::EMotionType::Dynamic;
    case 1:
      return JPH::EMotionType::Static;
    case 2:
      return JPH::EMotionType::Kinematic;
    default:
      return std::nullopt;
  }
}

static JPH::ShapeSettings::ShapeResult make_collision_shape(
    const CollisionShapeType type,
    const geometry::jolt::CollisionShapeParams &params,
    geometry::jolt::CollisionShapeCache &cache)
{
  switch (type) {
    case CollisionShapeType::Box: {
      return cache.get_or_create_box(params);
    }
    case CollisionShapeType::Sphere: {
      return cache.get_or_create_sphere(params);
    }
    case CollisionShapeType::ConvexHull: {
      return cache.get_or_create_convex_hull(params);
    }
  }
  BLI_assert_unreachable();
  return {};
}

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

  Mutex warnings_mutex_;
  VectorSet<std::pair<NodeWarningType, std::string>> warnings_;

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

    this->update_jolt_state_from_instances();
    JPH::TempAllocatorImpl temp_allocator(10 * 1024 * 1024);
    JPH::JobSystemSingleThreaded job_system;
    state_.system.Update(delta_time_, substeps_, &temp_allocator, &job_system);
    this->update_instances_from_jolt_state();

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
      if (geometry.has_instances()) {
        geometries_.data_keys.add_new(
            {geo_bundle_i, bke::GeometryComponent::Type::Instance, std::nullopt});

        bke::Instances &instances = *geometry.get_instances_for_write();
        GeometryData &geo_data = tls.scope.construct<GeometryData>(
            instances.attributes_for_write());
        geo_data.domain = AttrDomain::Instance;
        geo_data.instance_ids = instances.unique_ids();
        geo_data.references = instances.references();
        geo_data.reference_handles = instances.reference_handles();

        geometries_.data.append(&geo_data);
      }
    }

    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = *geometries_.data[data_key_i];
      const AttrDomain domain = geo_data.domain;
      geo_data.size = geo_data.attributes.domain_size(domain);

      // geo_data.temp_positions.reinitialize(geo_data.size);
      // geo_data.temp_rotations.reinitialize(geo_data.size);

      geo_data.transform_attr = simulation::ensure_attribute<float4x4>(
          geo_data.attributes, attribute_names::instance_transform, domain);
      geo_data.velocity_attr = simulation::ensure_attribute<float3>(
          geo_data.attributes, attribute_names::velocity, domain);
      geo_data.angular_velocity_attr = simulation::ensure_attribute<float3>(
          geo_data.attributes, attribute_names::angular_velocity, domain);

      // geo_data.external_force_attr = this->lookup_attribute_default<float3>(
      //     data_key_i, attribute_names::external_force, domain, float3(0, 0, 0));
      // geo_data.masses = this->lookup_attribute_default<float>(
      //     data_key_i, attribute_names::mass, domain, 1.0f);
      // geo_data.external_torque_attr = this->lookup_attribute_default<float3>(
      //     data_key_i, attribute_names::external_torque, domain, float3(0, 0, 0));
      // geo_data.moments_of_inertia = this->lookup_attribute_default<float3>(
      //     data_key_i, attribute_names::moment_of_inertia, domain, float3(1.0f));

      geo_data.collision_shape_types = this->lookup_attribute_default<int>(
          data_key_i, attribute_names::collision_shape_type, geo_data.domain, 0);
      geo_data.motion_types = this->lookup_attribute_default<int>(
          data_key_i, attribute_names::motion_type, geo_data.domain, 0.0f);
      geo_data.frictions = this->lookup_attribute_default<float>(
          data_key_i, attribute_names::static_friction, geo_data.domain, 0.5f);
      geo_data.bouncinesses = this->lookup_attribute_default<float>(
          data_key_i, attribute_names::bounciness, geo_data.domain, 0.5f);
      geo_data.densities = this->lookup_attribute_default<float>(
          data_key_i, attribute_names::density, geo_data.domain, 1.0f);
    }
  }

  void update_jolt_state_from_instances()
  {
    state_.collision_shape_cache.reset_used();

    /* XXX In the earlier implementation the instance transforms had to be updated first because
     * field evaluation could depend on current state.
     * It should not be necessary any more because geometry attributes are transferred from the
     * cache to the new world input outside of the solver. */
    // update_instances_from_jolt_state();

    Map<DataKey, JoltRigidBodies> new_rigid_bodies_by_key;
    JPH::BodyInterface &body_interface = state_.system.GetBodyInterfaceNoLock();

    Vector<JPH::BodyID> bodies_to_remove;
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      const DataKey &data_key = geometries_.data_keys[data_key_i];
      const GeometryData &geo_data = *geometries_.data[data_key_i];

      JoltRigidBodies *old_rigid_bodies = state_.rigid_bodies_by_key.lookup_ptr(data_key);
      JoltRigidBodies new_rigid_bodies = this->update_rigid_bodies(geo_data, old_rigid_bodies);

      if (old_rigid_bodies) {
        bodies_to_remove.reserve(bodies_to_remove.size() + old_rigid_bodies->bodies_by_id.size());
        for (JoltRigidBody &body : old_rigid_bodies->bodies_by_id.values()) {
          bodies_to_remove.append_unchecked(body.body->GetID());
        }
      }
    }

    /* Remove old rigid bodies. */
    body_interface.RemoveBodies(bodies_to_remove.data(), bodies_to_remove.size());
    body_interface.DestroyBodies(bodies_to_remove.data(), bodies_to_remove.size());

    state_.rigid_bodies_by_key = std::move(new_rigid_bodies_by_key);
    state_.collision_shape_cache.remove_unused();

    // update_gravity(params, state, behaviors);
    // update_forces(state, behaviors);
  }

  JoltRigidBodies update_rigid_bodies(const GeometryData &geo_data,
                                      JoltRigidBodies *old_rigid_bodies)
  {
    JPH::BodyInterface &body_interface = state_.system.GetBodyInterfaceNoLock();

    // const bke::Instances *instances = behavior.simulated_geometry.get_instances();
    // if (!instances) {
    //   return;
    // }
    // const int instances_num = instances->instances_num();
    // const int references_num = instances->references_num();
    // const Span<int> instance_ids = instances->almost_unique_ids();
    // const Span<float4x4> transforms = instances->transforms();
    // const Span<bke::InstanceReference> references = instances->references();
    // const Span<int> handles = instances->reference_handles();

    // bke::InstancesFieldContext field_context{*instances};
    // fn::FieldEvaluator field_evaluator{field_context, instances_num};
    // field_evaluator.add(behavior.collision_shape_type_field);
    // field_evaluator.add(behavior.motion_type_field);
    // field_evaluator.add(behavior.friction_field);
    // field_evaluator.add(behavior.bounciness_field);
    // field_evaluator.add(behavior.density_field);
    // field_evaluator.evaluate();
    // const VArray<int> collision_shape_types = field_evaluator.get_evaluated<int>(0);
    // const VArray<int> motion_types = field_evaluator.get_evaluated<int>(1);
    // const VArray<float> frictions = field_evaluator.get_evaluated<float>(2);
    // const VArray<float> bouncinesses = field_evaluator.get_evaluated<float>(3);
    // const VArray<float> densities = field_evaluator.get_evaluated<float>(4);

    Array<GeometrySet> reference_geometry_sets(geo_data.references.size());
    for (const int i : geo_data.references.index_range()) {
      const bke::InstanceReference &reference = geo_data.references[i];
      GeometrySet reference_geometry;
      reference.to_geometry_set(reference_geometry);
      reference_geometry_sets[i] = std::move(reference_geometry);
    }

    JoltRigidBodies rigid_bodies;
    for (const int instance_i : geo_data.instance_ids.index_range()) {
      const int reference_i = geo_data.reference_handles[instance_i];
      const int instance_id = geo_data.instance_ids[instance_i];
      if (!reference_geometry_sets.index_range().contains(reference_i)) {
        continue;
      }
      const std::optional<CollisionShapeType> collision_shape_type = parse_collision_shape_type(
          geo_data.collision_shape_types[instance_i]);
      if (!collision_shape_type) {
        continue;
      }
      const std::optional<JPH::EMotionType> motion_type = parse_motion_type(
          geo_data.motion_types[instance_i]);
      if (!motion_type) {
        continue;
      }
      const GeometrySet &reference_geometry = reference_geometry_sets[reference_i];
      const std::optional<Bounds<float3>> bounds =
          reference_geometry.compute_boundbox_without_instances(true);
      if (!bounds) {
        continue;
      }
      const float4x4 &instance_transform = geo_data.transform_attr.span[instance_i];
      float3 instance_position;
      math::Quaternion instance_rotation;
      float3 instance_scale;
      math::to_loc_rot_scale_safe<true>(
          instance_transform, instance_position, instance_rotation, instance_scale);

      float density = geo_data.densities[instance_i];
      if (density <= 0.0f) {
        density = 1.0f;
      }

      const geometry::jolt::CollisionShapeParams collision_shape_params{
          reference_geometry, instance_scale, density};
      JPH::ShapeSettings::ShapeResult collision_shape = make_collision_shape(
          *collision_shape_type, collision_shape_params, state_.collision_shape_cache);
      if (!collision_shape.IsValid()) {
        continue;
      }

      const float friction = geo_data.frictions[instance_i];
      const float bounciness = geo_data.bouncinesses[instance_i];

      std::optional<JoltRigidBody> rigid_body;
      if (old_rigid_bodies) {
        if (std::optional<JoltRigidBody> old_rigid_body = old_rigid_bodies->bodies_by_id.pop_try(
                instance_id))
        {
          rigid_body = old_rigid_body;
        }
      }
      if (!rigid_body) {
        JPH::BodyCreationSettings jolt_body_settings{
            collision_shape.Get(),
            geometry::jolt::convert_dvec3(instance_position),
            geometry::jolt::convert_quat(instance_rotation),
            *motion_type,
            *motion_type == JPH::EMotionType::Dynamic ? ObjectLayers::moving :
                                                        ObjectLayers::non_moving};

        JPH::Body *jolt_body = body_interface.CreateBody(jolt_body_settings);
        body_interface.AddBody(jolt_body->GetID(), JPH::EActivation::Activate);
        rigid_body = JoltRigidBody{jolt_body};
      }

      body_interface.SetShape(
          rigid_body->body->GetID(), collision_shape.Get(), true, JPH::EActivation::Activate);
      rigid_body->body->SetFriction(friction);
      rigid_body->body->SetRestitution(bounciness);

      if (motion_type == JPH::EMotionType::Kinematic) {
        body_interface.SetPositionAndRotationWhenChanged(
            rigid_body->body->GetID(),
            geometry::jolt::convert_dvec3(instance_position),
            geometry::jolt::convert_quat(instance_rotation),
            JPH::EActivation::Activate);
      }

      rigid_bodies.bodies_by_id.add(instance_id, std::move(*rigid_body));
    }

    return rigid_bodies;
  }

  void update_instances_from_jolt_state()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      const DataKey &data_key = geometries_.data_keys[data_key_i];
      const JoltRigidBodies *rigid_bodies = state_.rigid_bodies_by_key.lookup_ptr(data_key);
      if (!rigid_bodies) {
        continue;
      }

      GeometryData &geo_data = *geometries_.data[data_key_i];

      for (const int instance_i : geo_data.instance_ids.index_range()) {
        const int instance_id = geo_data.instance_ids[instance_i];
        const JoltRigidBody *rigid_body = rigid_bodies->bodies_by_id.lookup_ptr(instance_id);
        if (!rigid_body) {
          continue;
        }
        const JPH::Body &jolt_body = *rigid_body->body;

        float4x4 &transform = geo_data.transform_attr.span[instance_i];
        const JPH::DVec3 jolt_position = jolt_body.GetPosition();
        const JPH::Quat jolt_rotation = jolt_body.GetRotation();

        const float3 position = geometry::jolt::convert_dvec3(jolt_position);
        const math::Quaternion rotation = geometry::jolt::convert_quat(jolt_rotation);

        /* Scale is not simulated to Jolt, so keep the scale of the original geometry. */
        const float3 scale = math::to_scale(transform);

        transform = math::from_loc_rot_scale<float4x4>(position, rotation, scale);
      }
    }
  }

  void finish_common_attribute_writers()
  {
    for (const int data_key_i : geometries_.data_keys.index_range()) {
      GeometryData &geo_data = *geometries_.data[data_key_i];

      geo_data.transform_attr.finish();
      geo_data.velocity_attr.finish();
      geo_data.angular_velocity_attr.finish();
    }
  }

  template<typename T>
  VArray<T> lookup_attribute_default(const int data_key_i,
                                     const StringRef attribute_name,
                                     const AttrDomain domain,
                                     const T &default_value)
  {
    GeometryData &geo_data = *geometries_.data[data_key_i];
    VArray<T> varray = *geo_data.attributes.lookup<T>(attribute_name, domain);
    if (!varray) {
      this->report(
          NodeWarningType::Info,
          fmt::format(
              "Attribute \"{}\" not found in \"{}\", using fallback value",
              attribute_name,
              geometries_.geometry_sets[geometries_.data_keys[data_key_i].geo_bundle_i].path));
      return VArray<T>::from_single(default_value, geo_data.attributes.domain_size(domain));
    }
    return varray;
  }

  template<typename T>
  VArray<T> lookup_attribute_required(const int data_key_i,
                                      const StringRef attribute_name,
                                      const AttrDomain domain)
  {
    GeometryData &geo_data = *geometries_.data[data_key_i];
    VArray<T> varray = *geo_data.attributes.lookup<T>(attribute_name, domain);
    if (!varray) {
      this->report(
          NodeWarningType::Error,
          fmt::format(
              "Attribute \"{}\" not found on \"{}\", some functionality is disabled",
              attribute_name,
              geometries_.geometry_sets[geometries_.data_keys[data_key_i].geo_bundle_i].path));
    }
    return varray;
  }

  template<typename T>
  VArray<T> lookup_attribute_optional(const int data_key_i,
                                      const StringRef attribute_name,
                                      const AttrDomain domain)
  {
    GeometryData &geo_data = *geometries_.data[data_key_i];
    VArray<T> varray = *geo_data.attributes.lookup<T>(attribute_name, domain);
    if (!varray) {
      this->report(
          NodeWarningType::Info,
          fmt::format(
              "Optional attribute \"{}\" not found on \"{}\"",
              attribute_name,
              geometries_.geometry_sets[geometries_.data_keys[data_key_i].geo_bundle_i].path));
    }
    return varray;
  }

  void report(NodeWarningType type, std::string warning)
  {
    std::lock_guard<Mutex> lock(warnings_mutex_);
    warnings_.add({type, std::move(warning)});
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
    jolt_state_owner->state.ensure_initialize();

    JoltSolverStep jolt_step(jolt_state_owner->state,
                             world,
                             delta_time,
                             substeps,
                             interpolation_begin,
                             interpolation_end,
                             geometry_tag_filter,
                             simulation_to_world);
    jolt_step.do_step();
    jolt_state_owner->mutex.unlock();
  }
  else {
    params.error_message_add(NodeWarningType::Error,
                             TIP_("Jolt physics state cannot be used by multiple nodes"));
  }

  if (!solver_path.empty()) {
    // solver_data.add(Bundle::type_item_name, std::string(XPBDSolverDataBundle::name));
    // solver_data.add_path("residual_error", step.result().total_residual_error);
    solver_data.add_path("_state", std::move(jolt_state_owner));
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
