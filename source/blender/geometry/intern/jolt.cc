/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_JOLT

/* Has to be included before the other Jolt headers.*/
#  include <Jolt/Jolt.h>

#  include <Jolt/Core/Factory.h>
#  include <Jolt/RegisterTypes.h>

#  include <Jolt/Physics/Collision/Shape/BoxShape.h>
#  include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#  include <Jolt/Physics/Collision/Shape/SphereShape.h>

#  include "BLI_bounds.hh"

#  include "BKE_geometry_set.hh"
#  include "BKE_mesh.hh"

#  include "MEM_guardedalloc.h"

#  include "GEO_jolt.hh"

#  include <xxhash.h>

namespace blender::geometry::jolt {

struct JoltStartupAndExit {
  JoltStartupAndExit()
  {
    initialize_jolt_allocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
  }

  static void initialize_jolt_allocator()
  {
    constexpr const char *func = __func__;
    JPH::Allocate = [](size_t size) {
      /* Jolt requires 16-byte alignment when doing a normal allocation. */
      return MEM_new_uninitialized_aligned(size, 16, func);
    };
    JPH::Reallocate = [](void *mem, size_t /*old_size*/, size_t new_size) {
      if (mem == nullptr) {
        return JPH::Allocate(new_size);
      }
      return MEM_realloc_uninitialized_id(mem, new_size, func);
    };
    JPH::Free = [](void *mem) { MEM_delete_void(mem); };
    JPH::AlignedAllocate = [](size_t size, size_t alignment) {
      return MEM_new_uninitialized_aligned(size, alignment, func);
    };
    JPH::AlignedFree = [](void *mem) { MEM_delete_void(mem); };
  }

  ~JoltStartupAndExit()
  {
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
  }
};

void ensure_initialization()
{
  static JoltStartupAndExit jolt_startup_and_exit;
}

GeometryShapeHash GeometryShapeHash::from_geometry(const bke::GeometrySet &geometry_set)
{
  XXH3_state_t *hash_state = XXH3_createState();
  XXH3_128bits_reset(hash_state);

  if (const Mesh *mesh = geometry_set.get_mesh()) {
    const Span<float3> positions = mesh->vert_positions();
    XXH3_128bits_update(hash_state, positions.data(), positions.size_in_bytes());
  }

  const XXH128_hash_t digest = XXH3_128bits_digest(hash_state);
  XXH3_freeState(hash_state);

  GeometryShapeHash final_hash;
  static_assert(sizeof(final_hash) == sizeof(digest));
  memcpy(reinterpret_cast<void *>(&final_hash), &digest, sizeof(digest));
  return final_hash;
}

void CollisionShapeCache::reset_used()
{
  for (CachedShape &cached_shape : this->boxes.values()) {
    cached_shape.still_used = false;
  }
  for (CachedShape &cached_shape : this->spheres.values()) {
    cached_shape.still_used = false;
  }
  for (CachedShape &cached_shape : this->convex_hulls.values()) {
    cached_shape.still_used = false;
  }
}

void CollisionShapeCache::remove_unused()
{
  this->boxes.remove_if([](const auto &item) { return !item.value.still_used; });
  this->spheres.remove_if([](const auto &item) { return !item.value.still_used; });
  this->convex_hulls.remove_if([](const auto &item) { return !item.value.still_used; });
}

JPH::ShapeSettings::ShapeResult CollisionShapeCache::get_or_create_box(
    const CollisionShapeParams &params)
{
  const std::optional<Bounds<float3>> bounds = params.geometry.compute_boundbox_without_instances(
      true);
  if (!bounds) {
    return {};
  }
  const float3 half_extent = math::max(math::abs(bounds->min), math::abs(bounds->max)) *
                             params.scale;
  return this->get_or_create_box(half_extent, params.density);
}

JPH::ShapeSettings::ShapeResult CollisionShapeCache::get_or_create_box(const float3 &half_extent,
                                                                       const float density)
{
  const BoxID box_id{half_extent, density};
  CachedShape &cached_shape = this->boxes.lookup_or_add_cb(box_id, [&]() {
    JPH::BoxShapeSettings box_shape_settings{convert_vec3(half_extent)};
    box_shape_settings.mDensity = density;
    const float min_axis = std::min({half_extent.x, half_extent.y, half_extent.z});
    box_shape_settings.mConvexRadius = 0.5 * std::min(min_axis, 0.1f);
    return box_shape_settings.Create();
  });
  if (cached_shape.shape.IsValid()) {
    cached_shape.still_used = true;
  }
  return cached_shape.shape;
}

JPH::ShapeSettings::ShapeResult CollisionShapeCache::get_or_create_sphere(
    const CollisionShapeParams &params)
{
  const std::optional<Bounds<float3>> bounds = params.geometry.compute_boundbox_without_instances(
      true);
  if (!bounds) {
    return {};
  }
  const float3 half_extent = math::max(math::abs(bounds->min), math::abs(bounds->max)) *
                             params.scale;
  const float radius = std::max({half_extent.x, half_extent.y, half_extent.z});
  return this->get_or_create_sphere(radius, params.density);
}

JPH::ShapeSettings::ShapeResult CollisionShapeCache::get_or_create_sphere(const float radius,
                                                                          const float density)
{
  const SphereID sphere_id{radius, density};
  CachedShape &cached_shape = this->spheres.lookup_or_add_cb(sphere_id, [&]() {
    JPH::SphereShapeSettings sphere_shape_settings{radius};
    sphere_shape_settings.mDensity = density;
    return sphere_shape_settings.Create();
  });
  if (cached_shape.shape.IsValid()) {
    cached_shape.still_used = true;
  }
  return cached_shape.shape;
}

JPH::ShapeSettings::ShapeResult CollisionShapeCache::get_or_create_convex_hull(
    const CollisionShapeParams &params)
{
  const auto shape_hash = GeometryShapeHash::from_geometry(params.geometry);
  const ConvexHullID convex_hull_id{shape_hash, params.scale, params.density};
  CachedShape &cached_shape = this->convex_hulls.lookup_or_add_cb(
      convex_hull_id, [&]() -> CachedShape {
        Vector<JPH::Vec3, 0, GuardedAlignedAllocator<>> points;
        if (const Mesh *mesh = params.geometry.get_mesh()) {
          points.reserve(points.size() + mesh->verts_num);
          const Span<float3> positions = mesh->vert_positions();
          for (const int i : positions.index_range()) {
            points.append(convert_vec3(positions[i] * params.scale));
          }
        }
        if (points.is_empty()) {
          return {};
        }
        JPH::ConvexHullShapeSettings hull_settings(points.data(), points.size(), 0.0f);
        hull_settings.mDensity = params.density;
        return hull_settings.Create();
      });
  if (cached_shape.shape.IsValid()) {
    cached_shape.still_used = true;
  }
  return cached_shape.shape;
}

}  // namespace blender::geometry::jolt

#endif
