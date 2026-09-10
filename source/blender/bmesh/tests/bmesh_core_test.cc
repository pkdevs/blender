/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_math_vector_c.hh"

#include "BKE_gtest_base.hh"

#include "bmesh.hh"
#include "intern/bmesh_polygon_edgenet.hh"

namespace blender {

class BMeshCoreTest : public bke::BlenderGTestBase {};

TEST_F(BMeshCoreTest, BMVertCreate)
{
  BMesh *bm;
  BMVert *bv1, *bv2, *bv3;
  const float co1[3] = {1.0f, 2.0f, 0.0f};

  BMeshCreateParams bmesh_create_params{};
  bmesh_create_params.use_toolflags = true;
  bm = BM_mesh_create(&bm_mesh_allocsize_default, &bmesh_create_params);
  EXPECT_EQ(bm->totvert, 0);
  /* make a custom layer so we can see if it is copied properly */
  BM_data_layer_add(bm, &bm->vdata, CD_PROP_FLOAT);
  bv1 = BM_vert_create(bm, co1, nullptr, BM_CREATE_NOP);
  ASSERT_TRUE(bv1 != nullptr);
  EXPECT_EQ(bv1->co[0], 1.0f);
  EXPECT_EQ(bv1->co[1], 2.0f);
  EXPECT_EQ(bv1->co[2], 0.0f);
  EXPECT_TRUE(is_zero_v3(bv1->no));
  EXPECT_EQ(bv1->head.htype, char(BM_VERT));
  EXPECT_EQ(bv1->head.hflag, 0);
  EXPECT_EQ(bv1->head.api_flag, 0);
  bv2 = BM_vert_create(bm, nullptr, nullptr, BM_CREATE_NOP);
  ASSERT_TRUE(bv2 != nullptr);
  EXPECT_TRUE(is_zero_v3(bv2->co));
  /* create with example should copy custom data but not select flag */
  BM_vert_select_set(bm, bv2, true);
  BM_elem_float_data_set(&bm->vdata, bv2, CD_PROP_FLOAT, 1.5f);
  bv3 = BM_vert_create(bm, co1, bv2, BM_CREATE_NOP);
  ASSERT_TRUE(bv3 != nullptr);
  EXPECT_FALSE(BM_elem_flag_test((BMElem *)bv3, BM_ELEM_SELECT));
  EXPECT_EQ(BM_elem_float_data_get(&bm->vdata, bv3, CD_PROP_FLOAT), 1.5f);
  EXPECT_EQ(BM_mesh_elem_count(bm, BM_VERT), 3);
  BM_mesh_free(bm);
}

TEST_F(BMeshCoreTest, FaceSplitEdgeNetCornerAttributes)
{
  for (const bool add_uv_layer : {false, true}) {
    BMeshCreateParams params{};
    BMesh *bm = BM_mesh_create(&bm_mesh_allocsize_default, &params);
    BM_data_layer_add(bm, &bm->ldata, CD_PROP_FLOAT);
    if (add_uv_layer) {
      BM_data_layer_add(bm, &bm->ldata, CD_PROP_FLOAT2);
    }

    const float coords[5][3] = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}, {1, 1, 0}};
    BMVert *verts[5];
    for (int i = 0; i < 5; i++) {
      verts[i] = BM_vert_create(bm, coords[i], nullptr, BM_CREATE_NOP);
    }
    BMFace *face = BM_face_create_verts(bm, verts, 4, nullptr, BM_CREATE_NOP, true);
    BM_face_normal_update(face);
    BMIter iter;
    BMLoop *loop;
    BM_ITER_ELEM (loop, &iter, face, BM_LOOPS_OF_FACE) {
      BM_elem_float_data_set(
          &bm->ldata, loop, CD_PROP_FLOAT, 4.0f + loop->v->co[0] + 2.0f * loop->v->co[1]);
    }

    /* Split across an interior vertex to exercise both copying existing corners and
     * interpolating new corners. An unrelated UV layer must not affect either operation. */
    BMEdge *edges[2] = {BM_edge_create(bm, verts[0], verts[4], nullptr, BM_CREATE_NOP),
                        BM_edge_create(bm, verts[4], verts[2], nullptr, BM_CREATE_NOP)};
    Vector<BMFace *> faces;
    EXPECT_TRUE(BM_face_split_edgenet(bm, face, edges, 2, &faces));
    EXPECT_EQ(faces.size(), 2);
    EXPECT_EQ(bm->totloop, 8);
    for (BMFace *result_face : faces) {
      BM_ITER_ELEM (loop, &iter, result_face, BM_LOOPS_OF_FACE) {
        EXPECT_NEAR(BM_elem_float_data_get(&bm->ldata, loop, CD_PROP_FLOAT),
                    4.0f + loop->v->co[0] + 2.0f * loop->v->co[1],
                    1e-5f);
      }
    }
    BM_mesh_free(bm);
  }
}

}  // namespace blender
