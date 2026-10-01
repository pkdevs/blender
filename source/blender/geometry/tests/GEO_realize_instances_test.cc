/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <fmt/format.h>

#include "BLI_array_utils.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_rotation.hh"
#include "BLI_rand.hh"

#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_gtest_base.hh"
#include "BKE_instances.hh"
#include "BKE_lib_id.hh"
#include "BKE_pointcloud.hh"

#include "DNA_curves_types.h"
#include "DNA_pointcloud_types.h"

#include "GEO_join_geometries.hh"
#include "GEO_realize_instances.hh"
#include "GEO_transform.hh"

#include "testing/testing.h"

namespace blender {

using namespace blender::bke;

namespace geometry::tests {

class RealizeInstancesTest : public bke::BlenderGTestBase {};

static void create_test_curves(bke::CurvesGeometry &curves, Span<int> offsets)
{
  BLI_assert(!offsets.is_empty());
  const int curves_num = offsets.size() - 1;
  const int points_num = offsets.last();

  curves.resize(points_num, curves_num);
  curves.offsets_for_write().copy_from(offsets);
  curves.update_curve_types();

  /* Attribute storing original indices to test point remapping. */
  SpanAttributeWriter<int> test_indices_writer =
      curves.attributes_for_write().lookup_or_add_for_write_span<int>(
          "test_index", bke::AttrDomain::Point, bke::AttributeInitConstruct());
  array_utils::fill_index_range(test_indices_writer.span);
  test_indices_writer.finish();
}

/* Regression test for builtin curve attributes:
 * The attribute can be added with arbitrary type/domain on instances,
 * but is built-in and restricted on curves, which will not allow writing it
 * to the realized curves geometry. #142163 */
TEST_F(RealizeInstancesTest, InstanceAttributeToBuiltinCurvesAttribute)
{
  Curves *curves_id = BKE_id_new_nomain<Curves>("TestCurves");
  create_test_curves(curves_id->geometry.wrap(), {0, 3});
  bke::GeometrySet curves_geometry = GeometrySet::from_curves(curves_id);

  auto instances = std::make_unique<Instances>(2);
  const int handle = instances->add_reference(bke::InstanceReference{curves_geometry});
  /* The issue only occurs with 2 or more instances. In case of a single instance the code takes a
   * special path that does not run cause this problem. */
  instances->reference_handles_for_write().fill(handle);
  instances->transforms_for_write().fill(float4x4::identity());
  /* This attribute will be converted to the point domain, where it is invalid on curves. */
  instances->attributes_for_write().add<float>(
      "curve_type", AttrDomain::Instance, AttributeInitDefaultValue());
  bke::GeometrySet instances_geometry = GeometrySet::from_instances(std::move(instances));

  geometry::RealizeInstancesOptions options;
  options.realize_instance_attributes = true;
  GeometrySet realized_geometry_set =
      geometry::realize_instances(instances_geometry, options).geometry;
}

static std::string sh_name(const int i)
{
  return fmt::format("radiance:sh_{}", i);
}

static float3 random_float3(RandomNumberGenerator &rng)
{
  const float x = rng.get_float();
  const float y = rng.get_float();
  const float z = rng.get_float();
  return float3(x, y, z) * 2.0f - 1.0f;
}

static PointCloud *create_splats(const PointCloudType type,
                                 const int points_num,
                                 const bool rotation_and_scale,
                                 const int sh_num,
                                 const uint32_t seed)
{
  PointCloud *pointcloud = BKE_pointcloud_new_nomain(type, points_num);
  MutableAttributeAccessor attributes = pointcloud->attributes_for_write();
  RandomNumberGenerator rng(seed);
  for (float3 &position : pointcloud->positions_for_write()) {
    position = random_float3(rng);
  }
  if (rotation_and_scale) {
    SpanAttributeWriter rotations = attributes.lookup_or_add_for_write_only_span<math::Quaternion>(
        "rotation", AttrDomain::Point);
    for (math::Quaternion &rotation : rotations.span) {
      const float4 value(random_float3(rng), rng.get_float() - 0.5f);
      rotation = math::normalize(math::Quaternion(value));
    }
    rotations.finish();
    SpanAttributeWriter scales = attributes.lookup_or_add_for_write_only_span<float3>(
        "scale", AttrDomain::Point);
    for (float3 &scale : scales.span) {
      scale = math::abs(random_float3(rng)) + float3(0.01f);
    }
    scales.finish();
  }
  for (const int i : IndexRange(sh_num)) {
    SpanAttributeWriter sh = attributes.lookup_or_add_for_write_only_span<float3>(
        sh_name(i), AttrDomain::Point);
    for (float3 &value : sh.span) {
      value = random_float3(rng);
    }
    sh.finish();
  }
  return pointcloud;
}

static GeometrySet create_instances(const Span<GeometrySet> references,
                                    const Span<int> handles,
                                    const Span<float4x4> transforms)
{
  auto instances = std::make_unique<Instances>(handles.size());
  for (const GeometrySet &reference : references) {
    instances->add_new_reference(InstanceReference{reference});
  }
  instances->reference_handles_for_write().copy_from(handles);
  instances->transforms_for_write().copy_from(transforms);
  return GeometrySet::from_instances(std::move(instances));
}

static const PointCloud &realize(const GeometrySet &geometry, GeometrySet &r_result)
{
  r_result = geometry::realize_instances(geometry, geometry::RealizeInstancesOptions()).geometry;
  return *r_result.get_pointcloud();
}

static float4x4 rotation_and_non_uniform_scale()
{
  return math::from_loc_rot_scale<float4x4>(
      float3(1, 2, 3),
      math::to_quaternion(math::AxisAngle(math::normalize(float3(1, 2, 3)), 0.7f)),
      float3(1.0f, 3.0f, 0.2f));
}

static float4x4 mirror_and_uniform_scale()
{
  return math::from_loc_rot_scale<float4x4>(
      float3(-1, 0, 2),
      math::to_quaternion(math::AxisAngle(math::normalize(float3(0, 1, 1)), 2.0f)),
      float3(-2.0f, 2.0f, 2.0f));
}

template<typename T>
static Array<T> read(const PointCloud &pointcloud, const StringRef name, const IndexRange slice)
{
  const AttributeReader<T> attribute = pointcloud.attributes().lookup<T>(name, AttrDomain::Point);
  if (!attribute) {
    ADD_FAILURE() << "Missing attribute " << name;
    return {};
  }
  return Array<T>(VArraySpan<T>(attribute.varray).slice(slice));
}

static void expect_near(const Span<float3> a, const Span<float3> b, const float epsilon)
{
  ASSERT_EQ(a.size(), b.size());
  for (const int i : a.index_range()) {
    const float3 ai = a[i];
    const float3 bi = b[i];
    EXPECT_V3_NEAR(ai, bi, epsilon);
  }
}

static void expect_near(const Span<math::Quaternion> a,
                        const Span<math::Quaternion> b,
                        const float epsilon)
{
  ASSERT_EQ(a.size(), b.size());
  for (const int i : a.index_range()) {
    const float4 ai(a[i]);
    const float4 bi(b[i]);
    EXPECT_V4_NEAR(ai, bi, epsilon);
  }
}

template<typename T> static void expect_default(const Array<T> &values)
{
  const T &default_value = *static_cast<const T *>(CPPType::get<T>().default_value());
  for (const T &value : values) {
    EXPECT_EQ(value, default_value);
  }
}

/**
 * Realizing an instance of gaussian splats must give the same splats as transforming the
 * instanced geometry with the instance transform.
 */
static void expect_splats_transformed(const PointCloud &result,
                                      const int start,
                                      const PointCloud &src,
                                      const float4x4 &transform)
{
  GeometrySet expected_geometry = GeometrySet::from_pointcloud(BKE_pointcloud_copy_for_eval(&src));
  transform_geometry(expected_geometry, transform);
  const PointCloud &expected = *expected_geometry.get_pointcloud();
  const IndexRange slice(start, src.totpoint);
  const IndexRange all(src.totpoint);

  expect_near(result.positions().slice(slice), expected.positions(), 1e-5f);
  const AttributeAccessor src_attributes = src.attributes();
  if (src_attributes.contains("rotation")) {
    expect_near(read<math::Quaternion>(result, "rotation", slice),
                read<math::Quaternion>(expected, "rotation", all),
                1e-6f);
    expect_near(read<float3>(result, "scale", slice), read<float3>(expected, "scale", all), 1e-6f);
  }
  for (int i = 0; src_attributes.contains(sh_name(i)); i++) {
    expect_near(
        read<float3>(result, sh_name(i), slice), read<float3>(expected, sh_name(i), all), 1e-6f);
  }
}

TEST_F(RealizeInstancesTest, GSplatsSingleInstance)
{
  const GeometrySet splats = GeometrySet::from_pointcloud(
      create_splats(PointCloudType::GSplat, 16, true, 15, 1));
  const float4x4 transform = rotation_and_non_uniform_scale();
  GeometrySet result_geometry;
  const PointCloud &result = realize(create_instances({splats}, {0}, {transform}),
                                     result_geometry);

  EXPECT_EQ(result.type, PointCloudType::GSplat);
  expect_splats_transformed(result, 0, *splats.get_pointcloud(), transform);
}

/* Instances of point clouds with different sets of attributes, so that some values in the result
 * come from defaults and must not be transformed. */
TEST_F(RealizeInstancesTest, GSplatsMultipleInstances)
{
  /* Band 1, band 2 without rotation and scale, and band 1 followed by an incomplete band. */
  const GeometrySet splats_a = GeometrySet::from_pointcloud(
      create_splats(PointCloudType::GSplat, 5, true, 3, 1));
  const GeometrySet splats_b = GeometrySet::from_pointcloud(
      create_splats(PointCloudType::GSplat, 7, false, 8, 2));
  const GeometrySet splats_c = GeometrySet::from_pointcloud(
      create_splats(PointCloudType::GSplat, 3, true, 5, 3));
  const PointCloud &a = *splats_a.get_pointcloud();
  const PointCloud &b = *splats_b.get_pointcloud();
  const PointCloud &c = *splats_c.get_pointcloud();
  const Array<float4x4> transforms = {rotation_and_non_uniform_scale(),
                                      mirror_and_uniform_scale(),
                                      float4x4::identity(),
                                      math::from_rotation<float4x4>(math::AxisAngle(
                                          math::AxisSigned::Y_POS, math::AngleRadian(1.2f)))};
  GeometrySet result_geometry;
  const PointCloud &result = realize(
      create_instances({splats_a, splats_b, splats_c}, {0, 1, 0, 2}, transforms), result_geometry);

  ASSERT_EQ(result.totpoint, 5 + 7 + 5 + 3);
  expect_splats_transformed(result, 0, a, transforms[0]);
  expect_splats_transformed(result, 5, b, transforms[1]);
  expect_splats_transformed(result, 12, a, transforms[2]);
  expect_splats_transformed(result, 17, c, transforms[3]);

  const IndexRange slice_b(5, 7);
  expect_default(read<math::Quaternion>(result, "rotation", slice_b));
  expect_default(read<float3>(result, "scale", slice_b));
  for (const int i : IndexRange(3, 5)) {
    expect_default(read<float3>(result, sh_name(i), IndexRange(0, 5)));
    expect_default(read<float3>(result, sh_name(i), IndexRange(12, 5)));
  }
  for (const int i : IndexRange(5, 3)) {
    expect_default(read<float3>(result, sh_name(i), IndexRange(17, 3)));
  }
}

TEST_F(RealizeInstancesTest, GSplatsWithSingleValueAttributes)
{
  PointCloud *pointcloud = BKE_pointcloud_new_nomain(PointCloudType::GSplat, 4);
  MutableAttributeAccessor attributes = pointcloud->attributes_for_write();
  attributes.add<math::Quaternion>(
      "rotation", AttrDomain::Point, AttributeInitValue(math::Quaternion::identity()));
  attributes.add<float3>("scale", AttrDomain::Point, AttributeInitValue(float3(1, 2, 3)));
  for (const int i : IndexRange(3)) {
    attributes.add<float3>(sh_name(i), AttrDomain::Point, AttributeInitValue(float3(i + 1)));
  }
  const GeometrySet splats = GeometrySet::from_pointcloud(pointcloud);
  const Array<float4x4> transforms = {rotation_and_non_uniform_scale(),
                                      mirror_and_uniform_scale()};
  GeometrySet result_geometry;
  const PointCloud &result = realize(create_instances({splats}, {0, 0}, transforms),
                                     result_geometry);

  expect_splats_transformed(result, 0, *pointcloud, transforms[0]);
  expect_splats_transformed(result, 4, *pointcloud, transforms[1]);
}

TEST_F(RealizeInstancesTest, GSplatsNestedInstances)
{
  const GeometrySet splats = GeometrySet::from_pointcloud(
      create_splats(PointCloudType::GSplat, 6, true, 8, 4));
  const float4x4 inner_transform = rotation_and_non_uniform_scale();
  const GeometrySet inner = create_instances({splats}, {0}, {inner_transform});
  const Array<float4x4> outer_transforms = {
      mirror_and_uniform_scale(),
      math::from_rotation<float4x4>(
          math::AxisAngle(math::AxisSigned::X_POS, math::AngleRadian(1.0f)))};
  GeometrySet result_geometry;
  const PointCloud &result = realize(create_instances({inner}, {0, 0}, outer_transforms),
                                     result_geometry);

  expect_splats_transformed(
      result, 0, *splats.get_pointcloud(), outer_transforms[0] * inner_transform);
  expect_splats_transformed(
      result, 6, *splats.get_pointcloud(), outer_transforms[1] * inner_transform);
}

TEST_F(RealizeInstancesTest, GSplatsTranslationSharesData)
{
  const GeometrySet splats = GeometrySet::from_pointcloud(
      create_splats(PointCloudType::GSplat, 4, true, 3, 5));
  const PointCloud &src = *splats.get_pointcloud();
  GeometrySet result_geometry;
  const PointCloud &result = realize(
      create_instances({splats}, {0}, {math::from_location<float4x4>(float3(1, 2, 3))}),
      result_geometry);

  for (const StringRef name : {"rotation", "scale", "radiance:sh_0"}) {
    EXPECT_EQ(result.attributes().lookup(name).varray.get_internal_span().data(),
              src.attributes().lookup(name).varray.get_internal_span().data());
  }
}

/* Values that come from instance attributes are not transformed, like with Transform Geometry
 * followed by Realize Instances. */
TEST_F(RealizeInstancesTest, GSplatsInstanceAttributesAreNotTransformed)
{
  const GeometrySet splats = GeometrySet::from_pointcloud(
      create_splats(PointCloudType::GSplat, 4, false, 0, 6));
  GeometrySet instances = create_instances(
      {splats}, {0, 0}, {rotation_and_non_uniform_scale(), mirror_and_uniform_scale()});
  MutableAttributeAccessor instance_attributes =
      instances.get_instances_for_write()->attributes_for_write();
  instance_attributes.add<math::Quaternion>(
      "rotation", AttrDomain::Instance, AttributeInitValue(math::Quaternion::identity()));
  instance_attributes.add<float3>("scale", AttrDomain::Instance, AttributeInitValue(float3(2)));
  GeometrySet result_geometry;
  const PointCloud &result = realize(instances, result_geometry);

  for (const math::Quaternion &rotation :
       read<math::Quaternion>(result, "rotation", IndexRange(8)))
  {
    EXPECT_EQ(rotation, math::Quaternion::identity());
  }
  for (const float3 &scale : read<float3>(result, "scale", IndexRange(8))) {
    EXPECT_EQ(scale, float3(2));
  }
}

TEST_F(RealizeInstancesTest, PointsAttributesAreNotTransformed)
{
  const GeometrySet points = GeometrySet::from_pointcloud(
      create_splats(PointCloudType::Points, 4, true, 3, 7));
  const PointCloud &src = *points.get_pointcloud();
  GeometrySet result_geometry;
  const PointCloud &result = realize(
      create_instances({points}, {0, 0}, {rotation_and_non_uniform_scale(), float4x4::identity()}),
      result_geometry);

  EXPECT_EQ(result.type, PointCloudType::Points);
  for (const int start : {0, 4}) {
    const IndexRange slice(start, 4);
    const IndexRange all(4);
    expect_near(read<math::Quaternion>(result, "rotation", slice),
                read<math::Quaternion>(src, "rotation", all),
                0.0f);
    expect_near(read<float3>(result, "scale", slice), read<float3>(src, "scale", all), 0.0f);
    expect_near(read<float3>(result, "radiance:sh_0", slice),
                read<float3>(src, "radiance:sh_0", all),
                0.0f);
  }
}

TEST_F(RealizeInstancesTest, MixedPointsAndGSplatsGiveGSplats)
{
  const GeometrySet points = GeometrySet::from_pointcloud(
      create_splats(PointCloudType::Points, 2, false, 0, 8));
  const GeometrySet splats = GeometrySet::from_pointcloud(
      create_splats(PointCloudType::GSplat, 3, true, 3, 9));
  for (const Array<GeometrySet> &inputs :
       {Array<GeometrySet>{points, splats}, Array<GeometrySet>{splats, points}})
  {
    GeometrySet result_geometry;
    const PointCloud &result = realize(
        create_instances(inputs, {0, 1}, {float4x4::identity(), float4x4::identity()}),
        result_geometry);
    EXPECT_EQ(result.type, PointCloudType::GSplat);
    EXPECT_EQ(result.totpoint, 5);

    const GeometrySet joined = join_geometries(inputs, AttributeFilter::default_filter());
    EXPECT_EQ(joined.get_pointcloud()->type, PointCloudType::GSplat);
  }
}

}  // namespace geometry::tests
}  // namespace blender
