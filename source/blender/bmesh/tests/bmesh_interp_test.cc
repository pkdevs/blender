/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_linklist.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_memarena.hh"

#include "BKE_customdata.hh"
#include "BKE_gtest_base.hh"

#include "bmesh.hh"

namespace blender {

class BMeshInterpTest : public bke::BlenderGTestBase {
 protected:
  BMesh *bm;
  MemArena *group_arena = nullptr;

  void SetUp() override
  {
    BlenderGTestBase::SetUp();
    const BMeshCreateParams mesh_create_params{};
    bm = BM_mesh_create(&bm_mesh_allocsize_default, &mesh_create_params);
  }

  void TearDown() override
  {
    if (group_arena != nullptr) {
      BLI_memarena_free(group_arena);
    }
    BM_mesh_free(bm);
    BlenderGTestBase::TearDown();
  }

  BMFace *create_face(const Span<float3> vertex_positions)
  {
    Array<BMVert *> vertices(vertex_positions.size());
    for (const int i : vertex_positions.index_range()) {
      vertices[i] = BM_vert_create(bm, vertex_positions[i], nullptr, BM_CREATE_NOP);
    }
    return BM_face_create_verts(
        bm, vertices.data(), vertices.size(), nullptr, BM_CREATE_NOP, true);
  }

  static float2 &loop_uv(BMLoop *loop, const int uv_offset)
  {
    return *static_cast<float2 *>(BM_ELEM_CD_GET_VOID_P(loop, uv_offset));
  }
};

TEST_F(BMeshInterpTest, FacePreservesConstantUVComponents)
{
  /* When Correct UVs interpolates corner data during editing, rounded weights can change a UV
   * component that was constant across the source face. Such drift can give collapsed UV faces
   * an unstable winding. Give a quad two UV layers: one with constant V and the other with
   * constant U. Sample 49 interior positions and require both constant components to stay exact.
   * The varying components follow X and Y, so their expected values are known as well. */
  BM_data_layer_add_named(bm, &bm->ldata, CD_PROP_FLOAT2, "ConstantV");
  BM_data_layer_add_named(bm, &bm->ldata, CD_PROP_FLOAT2, "ConstantU");
  const int constant_v_offset = CustomData_get_n_offset(&bm->ldata, CD_PROP_FLOAT2, 0);
  const int constant_u_offset = CustomData_get_n_offset(&bm->ldata, CD_PROP_FLOAT2, 1);
  const std::array<float3, 4> source_positions = {
      float3(1, 0, 0), float3(0, 1, 0), float3(-1, 0, 0), float3(0, -1, 0)};
  BMFace *source_face = create_face(source_positions);
  const std::array<float3, 3> destination_positions = {
      float3(0, 0, 0), float3(0, 0, 1), float3(0, 1, 1)};
  BMLoop *destination_loop = BM_FACE_FIRST_LOOP(create_face(destination_positions));
  BMLoop *source_loop = BM_FACE_FIRST_LOOP(source_face);
  do {
    loop_uv(source_loop, constant_v_offset) = float2(source_loop->v->co[0], 0.5f);
    loop_uv(source_loop, constant_u_offset) = float2(-0.25f, source_loop->v->co[1]);
  } while ((source_loop = source_loop->next) != BM_FACE_FIRST_LOOP(source_face));
  BM_mesh_normals_update(bm);

  /* Sample several positions inside the quad to exercise different interpolation weights.
   * The constant components must stay exact; the varying components may have rounding error. */
  for (int x = -3; x <= 3; x++) {
    for (int y = -3; y <= 3; y++) {
      SCOPED_TRACE(::testing::Message() << "Sample (" << x << ", " << y << ")");
      destination_loop->v->co[0] = float(x) / 20.0f;
      destination_loop->v->co[1] = float(y) / 20.0f;
      BM_loop_interp_from_face(bm, destination_loop, source_face, false, false);
      EXPECT_EQ(loop_uv(destination_loop, constant_v_offset).y, 0.5f);
      EXPECT_EQ(loop_uv(destination_loop, constant_u_offset).x, -0.25f);
      EXPECT_NEAR(
          loop_uv(destination_loop, constant_v_offset).x, destination_loop->v->co[0], 1e-6f);
      EXPECT_NEAR(
          loop_uv(destination_loop, constant_u_offset).y, destination_loop->v->co[1], 1e-6f);
    }
  }
}

TEST_F(BMeshInterpTest, FaceInterpolatesUVsAndOtherCornerData)
{
  /* Face interpolation chooses a calculation for each corner-data layer. UVs with nonnegative
   * weights use the offset calculation, while other attributes retain their existing callbacks.
   * Create a triangle with a UV layer and a scalar layer, both linear functions of the vertex
   * coordinates. Interpolate at a known interior point and check both results. This catches a
   * layer being skipped or written through another layer's offset. */
  BM_data_layer_add_named(bm, &bm->ldata, CD_PROP_FLOAT2, "UVMap");
  BM_data_layer_add_named(bm, &bm->ldata, CD_PROP_FLOAT, "Value");
  const int uv_offset = CustomData_get_offset(&bm->ldata, CD_PROP_FLOAT2);
  const int value_offset = CustomData_get_offset(&bm->ldata, CD_PROP_FLOAT);
  const std::array<float3, 3> source_positions = {
      float3(0, 0, 0), float3(1, 0, 0), float3(0, 1, 0)};
  BMFace *source_face = create_face(source_positions);
  BMLoop *source_loop = BM_FACE_FIRST_LOOP(source_face);
  do {
    const float x = source_loop->v->co[0];
    const float y = source_loop->v->co[1];
    loop_uv(source_loop, uv_offset) = float2(x + 0.5f, y - 0.25f);
    BM_ELEM_CD_SET_FLOAT(source_loop, value_offset, 4.0f * x + 8.0f * y);
  } while ((source_loop = source_loop->next) != BM_FACE_FIRST_LOOP(source_face));
  const std::array<float3, 3> destination_positions = {
      float3(0.25f, 0.5f, 0), float3(0, 0, 1), float3(0, 1, 1)};
  BMLoop *destination_loop = BM_FACE_FIRST_LOOP(create_face(destination_positions));
  loop_uv(destination_loop, uv_offset) = float2(-10.0f);
  BM_ELEM_CD_SET_FLOAT(destination_loop, value_offset, -10.0f);
  BM_mesh_normals_update(bm);

  BM_loop_interp_from_face(bm, destination_loop, source_face, false, false);

  /* At (0.25, 0.5), UV = (x + 0.5, y - 0.25) and Value = 4x + 8y. */
  EXPECT_NEAR(loop_uv(destination_loop, uv_offset).x, 0.75f, 1e-6f);
  EXPECT_NEAR(loop_uv(destination_loop, uv_offset).y, 0.25f, 1e-6f);
  EXPECT_NEAR(BM_ELEM_CD_GET_FLOAT(destination_loop, value_offset), 5.0f, 1e-6f);
}

TEST_F(BMeshInterpTest, FaceInterpolationCanUseItsDestinationAsSource)
{
  /* The destination loop can belong to the source face, so its UV storage may be read and
   * written in the same call. Use each corner of a triangle as the destination in turn.
   * Sampling at a source corner must return that corner's original UV exactly. This catches
   * source values being overwritten before they are read. */
  BM_data_layer_add_named(bm, &bm->ldata, CD_PROP_FLOAT2, "UVMap");
  const int uv_offset = CustomData_get_offset(&bm->ldata, CD_PROP_FLOAT2);
  const std::array<float3, 3> source_positions = {
      float3(0, 0, 0), float3(1, 0, 0), float3(0, 1, 0)};
  BMFace *source_face = create_face(source_positions);
  BMLoop *source_loop = BM_FACE_FIRST_LOOP(source_face);
  do {
    loop_uv(source_loop, uv_offset) = float2(source_loop->v->co[0] + 0.5f,
                                             source_loop->v->co[1] - 0.25f);
  } while ((source_loop = source_loop->next) != BM_FACE_FIRST_LOOP(source_face));
  BM_mesh_normals_update(bm);
  int corner_index = 0;
  do {
    SCOPED_TRACE(::testing::Message() << "Source corner " << corner_index++);
    const float2 expected_uv = loop_uv(source_loop, uv_offset);
    BM_loop_interp_from_face(bm, source_loop, source_face, false, false);
    EXPECT_EQ(loop_uv(source_loop, uv_offset), expected_uv);
  } while ((source_loop = source_loop->next) != BM_FACE_FIRST_LOOP(source_face));
}

TEST_F(BMeshInterpTest, FaceExtrapolationKeepsExistingInterpolation)
{
  /* Signed weights retain the existing interpolation callback because changing the arithmetic
   * can worsen rounding errors when extrapolating from thin faces. Place the destination far
   * outside a thin quad and assert that a negative weight is present. Compare face interpolation
   * with the original weighted sum using the same projected weights. The results must match
   * exactly, including rounding, to verify that this case keeps its existing behavior. */
  BM_data_layer_add_named(bm, &bm->ldata, CD_PROP_FLOAT2, "UVMap");
  const int uv_offset = CustomData_get_offset(&bm->ldata, CD_PROP_FLOAT2);
  constexpr float face_height = 1.0f / (1 << 20);
  const std::array<float3, 4> source_positions = {
      float3(-1, 0, 0), float3(1, 0, 0), float3(1, face_height, 0), float3(-1, face_height, 0)};
  BMFace *source_face = create_face(source_positions);
  const std::array<float3, 3> destination_positions = {
      float3(2, 2, 0), float3(2, 2, 1), float3(2, 3, 1)};
  BMLoop *destination_loop = BM_FACE_FIRST_LOOP(create_face(destination_positions));
  BM_mesh_normals_update(bm);

  /* Project the source corners and destination into the face plane to calculate the weights. */
  float projection_matrix[3][3];
  axis_dominant_v3_to_m3(projection_matrix, source_face->no);
  float projected_corners[4][2];
  float interpolation_weights[4];
  const void *source_uvs[4];
  BMLoop *source_loop = BM_FACE_FIRST_LOOP(source_face);
  for (int corner_index = 0; corner_index < 4; corner_index++, source_loop = source_loop->next) {
    const float x = source_loop->v->co[0];
    const float y = source_loop->v->co[1];
    loop_uv(source_loop, uv_offset) = float2(0.25f + x * 0.125f + y * 0.0625f,
                                             -0.5f + x * 0.25f - y * 0.125f);
    source_uvs[corner_index] = &loop_uv(source_loop, uv_offset);
    mul_v2_m3v3(projected_corners[corner_index], projection_matrix, source_loop->v->co);
  }
  float projected_destination[2];
  mul_v2_m3v3(projected_destination, projection_matrix, destination_loop->v->co);
  interp_weights_poly_v2(interpolation_weights, projected_corners, 4, projected_destination);
  bool has_negative_weight = false;
  for (const float weight : interpolation_weights) {
    has_negative_weight |= weight < 0.0f;
  }
  ASSERT_TRUE(has_negative_weight) << "The destination must exercise the signed-weight fallback.";

  /* Compute the original weighted sum independently of the callback under test. */
  float2 expected_uv(0.0f);
  for (int i = 0; i < 4; i++) {
    expected_uv += *static_cast<const float2 *>(source_uvs[i]) * interpolation_weights[i];
  }

  BM_loop_interp_from_face(bm, destination_loop, source_face, false, false);
  EXPECT_EQ(loop_uv(destination_loop, uv_offset), expected_uv);
}

TEST_F(BMeshInterpTest, LoopGroupMergingPreservesConstantComponent)
{
  /* Connected face corners at a vertex form a group when their UVs match. Editing can change
   * these UVs independently, and merging combines them using normalized weights. Rounding in
   * those weights can perturb a component that stayed constant across the group.
   *
   * Build seven triangles around a shared vertex and group their initially identical UVs.
   * Then vary U while keeping V constant. Both merge paths must write the weighted average U
   * to every corner and preserve V exactly. Varying U also ensures that leaving the data
   * untouched cannot pass this test. */
  BM_data_layer_add_named(bm, &bm->ldata, CD_PROP_FLOAT2, "UVMap");
  const int uv_layer_index = CustomData_get_layer_index(&bm->ldata, CD_PROP_FLOAT2);
  const int uv_offset = CustomData_get_offset(&bm->ldata, CD_PROP_FLOAT2);
  const float3 center_position(0.0f);
  BMVert *center_vertex = BM_vert_create(bm, center_position, nullptr, BM_CREATE_NOP);
  constexpr int ring_vertex_count = 7;
  const std::array<float3, ring_vertex_count> ring_positions = {float3(1, 0, 0),
                                                                float3(1, 1, 0),
                                                                float3(0, 1, 0),
                                                                float3(-1, 1, 0),
                                                                float3(-1, 0, 0),
                                                                float3(-1, -1, 0),
                                                                float3(0, -1, 0)};
  std::array<BMVert *, ring_vertex_count> ring_vertices;
  for (int i = 0; i < ring_vertex_count; i++) {
    ring_vertices[i] = BM_vert_create(bm, ring_positions[i], nullptr, BM_CREATE_NOP);
  }
  for (int i = 0; i < ring_vertex_count; i++) {
    BMVert *triangle_vertices[3] = {
        center_vertex, ring_vertices[i], ring_vertices[(i + 1) % ring_vertex_count]};
    BMFace *triangle = BM_face_create_verts(
        bm, triangle_vertices, 3, nullptr, BM_CREATE_NOP, true);
    BMLoop *loop = BM_FACE_FIRST_LOOP(triangle);
    do {
      loop_uv(loop, uv_offset) = float2(0.5f, -0.25f);
    } while ((loop = loop->next) != BM_FACE_FIRST_LOOP(triangle));
  }
  BM_mesh_normals_update(bm);
  const std::array<float, ring_vertex_count> loop_weights = {
      0.3f, 0.7f, 0.1f, 0.9f, 0.2f, 0.4f, 0.8f};
  group_arena = BLI_memarena_new(BLI_MEMARENA_STD_BUFSIZE, __func__);
  LinkNode *loop_groups = BM_vert_loop_groups_data_layer_create(
      bm, center_vertex, uv_layer_index, loop_weights.data(), group_arena);
  ASSERT_NE(loop_groups, nullptr);
  ASSERT_EQ(BLI_linklist_count(loop_groups), 1);
  ASSERT_EQ(BM_vert_face_count(center_vertex), ring_vertex_count);

  /* Check the stored-weight and updated-weight merge paths. Reset the source UVs before each
   * merge so both paths start with the same values. */
  for (const bool recalculate_weights : {false, true}) {
    SCOPED_TRACE(recalculate_weights ? "Merge with updated weights" : "Merge with stored weights");
    BMIter iter;
    BMLoop *loop;
    BM_ITER_ELEM (loop, &iter, center_vertex, BM_LOOPS_OF_VERT) {
      /* Group creation assigns these indices to match the input weights. */
      loop_uv(loop, uv_offset) = float2(float(BM_elem_index_get(loop)), -0.25f);
    }

    /* Calculate the expected U directly as sum(U * weight) / sum(weight) in double precision.
     * This provides a reference independent of the production helper's offset calculation.
     * Reusing the input weights for the update makes each combined weight proportional to its
     * square, because the updating merge multiplies the stored weights by the supplied weights. */
    double weighted_u_sum = 0.0;
    double weight_sum = 0.0;
    for (int i = 0; i < ring_vertex_count; i++) {
      double weight = loop_weights[i];
      if (recalculate_weights) {
        weight *= loop_weights[i];
      }
      weighted_u_sum += i * weight;
      weight_sum += weight;
    }
    const float expected_u = float(weighted_u_sum / weight_sum);

    if (recalculate_weights) {
      BM_vert_loop_groups_data_layer_merge_weights(
          bm, loop_groups, uv_layer_index, loop_weights.data());
    }
    else {
      BM_vert_loop_groups_data_layer_merge(bm, loop_groups, uv_layer_index);
    }
    BM_ITER_ELEM (loop, &iter, center_vertex, BM_LOOPS_OF_VERT) {
      SCOPED_TRACE(::testing::Message() << "Center corner " << BM_elem_index_get(loop));
      EXPECT_EQ(loop_uv(loop, uv_offset).y, -0.25f);
      EXPECT_NEAR(loop_uv(loop, uv_offset).x, expected_u, 1e-6f);
    }
  }
}

TEST_F(BMeshInterpTest, SharedFloat2InterpolationPreservesConstantsAndAliasing)
{
  /* Exercise the shared callback directly, as other editing operations do. Seven equal weights
   * are rounded, so their sum need not be exactly one. The constant V must stay exact while U
   * is averaged. Repeat with the destination inside the source array to check deferred writes. */
  BM_data_layer_add_named(bm, &bm->ldata, CD_PROP_FLOAT2, "UVMap");
  const int layer_index = CustomData_get_layer_index(&bm->ldata, CD_PROP_FLOAT2);
  float2 values[7];
  const void *sources[7];
  float weights[7];
  for (int i = 0; i < 7; i++) {
    values[i] = float2(float(i), 0.7f);
    sources[i] = &values[i];
    weights[i] = 1.0f / 7.0f;
  }
  float2 result;
  CustomData_bmesh_interp_n(&bm->ldata, sources, weights, 7, &result, layer_index);
  EXPECT_FLOAT_EQ(result.x, 3.0f);
  EXPECT_EQ(result.y, 0.7f);
  CustomData_bmesh_interp_n(&bm->ldata, sources, weights, 7, &values[3], layer_index);
  EXPECT_EQ(values[3], result);

  /* With no contribution, retain the original zero result rather than reading a reference. */
  std::fill_n(weights, 7, 0.0f);
  CustomData_bmesh_interp_n(&bm->ldata, sources, weights, 7, &result, layer_index);
  EXPECT_EQ(result, float2(0.0f));
}

}  // namespace blender
