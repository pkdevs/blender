/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#ifdef WITH_JOLT

#  include "BLI_map.hh"
#  include "BLI_math_quaternion_types.hh"
#  include "BLI_math_vector_types.hh"

#  include <Jolt/Jolt.h>
#  include <Jolt/Physics/Collision/Shape/Shape.h>

namespace blender::bke {
struct GeometrySet;
}

namespace blender::geometry::jolt {

/** Makes sure that Jolt is properly initialized so that its API can be used. */
void ensure_initialization();

inline JPH::Vec3 convert_vec3(const float3 &v)
{
  return JPH::Vec3(v.x, v.y, v.z);
}
inline float3 convert_vec3(const JPH::Vec3 &v)
{
  return float3(v.GetX(), v.GetY(), v.GetZ());
}
inline JPH::DVec3 convert_dvec3(const float3 &v)
{
  return JPH::RVec3(v.x, v.y, v.z);
}
inline float3 convert_dvec3(const JPH::DVec3 &v)
{
  return float3(v.GetX(), v.GetY(), v.GetZ());
}
inline JPH::Quat convert_quat(const math::Quaternion &q)
{
  return JPH::Quat(q.x, q.y, q.z, q.w);
}
inline math::Quaternion convert_quat(const JPH::Quat &q)
{
  return math::Quaternion(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
}

struct CollisionShapeParams {
  const bke::GeometrySet &geometry;
  const float3 &scale;
  const float density;
};

class GeometryShapeHash {
 private:
  uint64_t v1_ = 0;
  uint64_t v2_ = 0;

 public:
  GeometryShapeHash() = default;

  static GeometryShapeHash from_geometry(const bke::GeometrySet &geometry_set);

  uint64_t hash() const
  {
    return v1_;
  }

  bool operator==(const GeometryShapeHash &) const = default;
};

struct CollisionShapeCache {
  struct CachedShape {
    JPH::ShapeSettings::ShapeResult shape;
    bool still_used = true;

    CachedShape(JPH::ShapeSettings::ShapeResult shape = {}) : shape(std::move(shape)) {}
  };

  struct BoxID {
    float3 half_extent;
    float density;

    uint64_t hash() const
    {
      return get_default_hash(this->half_extent, this->density);
    }

    bool operator==(const BoxID &) const = default;
  };

  struct SphereID {
    float radius;
    float density;

    uint64_t hash() const
    {
      return get_default_hash(this->radius, this->density);
    }

    bool operator==(const SphereID &) const = default;
  };

  struct ConvexHullID {
    GeometryShapeHash shape_hash;
    float3 scale;
    float density;

    uint64_t hash() const
    {
      return get_default_hash(this->shape_hash, this->scale, this->density);
    }

    bool operator==(const ConvexHullID &) const = default;
  };

  Map<BoxID, CachedShape> boxes;
  Map<SphereID, CachedShape> spheres;
  Map<ConvexHullID, CachedShape> convex_hulls;

  void reset_used();
  void remove_unused();

  JPH::ShapeSettings::ShapeResult get_or_create_box(const CollisionShapeParams &params);
  JPH::ShapeSettings::ShapeResult get_or_create_box(const float3 &half_extent,
                                                    const float density);
  JPH::ShapeSettings::ShapeResult get_or_create_sphere(const CollisionShapeParams &params);
  JPH::ShapeSettings::ShapeResult get_or_create_sphere(const float radius, const float density);
  JPH::ShapeSettings::ShapeResult get_or_create_convex_hull(const CollisionShapeParams &params);
};

#endif

}  // namespace blender::geometry::jolt
