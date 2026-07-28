/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_index_range.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/Interpolation.h>
#endif

namespace blender::geometry {

#ifdef WITH_OPENVDB

namespace grid_sampling {

template<typename T> struct GradientTypeTraits {
  using type = void;
};
template<> struct GradientTypeTraits<float> {
  using type = float3;
};
template<> struct GradientTypeTraits<float3> {
  using type = float3x3;
};

template<typename T> struct OpenvdbGradientTypeTraits {
  using type = void;
};
template<> struct OpenvdbGradientTypeTraits<float> {
  using type = openvdb::Vec3s;
};
template<> struct OpenvdbGradientTypeTraits<openvdb::Vec3s> {
  using type = openvdb::Mat3s;
};

template<typename T> struct DivergenceTypeTraits {
  using type = void;
};
template<> struct DivergenceTypeTraits<float3> {
  using type = float;
};
template<> struct DivergenceTypeTraits<float3x3> {
  using type = float3;
};
template<> struct DivergenceTypeTraits<float4x4> {
  using type = float3;
};

template<typename T> struct OpenvdbDivergenceTypeTraits {
  using type = void;
};
template<> struct OpenvdbDivergenceTypeTraits<openvdb::Vec3s> {
  using type = float;
};
template<> struct OpenvdbDivergenceTypeTraits<openvdb::Mat3s> {
  using type = openvdb::Vec3s;
};
template<> struct OpenvdbDivergenceTypeTraits<openvdb::Mat4s> {
  using type = openvdb::Vec3s;
};

template<typename T> using GradientType = typename GradientTypeTraits<T>::type;
template<typename T> using OpenvdbGradientType = typename OpenvdbGradientTypeTraits<T>::type;
template<typename T> using DivergenceType = typename DivergenceTypeTraits<T>::type;
template<typename T> using OpenvdbDivergenceType = typename OpenvdbDivergenceTypeTraits<T>::type;

template<int N, class AccessorT>
inline bool probe_values(const AccessorT &accessor,
                         const openvdb::CoordBBox &index_box,
                         typename AccessorT::ValueType (&data)[N][N][N])
{
  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  bool any_active = false;
  for (const int dx : IndexRange(N)) {
    for (const int dy : IndexRange(N)) {
      for (const int dz : IndexRange(N)) {
        if (accessor.probeValue(index_box.getStart() + openvdb::Coord(dx, dy, dz),
                                data[dx][dy][dz]))
        {
          any_active = true;
        }
      }
    }
  }
  return any_active;
}

template<int N, class AccessorT>
inline void get_values(const AccessorT &accessor,
                       const openvdb::CoordBBox &index_box,
                       typename AccessorT::ValueType (&data)[N][N][N])
{
  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  for (const int dx : IndexRange(N)) {
    for (const int dy : IndexRange(N)) {
      for (const int dz : IndexRange(N)) {
        data[dx][dy][dz] = accessor.getValue(index_box.getStart() + openvdb::Coord(dx, dy, dz));
      }
    }
  }
}

/* Interpolate values in 3D using a function that combines a 1-dimensional array. */
template<typename ValueT, int N, typename KernelFn>
inline void interpolate_value_3d(ValueT (&data)[N][N][N],
                                 const openvdb::Vec3R &uvw,
                                 KernelFn kernel_fn,
                                 ValueT &result)
{
  ValueT vx[N];
  for (const int dx : IndexRange(N)) {
    ValueT vy[N];
    for (const int dy : IndexRange(N)) {
      const ValueT *vz = &data[dx][dy][0];
      vy[dy] = kernel_fn(vz, uvw.z());
    }
    vx[dx] = kernel_fn(vy, uvw.y());
  }
  result = kernel_fn(vx, uvw.x());
}

/* Interpolate a gradient in 3D using a function that combines a 1-dimensional array and its
 * derivative function. */
template<typename ValueT, int N, typename KernelFn, typename DerivativeFn>
void interpolate_gradient_3d(ValueT (&data)[N][N][N],
                             const openvdb::Vec3R &uvw,
                             const openvdb::math::Transform &transform,
                             KernelFn kernel_fn,
                             DerivativeFn derivative_fn,
                             OpenvdbGradientType<ValueT> &result)
{
  using GradientT = OpenvdbGradientType<ValueT>;

  ValueT vvx[N], vgx[N], gvx[N];
  for (int dx = 0; dx < N; ++dx) {
    ValueT vy[N], gy[N];
    for (int dy = 0; dy < N; ++dy) {
      const ValueT *vz = &data[dx][dy][0];
      vy[dy] = kernel_fn(vz, uvw.z());
      gy[dy] = derivative_fn(vz, uvw.z());
    }
    vvx[dx] = kernel_fn(vy, uvw.y());
    vgx[dx] = kernel_fn(gy, uvw.y());
    gvx[dx] = derivative_fn(vy, uvw.y());
  }
  const openvdb::math::Mat3 matrix =
      transform.baseMap()->getAffineMap()->getMat4().getMat3().inverse();
  if constexpr (std::is_same_v<GradientT, openvdb::Mat3s>) {
    /* Gradient of vectors is constructed by outer product with the weights gradient:
     *
     *         V  = sum_ijk(V_ijk * W_ijk)
     * => grad(V) = sum_ijk(V_ijk * grad(W_ijk)^T)
     *
     * The matrix constructor takes row vectors by default, use column vectors instead.
     */
    result = matrix * GradientT(derivative_fn(vvx, uvw.x()),
                                kernel_fn(gvx, uvw.x()),
                                kernel_fn(vgx, uvw.x()),
                                /*rows=*/false);
  }
  else {
    result = matrix * GradientT(derivative_fn(vvx, uvw.x()),
                                kernel_fn(gvx, uvw.x()),
                                kernel_fn(vgx, uvw.x()));
  }
}

/** Utility struct for reading a fixed number of samples based on a kernel type. */
template<typename Kernel, class AccessorT> struct SampleBuffer {
  using ValueType = typename AccessorT::ValueType;

  /* Size of the buffer in one dimension. */
  static const int N = Kernel::samples_left + Kernel::samples_right;

  /* Coordinates of the samples. */
  openvdb::CoordBBox index_box;
  /* Fractional offset of the sampling location from the center coordinate. */
  openvdb::Vec3R uvw;
  /* Sample data. */
  ValueType data[N][N][N];

  SampleBuffer(const openvdb::Vec3R &coord)
  {
    const openvdb::Vec3R sample_offset = openvdb::Vec3R(Kernel::sample_offset);
    const openvdb::Coord index = openvdb::Coord(
        openvdb::tools::local_util::floorVec3(coord + sample_offset));
    index_box = openvdb::CoordBBox(index - openvdb::Coord(Kernel::samples_left - 1),
                                   index + openvdb::Coord(Kernel::samples_right));
    uvw = coord - index;
  }
};

template<typename Kernel, class AccessorT>
inline bool sample_tree(const AccessorT &accessor,
                        const openvdb::Vec3R &coord,
                        typename AccessorT::ValueType &result)
{
  using ValueT = typename AccessorT::ValueType;

  SampleBuffer<Kernel, AccessorT> buffer(coord);
  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  bool active = probe_values(accessor, buffer.index_box, buffer.data);
  interpolate_value_3d(buffer.data, buffer.uvw, Kernel::template sample_value<ValueT>, result);

  return active;
}

template<typename Kernel, class AccessorT>
inline typename AccessorT::ValueType sample_tree(const AccessorT &accessor,
                                                 const openvdb::Vec3R &coord)
{
  using ValueT = typename AccessorT::ValueType;

  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  SampleBuffer<Kernel, AccessorT> buffer(coord);
  get_values(accessor, buffer.index_box, buffer.data);

  ValueT result;
  interpolate_value_3d(buffer.data, buffer.uvw, Kernel::template sample_value<ValueT>, result);
  return result;
}

template<typename Kernel, class AccessorT>
bool sample_tree_gradient(const AccessorT &accessor,
                          const openvdb::math::Transform &transform,
                          const openvdb::Vec3R &coord,
                          OpenvdbGradientType<typename AccessorT::ValueType> &result)
{
  using ValueT = typename AccessorT::ValueType;

  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  SampleBuffer<Kernel, AccessorT> buffer(coord);
  bool active = probe_values(accessor, buffer.index_box, buffer.data);
  interpolate_gradient_3d(buffer.data,
                          buffer.uvw,
                          transform,
                          Kernel::template sample_value<ValueT>,
                          Kernel::template sample_gradient<ValueT>,
                          result);

  return active;
}

template<typename Kernel, class AccessorT>
OpenvdbGradientType<typename AccessorT::ValueType> sample_tree_gradient(
    const AccessorT &accessor,
    const openvdb::math::Transform &transform,
    const openvdb::Vec3R &coord)
{
  using ValueT = typename AccessorT::ValueType;

  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  SampleBuffer<Kernel, AccessorT> buffer(coord);
  get_values(accessor, buffer.index_box, buffer.data);

  ValueT result;
  interpolate_gradient_3d(buffer.data,
                          buffer.uvw,
                          transform,
                          Kernel::template sample_value<ValueT>,
                          Kernel::template sample_gradient<ValueT>,
                          result);
  return result;
}

template<typename Kernel, int Moment, typename ValueT, typename ResultT, int N>
void compute_moments(ValueT const (&data)[N][N][N],
                     ResultT (&moments)[N][N][N],
                     const openvdb::Vec3R &uvw,
                     const openvdb::math::Transform &transform)
{
  const openvdb::Vec3R kernel_offset = openvdb::Vec3R((N - 1) >> 1);
  const openvdb::math::AffineMap::ConstPtr affine_map = transform.baseMap()->getAffineMap();

  /* Compute moment contributions by multiplying with distance. */
  for (const int i : IndexRange(N)) {
    for (const int j : IndexRange(N)) {
      for (const int k : IndexRange(N)) {
        openvdb::Vec3s delta = affine_map->applyJacobian(openvdb::Vec3s(i, j, k) - kernel_offset -
                                                         uvw);
        if constexpr (std::is_same_v<ValueT, float>) {
          if constexpr (Moment == 1) {
            /* Scalar product of the position vector. */
            moments[i][j][k] = delta * data[i][j][k];
          }
          if constexpr (Moment == 2) {
            /* Outer product of the position vector. */
            openvdb::Vec3s vec = delta * data[i][j][k];
            moments[i][j][k] = openvdb::Mat3s(vec * delta.x(),
                                              vec * delta.y(),
                                              vec * delta.z(),
                                              /*rows=*/false);
          }
        }
        if constexpr (std::is_same_v<ValueT, openvdb::Vec3s>) {
          if constexpr (Moment == 1) {
            /* Outer product of the position and data vectors. */
            openvdb::Vec3s vec = data[i][j][k];
            moments[i][j][k] = openvdb::Mat3s(vec * delta.x(),
                                              vec * delta.y(),
                                              vec * delta.z(),
                                              /*rows=*/false);
          }
        }
      }
    }
  }
}

template<typename Kernel, int Moment, typename ResultT, class AccessorT>
bool sample_tree_moment(const AccessorT &accessor,
                        const openvdb::math::Transform &transform,
                        const openvdb::Vec3R &coord,
                        ResultT &result)
{
  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  SampleBuffer<Kernel, AccessorT> buffer(coord);
  bool active = probe_values(accessor, buffer.index_box, buffer.data);
  ResultT moments[buffer.N][buffer.N][buffer.N];
  compute_moments<Kernel, Moment>(buffer.data, moments, buffer.uvw, transform);
  interpolate_value_3d(moments, buffer.uvw, Kernel::template sample_value<ResultT>, result);

  return active;
}

template<typename Kernel, int Moment, typename ResultT, class AccessorT>
ResultT sample_tree_moment(const AccessorT &accessor,
                           const openvdb::math::Transform &transform,
                           const openvdb::Vec3R &coord)
{
  /* Retrieve the values of the voxels surrounding the fractional source coordinates. */
  SampleBuffer<Kernel, AccessorT> buffer(coord);
  get_values(accessor, buffer.index_box, buffer.data);
  ResultT moments[buffer.N][buffer.N][buffer.N];
  compute_moments<Kernel, Moment>(buffer.data, moments, buffer.uvw, transform);

  ResultT result;
  interpolate_value_3d(moments, buffer.uvw, Kernel::template sample_value<ResultT>, result);
  return result;
}

/**
 * Nearest-point kernel function.
 *
 * This kernel has a range of 0.5 voxels. For sampling in the index space of i <= x <= i+1
 * the contribution of points {i, i+1} must be considered. However, the influence of one
 * grid node can be avoided by shifting the sampling interval by half a voxel from [i, i+1]
 * to [i-0.5, i+0.5]. On this interval only point i affects the sampling.
 */
struct NearestPointKernel {
  static constexpr int samples_left = 1;
  static constexpr int samples_right = 0;
  /* Shift the sampling interval (round the coordinate), to use the value the closest point. */
  static constexpr float sample_offset = 0.5f;

  static float weight(float x)
  {
    if (x < -0.5f) {
      return 0.0f;
    }
    if (x < 0.5f) {
      return 1.0f;
    }
    return 0.0f;
  }

  static float derivative(float /*x*/)
  {
    return 0.0f;
  }

  template<class ValueT> static ValueT sample_value(const ValueT *values, float /*weight*/)
  {
    return values[0];
  }

  template<class ValueT> static ValueT sample_gradient(const ValueT * /*values*/, float /*weight*/)
  {
    return ValueT(0.0);
  }
};

/**
 * Linear kernel function.
 */
struct LinearKernel {
  static constexpr int samples_left = 1;
  static constexpr int samples_right = 1;
  static constexpr float sample_offset = 0.0f;

  static float weight(float x)
  {
    if (x < -1.0f) {
      return 0.0f;
    }
    if (x < 0.0f) {
      return x + 1.0f;
    }
    if (x < 1.0f) {
      return -x + 1.0f;
    }
    return 0.0f;
  }

  static float derivative(float x)
  {
    if (x < -1.0f) {
      return 0.0f;
    }
    if (x < 0.0f) {
      return 1.0f;
    }
    if (x < 1.0f) {
      return -1.0f;
    }
    return 0.0f;
  }

  template<class ValueT> static ValueT sample_value(const ValueT *values, float weight)
  {
    const ValueT lin = ValueT(values[1] - values[0]);
    const ValueT con = ValueT(values[0]);
    return ValueT(weight * lin) + con;
  }

  template<class ValueT> static ValueT sample_gradient(const ValueT *values, float /*weight*/)
  {
    const ValueT con = ValueT(values[1] - values[0]);
    return con;
  }
};

/**
 * Quadratic B-spline kernel function as described in
 * Steffen et al., "Analysis and reduction of quadrature errors in the material point method (MPM)"
 *
 * The kernel is a piece-wise quadratic spline:
 * f(x) = 0                          for         x < -3/2
 * f(x) = 1/2*x^2 + 3/2*x + 9/8      for -3/2 <= x < -1/2
 * f(x) = -x^2 + 3/4                 for -1/2 <= x < 1/2
 * f(x) = 1/2*x^2 - 3/2*x + 9/8      for  1/2 <= x < 3/2
 * f(x) = 0                          for  3/2 <= x
 *
 * This kernel has a range of 1.5 voxels. For sampling in the index space of i <= x <= i+1
 * the contribution of points {i-1, i, i+1, i+2} must be considered. However, the influence of one
 * grid node can be avoided by shifting the sampling interval by half a voxel from [i, i+1]
 * to [i-0.5, i+0.5]. On this interval only points {i-1, i, i+1} affect the sampling.
 * The shifted interval is also aligned with the piecewise function changes, requiring no
 * branching.
 *
 * Shifting the kernel function to these voxel locations yields these contributions:
 * v(x) = v[i-1]*f(x+1) +   v[i]*f(x) + v[i+1]*f(x-1)
 *      =      A*f(x+1) +      B*f(x) +      C*f(x-1)
 *
 * This results in the following expression for sampling in one dimension over x in [i-0.5, i+0.5]:
 *   v(x) =   x^2*( 1/2*A     - B + 1/2*C)
 *          +   x*(-1/2*A         + 1/2*C)
 *          +     ( 1/8*A + 6/8*B + 1/8*C)
 *
 * and for the derivative:
 *   dv(x) =    x*(     A   - 2*B     + C)
 *           +    (-1/2*A         + 1/2*C)
 */
struct QuadraticBSplineKernel {
  static constexpr int samples_left = 2;
  static constexpr int samples_right = 1;
  /* Shift the sampling interval to simplify interpolation. */
  static constexpr float sample_offset = 0.5f;

  static float weight(float x)
  {
    if (x < -1.5f) {
      return 0.0f;
    }
    if (x < -0.5f) {
      return (0.5f * x + 1.5f) * x + 1.125f;
    }
    if (x < 0.5f) {
      return -x * x + 0.75f;
    }
    if (x < 1.5f) {
      return (0.5f * x - 1.5f) * x + 1.125f;
    }
    return 0.0f;
  }

  static float derivative(float x)
  {
    if (x < -1.5f) {
      return 0.0f;
    }
    if (x < -0.5f) {
      return x + 1.5f;
    }
    if (x < 0.5f) {
      return -2.0f * x;
    }
    if (x < 1.5f) {
      return x - 1.5f;
    }
    return 0.0f;
  }

  template<class ValueT> static ValueT sample_value(const ValueT *values, float weight)
  {
    const ValueT sqr = ValueT(0.5 * (values[0] + values[2]) - values[1]);
    const ValueT lin = ValueT(0.5 * (values[2] - values[0]));
    const ValueT con = ValueT(0.125 * (values[0] + values[2]) + 0.75 * values[1]);
    return weight * (weight * sqr + lin) + con;
  }

  template<class ValueT> static ValueT sample_gradient(const ValueT *values, float weight)
  {
    const ValueT lin = ValueT(values[0] - 2.0 * values[1] + values[2]);
    const ValueT con = ValueT(0.5 * (values[2] - values[0]));
    return weight * lin + con;
  }
};

/**
 * Cubic B-spline kernel function as described in
 * Steffen et al., "Analysis and reduction of quadrature errors in the material point method (MPM)"
 *
 * The kernel is a piece-wise cubic spline:
 * f(x) = 0                                 for       x  < -2
 * f(x) = 1/6*x^3 + x^2 + 2*x + 4/3         for  -2 <= x < -1
 * f(x) = -1/2*x^3 - x^2 + 2/3              for  -1 <= x < 0
 * f(x) = 1/2*x^3 - x^2 + 2/3               for   0 <= x < 1
 * f(x) = -1/6*x^3 + x^2 - 2*x + 4/3        for   1 <= x < 2
 * f(x) = 0                                 for   2 <= x
 *
 * The derivative is a piece-wise quadratic spline:
 * df(x) = 0                                for        x < -2
 * df(x) = 1/2*x^2 + 2*x + 2                for  -2 <= x < -1
 * df(x) = -3/2*x^2 - 2*x                   for  -1 <= x < 1
 * df(x) = 3/2*x^2 - 2*x                    for   0 <= x < 1
 * df(x) = -1/2*x^2 + 2*x - 2               for   1 <= x < 2
 * df(x) = 0                                for   2 <= x
 *
 * This kernel has a range of 2 voxels. For sampling in the index space of i <= x <= i+1
 * the contribution of points {i-1, i, i+1, i+2} must be considered.
 * Shifting the kernel function to these voxel locations yields these contributions:
 * v(x) = v[i-1]*f(x+1) +   v[i]*f(x) + v[i+1]*f(x-1) + v[i+2]*f(x-2)
 *      =      A*f(x+1) +      B*f(x) +      C*f(x-1) +      D*f(x-2)
 *
 * This results in the following expression for sampling in one dimension:
 * v(x) =   x^3*(-1/6*A + 1/2*B - 1/2*C + 1/6*D)
 *        + x^2*( 1/2*A     - B + 1/2*C)
 *        + x  *(-1/2*A         + 1/2*C)
 *        +     ( 1/6*A + 2/3*B + 1/6*C)
 *
 * and for the derivative:
 * dv(x) =  x^2*(-1/2*A + 3/2*B - 3/2*C + 1/2*D)
 *        + x  *(     A   - 2*B     + C)
 *        +     (-1/2*A         + 1/2*C)
 */
struct CubicBSplineKernel {
  static constexpr int samples_left = 2;
  static constexpr int samples_right = 2;
  static constexpr float sample_offset = 0.0f;

  static float weight(float x)
  {
    if (x < -2.0f) {
      return 0.0f;
    }
    if (x < -1.0f) {
      return ((x / 6.0f + 1.0f) * x + 2.0f) * x + 4.0f / 3.0f;
    }
    if (x < 0.0f) {
      return (-x * 0.5f - 1.0f) * x * x + 2.0f / 3.0f;
    }
    if (x < 1.0f) {
      return (x * 0.5f - 1.0f) * x * x + 2.0f / 3.0f;
    }
    if (x < 2.0f) {
      return ((-x / 6.0f + 1.0f) * x - 2.0f) * x + 4.0f / 3.0f;
    }
    return 0.0f;
  }

  static float derivative(float x)
  {
    if (x < -2.0f) {
      return 0.0f;
    }
    if (x < -1.0f) {
      return (0.5 * x + 2.0f) * x + 2.0f;
    }
    if (x < 0.0f) {
      return (-1.5f * x - 2.0f) * x;
    }
    if (x < 1.0f) {
      return (1.5f * x - 2.0f) * x;
    }
    if (x < 2.0f) {
      return (-0.5 * x + 2.0f) * x - 2.0f;
    }
    return 0.0f;
  }

  template<class ValueT> static ValueT sample_value(const ValueT *values, float weight)
  {
    constexpr double inv6 = 1.0 / 6.0;
    const ValueT cub = ValueT(inv6 * (values[3] - values[0]) + 0.5 * (values[1] - values[2]));
    const ValueT sqr = ValueT(0.5 * (values[0] + values[2]) - values[1]);
    const ValueT lin = ValueT(0.5 * (values[2] - values[0]));
    const ValueT con = ValueT(inv6 * (values[0] + 4.0 * values[1] + values[2]));
    return weight * (weight * (weight * cub + sqr) + lin) + con;
  }

  template<class ValueT> static ValueT sample_gradient(const ValueT *values, float weight)
  {
    const ValueT sqr = ValueT(0.5 * (values[3] - values[0]) + 1.5 * (values[1] - values[2]));
    const ValueT lin = ValueT(values[0] - 2.0 * values[1] + values[2]);
    const ValueT con = ValueT(0.5 * (values[2] - values[0]));
    return weight * (weight * sqr + lin) + con;
  }
};

/**
 * Grid value sampler using a kernel type.
 */
template<typename KernelT> struct SamplerWithKernel {
  template<class AccessorT>
  static bool sample(const AccessorT &accessor,
                     const openvdb::Vec3R &coord,
                     typename AccessorT::ValueType &result)
  {
    return grid_sampling::sample_tree<KernelT>(accessor, coord, result);
  }

  template<class AccessorT>
  static typename AccessorT::ValueType sample(const AccessorT &accessor,
                                              const openvdb::Vec3R &coord)
  {
    return grid_sampling::sample_tree<KernelT>(accessor, coord);
  }

  template<class AccessorT>
  static bool sample_gradient(
      const AccessorT &accessor,
      const openvdb::math::Transform &transform,
      const openvdb::Vec3R &coord,
      grid_sampling::OpenvdbGradientType<typename AccessorT::ValueType> &result)
  {
    return grid_sampling::sample_tree_gradient<KernelT>(accessor, transform, coord, result);
  }

  template<class AccessorT>
  static grid_sampling::OpenvdbGradientType<typename AccessorT::ValueType> sample_gradient(
      const AccessorT &accessor,
      const openvdb::math::Transform &transform,
      const openvdb::Vec3R &coord)
  {
    return grid_sampling::sample_tree_gradient<KernelT>(accessor, transform, coord);
  }

  template<int Moment, typename ResultT, class AccessorT>
  static bool sample_moment(const AccessorT &accessor,
                            const openvdb::math::Transform &transform,
                            const openvdb::Vec3R &coord,
                            ResultT &result)
  {
    return grid_sampling::sample_tree_moment<KernelT, Moment, ResultT>(
        accessor, transform, coord, result);
  }

  template<int Moment, typename ResultT, class AccessorT>
  static ResultT sample_moment(const AccessorT &accessor,
                               const openvdb::math::Transform &transform,
                               const openvdb::Vec3R &coord)
  {
    return grid_sampling::sample_tree_moment<KernelT, Moment, ResultT>(accessor, transform, coord);
  }
};

}  // namespace grid_sampling

/**
 * Grid value sampler using a nearest-point kernel.
 */
using NearestPointSampler = grid_sampling::SamplerWithKernel<grid_sampling::NearestPointKernel>;

/**
 * Grid value sampler using a linear kernel.
 */
using LinearSampler = grid_sampling::SamplerWithKernel<grid_sampling::LinearKernel>;

/**
 * Grid value sampler using a quadratic B-spline kernel.
 */
using QuadraticBSplineSampler =
    grid_sampling::SamplerWithKernel<grid_sampling::QuadraticBSplineKernel>;

/**
 * Grid value sampler using a cubic B-spline kernel.
 */
using CubicBSplineSampler = grid_sampling::SamplerWithKernel<grid_sampling::CubicBSplineKernel>;

#endif

}  // namespace blender::geometry
