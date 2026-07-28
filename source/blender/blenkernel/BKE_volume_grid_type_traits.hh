/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#ifdef WITH_OPENVDB

#  include "BLI_math_matrix_types.hh"
#  include "BLI_math_vector_types.hh"

#  include "BKE_volume_enums.hh"
#  include "openvdb_fwd.hh"

namespace blender::bke {

/**
 * We uses the native math types from OpenVDB when working with grids. For example, vector grids
 * contain `openvdb::Vec3f`. #VolumeGridTraits allows mapping between Blender's math types and the
 * ones from OpenVDB. This allows e.g. using `VolumeGrid<float3>` when the actual grid is a
 * `openvdb::Vec3SGrid`. The benefit of this is that most places in Blender can keep using our own
 * types, while only the code that deals with OpenVDB specifically has to care about the mapping
 * between math type representations.
 *
 * \tparam T: The Blender type that we want to get the grid traits for (e.g. `float3`).
 */
template<typename T> struct VolumeGridTraits {
  /**
   * The type that Blender uses to represent values of the voxel type (e.g. `float3`).
   */
  using BlenderType = void;
  /**
   * The type that OpenVDB uses.
   */
  using PrimitiveType = void;
  /**
   * The standard tree type we use for grids of the given type.
   */
  using TreeType = void;
  /**
   * The corresponding #VolumeGridType for the type.
   */
  static constexpr VolumeGridType EnumType = VOLUME_GRID_UNKNOWN;
};

template<> struct VolumeGridTraits<bool> {
  using BlenderType = bool;
  using PrimitiveType = bool;
  using TreeType = openvdb::BoolTree;
  static constexpr VolumeGridType EnumType = VOLUME_GRID_BOOLEAN;

  static bool to_openvdb(const bool &value)
  {
    return value;
  }
  static bool to_blender(const bool &value)
  {
    return value;
  }
};

template<> struct VolumeGridTraits<int> {
  using BlenderType = int;
  using PrimitiveType = int;
  using TreeType = openvdb::Int32Tree;
  static constexpr VolumeGridType EnumType = VOLUME_GRID_INT;

  static int to_openvdb(const int &value)
  {
    return value;
  }
  static int to_blender(const int &value)
  {
    return value;
  }
};

template<> struct VolumeGridTraits<float> {
  using BlenderType = float;
  using PrimitiveType = float;
  using TreeType = openvdb::FloatTree;
  static constexpr VolumeGridType EnumType = VOLUME_GRID_FLOAT;

  static float to_openvdb(const float &value)
  {
    return value;
  }
  static float to_blender(const float &value)
  {
    return value;
  }
};

template<> struct VolumeGridTraits<float3> {
  using BlenderType = float3;
  using PrimitiveType = openvdb::Vec3f;
  using TreeType = openvdb::Vec3STree;
  static constexpr VolumeGridType EnumType = VOLUME_GRID_VECTOR_FLOAT;

  static openvdb::Vec3f to_openvdb(const float3 &value)
  {
    return openvdb::Vec3f(value.x, value.y, value.z);
  }
  static float3 to_blender(const openvdb::Vec3f &value)
  {
    return float3(value.asV());
  }
};

template<> struct VolumeGridTraits<float3x3> {
  using BlenderType = float3x3;
  using PrimitiveType = openvdb::Mat3s;
  using TreeType = void;
  static constexpr VolumeGridType EnumType = VOLUME_GRID_UNKNOWN;

  static openvdb::Mat3s to_openvdb(const float3x3 &value)
  {
    /* float3x3 accessor returns columns, Mat3s expects values in row-major form. */
    return openvdb::Mat3s(value[0][0],
                          value[1][0],
                          value[2][0],
                          value[0][1],
                          value[1][1],
                          value[2][1],
                          value[0][2],
                          value[1][2],
                          value[2][2]);
  }
  static float3x3 to_blender(const openvdb::Mat3s &value)
  {
    /* float3x3 constructor takes column vectors. */
    return float3x3(float3(value(0, 0), value(1, 0), value(2, 0)),
                    float3(value(0, 1), value(1, 1), value(2, 1)),
                    float3(value(0, 2), value(1, 2), value(2, 2)));
  }
};

template<> struct VolumeGridTraits<float4x4> {
  using BlenderType = float4x4;
  using PrimitiveType = openvdb::Mat4s;
  using TreeType = void;
  static constexpr VolumeGridType EnumType = VOLUME_GRID_UNKNOWN;

  static openvdb::Mat4s to_openvdb(const float4x4 &value)
  {
    /* float4x4 accessor returns columns, Mat4s expects values in row-major form. */
    return openvdb::Mat4s(value[0][0],
                          value[1][0],
                          value[2][0],
                          value[3][0],
                          value[0][1],
                          value[1][1],
                          value[2][1],
                          value[3][1],
                          value[0][2],
                          value[1][2],
                          value[2][2],
                          value[3][2],
                          value[0][3],
                          value[1][3],
                          value[2][3],
                          value[3][3]);
  }
  static float4x4 to_blender(const openvdb::Mat4s &value)
  {
    /* float3x3 constructor takes column vectors. */
    return float4x4(float4(value(0, 0), value(1, 0), value(2, 0), value(3, 0)),
                    float4(value(0, 1), value(1, 1), value(2, 1), value(3, 1)),
                    float4(value(0, 2), value(1, 2), value(2, 2), value(3, 2)),
                    float4(value(0, 3), value(1, 3), value(2, 3), value(3, 3)));
  }
};

template<typename T> using OpenvdbTreeType = typename VolumeGridTraits<T>::TreeType;
template<typename T> using OpenvdbGridType = openvdb::Grid<OpenvdbTreeType<T>>;

}  // namespace blender::bke

#endif /* WITH_OPENVDB */
