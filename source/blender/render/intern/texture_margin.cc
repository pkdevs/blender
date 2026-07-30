/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup render
 */

#include "BLI_assert.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"

#include "IMB_imbuf.hh"
#include "IMB_interp.hh"

#include "MEM_guardedalloc.h"

#include "zbuf.h" /* For rasterizer (#ZSpan and associated functions). */

#include "RE_texture_margin.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace blender {

/**
 * The map class contains both a pixel map which maps out face indices for all UV-polygons and
 * adjacency tables.
 */
class TextureMarginMap {
  static const int directions[4][2];
  static const int distances[4];

  /* Mesh, UV and djkstra data shared between different methods. */
  int w_, h_;
  float uv_offset_[2];

  OffsetIndices<int> faces_;
  Span<int> corner_edges_;
  Span<int> corner_verts_;
  Span<float2> uv_map_;
  Span<bool> uv_seams_;

  Vector<uint32_t> pixel_data_;
  char *mask_;

  /* Rasterization arguments. */
  uint32_t value_to_store_;
  bool write_mask_;

  /* Tables for pixel lookup. */
  /* Maps face corners to other face corners with shared edge. */
  Vector<int> corner_adjacency_;
  /* Maps corners to their corresponding face. */
  Array<int> corner_to_face_;
  /* Is edge connected in UV space with its shared edge. */
  Vector<bool> edge_uv_connectivity_;
  /* Face winding in UV space. */
  Vector<bool> face_uv_winding_;

#define WINDING_CW false
#define WINDING_CCW true

 public:
  TextureMarginMap(const size_t w,
                   const size_t h,
                   const float uv_offset[2],
                   const OffsetIndices<int> faces,
                   const Span<int> corner_edges,
                   const Span<int> corner_verts,
                   const Span<float2> uv_map,
                   const Span<bool> uv_seams)
      : w_(w),
        h_(h),
        faces_(faces),
        corner_edges_(corner_edges),
        corner_verts_(corner_verts),
        uv_map_(uv_map),
        uv_seams_(uv_seams)
  {
    copy_v2_v2(uv_offset_, uv_offset);
  }

  ~TextureMarginMap() {}

  void set_pixel(const int x, const int y, const uint32_t value)
  {
    BLI_assert(x >= 0 && x < w_);
    BLI_assert(y >= 0 && y < h_);

    pixel_data_[y * w_ + x] = value;
  }

  uint32_t get_pixel(const int x, const int y) const
  {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) {
      return 0xFFFFFFFF;
    }

    return pixel_data_[y * w_ + x];
  }

  static void zscan_store_pixel(
      void *map, int x, int y, [[maybe_unused]] float u, [[maybe_unused]] float v)
  {
    TextureMarginMap *m = static_cast<TextureMarginMap *>(map);

    if (m->write_mask_) {
      /* if there is a mask and write_mask_ is true, write to the mask */
      m->mask_[y * m->w_ + x] = 1;
      m->set_pixel(x, y, m->value_to_store_);
    }
    else {
      /* if there is a mask and write_mask_ is false, read the mask
       * to decide if the map needs to be written
       */
      if (m->mask_[y * m->w_ + x] != 0) {
        m->set_pixel(x, y, m->value_to_store_);
      }
    }
  }

  void rasterize_tris(char *mask,
                      const bool draw_new_mask,
                      const Span<float3> vert_positions,
                      const bool conservative)
  {
    ZSpan zspan;
    zbuf_alloc_span(&zspan, w_, h_);

    pixel_data_.resize(w_ * h_, 0xFFFFFFFF);

    Array<int3> corner_tris(poly_to_tri_count(faces_.size(), corner_edges_.size()));
    bke::mesh::corner_tris_calc(vert_positions, faces_, corner_verts_, corner_tris);

    Array<int> tri_faces(corner_tris.size());
    bke::mesh::corner_tris_calc_face_indices(faces_, tri_faces);

    mask_ = mask;
    write_mask_ = draw_new_mask;

    for (int fill = conservative ? 0 : 1; fill < 2; fill++) {
      for (const int i : corner_tris.index_range()) {
        const int3 tri = corner_tris[i];
        float vec[3][2];

        for (int a = 0; a < 3; a++) {
          const float *uv = uv_map_[tri[a]];
          vec[a][0] = (uv[0] - uv_offset_[0]) * float(w_);
          vec[a][1] = (uv[1] - uv_offset_[1]) * float(h_);
        }

        /* NOTE: we need the top bit for the dijkstra distance map. */
        BLI_assert(tri_faces[i] < 0x80000000);

        value_to_store_ = tri_faces[i];

        if (fill) {
          zspan_rasterize_triangle(
              &zspan, this, vec[0], vec[1], vec[2], TextureMarginMap::zscan_store_pixel);
        }
        else {
          zspan_rasterize_conservative_wireframe(
              &zspan, this, vec[0], vec[1], vec[2], TextureMarginMap::zscan_store_pixel);
        }
      }
    }

    zbuf_free_span(&zspan);
  }

/* The map contains 2 kinds of pixels: DijkstraPixels and face indices. The top bit determines
 * what kind it is. With the top bit set, it is a 'dijkstra' pixel. The bottom 4 bits encode the
 * direction of the shortest path and the remaining 27 bits are used to store the distance. If
 * the top bit is not set, the rest of the bits is used to store the face index.
 */
#define PackDijkstraPixel(dist, dir) (0x80000000 + ((dist) << 4) + (dir))
#define DijkstraPixelGetDistance(dp) (((dp) ^ 0x80000000) >> 4)
#define DijkstraPixelGetDirection(dp) ((dp) & 0xF)
#define IsDijkstraPixel(dp) ((dp) & 0x80000000)
#define DijkstraPixelIsUnset(dp) ((dp) == 0xFFFFFFFF)

  /**
   * Use dijkstra's algorithm to 'grow' a border around the polygons marked in the map.
   * For each pixel mark which direction is the shortest way to a face.
   */
  void grow_dijkstra(int margin)
  {
    class DijkstraActivePixel {
     public:
      DijkstraActivePixel(int dist, int _x, int _y) : distance(dist), x(_x), y(_y) {}
      int distance;
      int x, y;
    };
    auto cmp_dijkstrapixel_fun = [](DijkstraActivePixel const &a1, DijkstraActivePixel const &a2) {
      return a1.distance > a2.distance;
    };

    Vector<DijkstraActivePixel> active_pixels;
    for (int y = 0; y < h_; y++) {
      for (int x = 0; x < w_; x++) {
        if (DijkstraPixelIsUnset(get_pixel(x, y))) {
          for (int i = 0; i < 4; i++) {
            int xx = x - directions[i][0];
            int yy = y - directions[i][1];

            if (xx >= 0 && xx < w_ && yy >= 0 && yy < h_ && !IsDijkstraPixel(get_pixel(xx, yy))) {
              set_pixel(x, y, PackDijkstraPixel(distances[i], i));
              active_pixels.append(DijkstraActivePixel(distances[i], x, y));

              /* All distances are the same so we can pick any valid direction. */
              break;
            }
          }
        }
      }
    }

    /* Not needed because all distances are the same. */
    // std::make_heap(active_pixels.begin(), active_pixels.end(), cmp_dijkstrapixel_fun);

    while (active_pixels.size()) {
      std::pop_heap(active_pixels.begin(), active_pixels.end(), cmp_dijkstrapixel_fun);
      DijkstraActivePixel p = active_pixels.pop_last();

      const int dist = p.distance;

      if (dist >= margin) {
        continue;
      }

      for (int i = 0; i < 4; i++) {
        const int new_distance = dist + distances[i];

        if (new_distance > margin) {
          continue;
        }

        const int x = p.x + directions[i][0];
        const int y = p.y + directions[i][1];

        if (x >= 0 && x < w_ && y >= 0 && y < h_) {
          uint32_t dp = get_pixel(x, y);

          if (IsDijkstraPixel(dp) && (DijkstraPixelGetDistance(dp) > new_distance)) {
            BLI_assert(DijkstraPixelGetDirection(dp) != i);
            set_pixel(x, y, PackDijkstraPixel(new_distance, i));
            active_pixels.append(DijkstraActivePixel(new_distance, x, y));
            std::push_heap(active_pixels.begin(), active_pixels.end(), cmp_dijkstrapixel_fun);
          }
        }
      }
    }
  }

  void build_tables(const size_t edges_num)
  {
    corner_to_face_ = bke::mesh::build_corner_to_face_map(faces_);

    corner_adjacency_.resize(corner_to_face_.size(), -1);
    edge_uv_connectivity_.resize(edges_num, false);
    face_uv_winding_.resize(faces_.size());

    /* Temporary buffer to find connected corners. */
    Vector<int> edge_to_corner;
    edge_to_corner.resize(edges_num * 2, -1);

    for (const int64_t face_a : faces_.index_range()) {
      /* Use shoelace signed area formula to determine face winding. */
      float signed_area = 0;

      for (const int corner_a1 : faces_[face_a]) {
        const int corner_a2 = face_next_corner(face_a, corner_a1);
        const int edge = corner_edges_[corner_a1];

        const float2 uv_a1 = corner_uv(corner_a1);
        const float2 uv_a2 = corner_uv(corner_a2);

        signed_area += math::cross(uv_a1, uv_a2);

        /* Only faces with different winding (where a1 equals b2)
         * should be considered adjacent.
         * Use vertex indices order to find face with different winding.*/
        const int edge_verts_order = corner_verts_[corner_a1] <= corner_verts_[corner_a2] ? 0 : 1;
        const int edge_lookup_idx_a = edge * 2 + edge_verts_order;
        const int edge_lookup_idx_b = edge * 2 + (1 - edge_verts_order);

        const int prev_corner_a = edge_to_corner[edge_lookup_idx_a];
        edge_to_corner[edge_lookup_idx_a] = corner_a1;

        const int corner_b1 = edge_to_corner[edge_lookup_idx_b];

        if (corner_b1 >= 0) {
          const int face_b = corner_to_face_[corner_b1];
          const int corner_b2 = face_next_corner(face_b, corner_b1);

          if (prev_corner_a >= 0) {
            corner_adjacency_[prev_corner_a] = -1;
          }
          
          corner_adjacency_[corner_a1] = corner_b1;
          corner_adjacency_[corner_b1] = corner_a1;

          const float2 uv_b1 = corner_uv(corner_b1);
          const float2 uv_b2 = corner_uv(corner_b2);

          const bool uv_connected = (math::length_squared(uv_a1 - uv_b2) <= FLT_EPSILON) &&
                                    (math::length_squared(uv_a2 - uv_b1) <= FLT_EPSILON);

          edge_uv_connectivity_[edge] = uv_connected;
          edge_uv_connectivity_[corner_edges_[corner_b1]] = uv_connected;
        }
      }

      face_uv_winding_[face_a] = (signed_area >= 0) ? WINDING_CCW : WINDING_CW;
    }
  }

  /**
   * Walk over the map and for margin pixels follow the direction stored in the bottom 3
   * bits back to the face. Then look up the pixel from the next face.
   */
  void lookup_pixels(ImBuf *ibuf, ImBuf *im_read, int maxFaceSteps)
  {
    float4 *ibuf_ptr_fl = reinterpret_cast<float4 *>(ibuf->float_data_for_write());
    uchar4 *ibuf_ptr_ch = reinterpret_cast<uchar4 *>(ibuf->byte_data_for_write());

    size_t pixel_index = 0;

    for (int y = 0; y < h_; y++) {
      for (int x = 0; x < w_; x++, pixel_index++) {
        uint32_t dp = pixel_data_[pixel_index];

        int xx = x, yy = y;
        bool inside_face = false;

        if (IsDijkstraPixel(dp)) {
          /* Margin pixel */
          if (DijkstraPixelIsUnset(dp)) {
            continue;
          }

          int dist = DijkstraPixelGetDistance(dp);
          int direction = DijkstraPixelGetDirection(dp);

          /* Follow the dijkstra directions to find the face this margin pixels belongs to. */
          while (dist > 0) {
            xx -= directions[direction][0];
            yy -= directions[direction][1];
            dp = get_pixel(xx, yy);
            dist -= distances[direction];
            BLI_assert(!dist || (dist == DijkstraPixelGetDistance(dp)));
            direction = DijkstraPixelGetDirection(dp);
          }

          BLI_assert(!IsDijkstraPixel(start_poly));
        }
        else {
          /* Face pixel */
          /* Process face pixels only close to empty pixels to improve performance a bit. */
          bool found_empty = false;

          for (int step_y = -2; step_y <= 2; step_y++) {
            for (int step_x = -2; step_x <= 2; step_x++) {
              if (step_x == 0 && step_y == 0) {
                continue;
              }

              const int tmp_x = x + step_x;
              const int tmp_y = y + step_y;

              if (tmp_x < 0 || tmp_x >= w_ || tmp_y < 0 || tmp_y >= h_) {
                continue;
              }

              if (IsDijkstraPixel(get_pixel(x + step_x, y + step_y))) {
                found_empty = true;
                break;
              }
            }
          }

          if (!found_empty) {
            continue;
          }

          inside_face = true;
        }

        const uint32_t start_face = get_pixel(xx, yy);
        float2 src_point = float2(x, y) + 0.5f;

        int prev_face = -1;
        int dst_face;
        float src_edge_perp_signed_dist;
        float2 dst_point;
        float2 point_on_src_edge, point_on_dst_edge;

        bool found_edge = lookup_pixel(src_point,
                                       prev_face,
                                       start_face,
                                       &dst_point,
                                       &dst_face,
                                       &src_edge_perp_signed_dist,
                                       &point_on_src_edge,
                                       &point_on_dst_edge);

        if (!found_edge) {
          continue;
        }

        /* Project source or destination point on an edge based on edge signed distance. */
        if (src_edge_perp_signed_dist > 0) {
          src_point = point_on_src_edge;
        }
        else {
          dst_point = point_on_dst_edge;
        }

        bool found_pixel_in_polygon = true;

        for (int i = 0; i < maxFaceSteps; i++) {
          /* If at least one pixel in interpolation range contains target face
           * then it's safe to sample interpolated data from this point.
           * A better solution is to analytically check if point is inside
           * target face, but this is fine enough heuristic considering
           * we may have some complex concave face here. */
          const int nx = std::clamp(int(floor(dst_point.x - 0.5f)), -1, w_ - 1);
          const int ny = std::clamp(int(floor(dst_point.y - 0.5f)), -1, h_ - 1);

          if (get_pixel(nx, ny) == dst_face || get_pixel(nx + 1, ny) == dst_face ||
              get_pixel(nx, ny + 1) == dst_face || get_pixel(nx + 1, ny + 1) == dst_face)
          {
            break;
          }

          /* Look up again, but starting from the face we were expected to land in. */
          prev_face = dst_face;
          float tmp_edge_sd;

          found_edge = lookup_pixel(dst_point,
                                    prev_face,
                                    dst_face,
                                    &dst_point,
                                    &dst_face,
                                    &tmp_edge_sd,
                                    &point_on_src_edge,
                                    &point_on_dst_edge);

          /* Selected edge does not have adjacent face, so clamp
           * destination point inside previous face. */
          if (!found_edge) {
            found_pixel_in_polygon = false;
            // dst_point = point_on_dst_edge;
            break;
          }
        }

        if (!found_pixel_in_polygon) {
          continue;
        }

        float w = std::clamp(src_edge_perp_signed_dist + 0.5f, 0.0f, 1.0f);

        if (ibuf_ptr_fl) {
          float4 inside_col = imbuf::interpolate_bilinear_fl(
              im_read, src_point.x - 0.5f, src_point.y - 0.5f);
          float4 outside_col = imbuf::interpolate_bilinear_fl(
              im_read, dst_point.x - 0.5f, dst_point.y - 0.5f);

          ibuf_ptr_fl[pixel_index] = math::interpolate(inside_col, outside_col, w);
        }

        if (ibuf_ptr_ch) {
          uchar4 inside_col = imbuf::interpolate_bilinear_byte(
              im_read, src_point.x - 0.5f, src_point.y - 0.5f);
          uchar4 outside_col = imbuf::interpolate_bilinear_byte(
              im_read, dst_point.x - 0.5f, dst_point.y - 0.5f);

          ibuf_ptr_ch[pixel_index] = math::interpolate(inside_col, outside_col, w);
        }

        /* Add our new pixels to the assigned pixel map. */
        mask_[pixel_index] = 1;
      }
    }
  }

 private:
  float2 corner_uv(const int corner) const
  {
    return (uv_map_[corner] - float2(uv_offset_[0], uv_offset_[1])) * float2(w_, h_);
  }

  int face_next_corner(const int face, const int corner) const
  {
    int next_corner = corner + 1;

    if (next_corner >= faces_[face].one_after_last()) {
      next_corner = faces_[face].start();
    }

    return next_corner;
  }

  /**
   * Find which edge of src_face is closest to x,y. Look up its adjacent UV-edge and face.
   * Returns the location of the equivalent pixel in the other face and other information.
   * Returns true if a nearest edge has an adjacent face, false if it doesn't, which can happen if
   * the shared edge is invalid, or if the UV-edge doesn't have an adjacent face.
   */
  bool lookup_pixel(const float2 point,
                    const int prev_face,
                    const int src_face,

                    float2 *r_dst_point,
                    int *r_other_face,
                    float *r_src_edge_perp_signed_dist,
                    float2 *r_point_on_src_edge,
                    float2 *r_point_on_dsr_edge) const
  {
    const bool src_face_winding = face_uv_winding_[src_face];

    int found_corner = -1;
    float min_dist = FLT_MAX;

    bool found_valid;
    float found_perp_signed_dist;
    float found_t;
    float found_ablen;
    float2 found_point_on_edge;

    /* Find the closest edge on which the point x,y can be projected. */
    for (const int corner_1 : faces_[src_face]) {
      bool is_valid = true;

      const bool has_adjacent_face = corner_adjacency_[corner_1] >= 0;
      is_valid &= has_adjacent_face;

      /* Only non-seam edges disconnected in UV space should be
       * considered for the first iteration. */
      if (has_adjacent_face && prev_face == -1) {
        const int edge = corner_edges_[corner_1];
        is_valid &= !uv_seams_[edge];
        is_valid &= !edge_uv_connectivity_[edge];
      }

      /* Consider selecting only valid edges for the first iteration. */
      if (!is_valid && prev_face == -1) {
        continue;
      }

      const int corner_2 = face_next_corner(src_face, corner_1);

      const float2 uv_1 = corner_uv(corner_1);
      const float2 uv_2 = corner_uv(corner_2);

      /* Vector AB is the vector from the first edge point to the second edge point.
       * Vector AP is the vector from the first edge point to our point under investigation. */
      const float2 ab = uv_2 - uv_1;
      const float2 ap = point - uv_1;

      const float ablensq = math::length_squared(ab);
      const float ablen = sqrtf(ablensq);

      /* Project ap onto ab. */
      const float dotv = math::dot(ab, ap);
      const float t = dotv / ablensq;
      const float t_clamped = std::clamp(t, 0.0f, 1.0f);

      /* Works both for the insides and outsides of convex and concave faces. */
      const float distance = math::length(uv_1 + (t_clamped * ab) - point) +
                             fabs(t_clamped - t) * ablen;

      if (distance < min_dist) {
        /* Store the info of the closest edge so far. */
        float2 perpendicular_ab = float2(ab.y, -ab.x);
        perpendicular_ab *= (src_face_winding == WINDING_CW) ? -1 : 1;

        const float signed_dist = math::dot(perpendicular_ab, ap) / ablen;

        found_corner = corner_1;
        min_dist = distance;

        found_valid = is_valid;
        found_perp_signed_dist = signed_dist;
        found_t = t;
        found_ablen = ablen;
        found_point_on_edge = uv_1 + t * ab;
      }
    }

    if (found_corner < 0 || !found_valid) {
      return false;
    }

    /* Get the 'other' corner. I.E. the UV corner from the neighbor face. */
    const int other_corner_1 = corner_adjacency_[found_corner];

    const int dst_face = corner_to_face_[other_corner_1];
    bool dst_face_winding = face_uv_winding_[dst_face];

    const int other_corner_2 = face_next_corner(dst_face, other_corner_1);

    const float2 other_uv_1 = corner_uv(other_corner_1);
    const float2 other_uv_2 = corner_uv(other_corner_2);

    /* Calculate the vector from the other edge second point to its first point.
     * Edge vertex order is inverted for valid connected faces. */
    const float2 other_ab = other_uv_1 - other_uv_2;
    const float2 projected_point = other_uv_2 + (found_t * other_ab);

    float2 perpendicular_other_ab = float2(other_ab.y, -other_ab.x);
    perpendicular_other_ab *= (dst_face_winding == WINDING_CW) ? -1 : 1;

    /* The new point is `found_sd * length(other_ab) / length(ab)` units
     * away from projected_point at a 90 degree angle to other_ab */
    const float2 new_point = projected_point +
                             (found_perp_signed_dist / found_ablen) * perpendicular_other_ab;

    *r_dst_point = new_point;
    *r_src_edge_perp_signed_dist = found_perp_signed_dist;
    *r_other_face = dst_face;
    *r_point_on_src_edge = found_point_on_edge;
    *r_point_on_dsr_edge = projected_point;

    return true;
  }
};  // class TextureMarginMap

const int TextureMarginMap::directions[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
const int TextureMarginMap::distances[4] = {1, 1, 1, 1};

void RE_generate_texturemargin_adjacentfaces(ImBuf *ibuf,
                                             char *mask,
                                             const int margin,
                                             const Mesh *mesh,
                                             StringRef uv_layer,
                                             const float uv_offset[2],
                                             const bool conservative)
{
  clock_t start = clock();
  /* Initialize TextureMarginMap with all necessary mesh attributes. */
  const StringRef name = uv_layer.is_empty() ? mesh->active_uv_map_name() : uv_layer;
  const bke::AttributeAccessor attributes = mesh->attributes();

  const VArraySpan<float2> uv_map = *attributes.lookup<float2>(name, bke::AttrDomain::Corner);
  const VArraySpan<bool> uv_seams = *attributes.lookup_or_default<bool>(
      "uv_seam", bke::AttrDomain::Edge, false);

  TextureMarginMap map(ibuf->x,
                       ibuf->y,
                       uv_offset,
                       mesh->faces(),
                       mesh->corner_edges(),
                       mesh->corner_verts(),
                       uv_map,
                       uv_seams);

  bool draw_new_mask = false;

  if (mask) {
    mask = MEM_dupalloc(mask);
  }
  else {
    mask = MEM_new_array_zeroed<char>(size_t(ibuf->x) * size_t(ibuf->y), __func__);
    draw_new_mask = true;
  }
  
  clock_t raster_start = clock();

  map.rasterize_tris(mask, draw_new_mask, mesh->vert_positions(), conservative);
  clock_t dijkstra_start = clock();
  map.grow_dijkstra(margin);

  /* Extend (with averaging). Those will be partially overwritten, but it
   * helps linear interpolations on the edges of faces during pixel lookup.
   * Produced mask is used to preserve original colors outside of expected margin. */
  clock_t extend_start = clock();
  char *tmpmask = MEM_dupalloc(mask);
  IMB_filter_extend(ibuf, tmpmask, margin);
  ImBuf *im_read = IMB_dupImBuf(ibuf);

  /* Looking further than 3 faces is usually not worth it for texture margin. */
  clock_t tables_build_start = clock();
  map.build_tables(mesh->edges_num);
  clock_t lookup_start = clock();
  map.lookup_pixels(ibuf, im_read, 3);
  clock_t finalize_start = clock();

  /* Extend around original pixels and pixels blended with adjacent faces. */
  IMB_filter_extend(ibuf, mask, margin);

  /* Restore original pixels outside expected margin. */
  const float4 *readbuf_ptr_fl = reinterpret_cast<const float4 *>(im_read->float_data());
  const uchar4 *readbuf_ptr_ch = reinterpret_cast<const uchar4 *>(im_read->byte_data());

  float4 *ibuf_ptr_fl = reinterpret_cast<float4 *>(ibuf->float_data_for_write());
  uchar4 *ibuf_ptr_ch = reinterpret_cast<uchar4 *>(ibuf->byte_data_for_write());

  size_t pixels_num = size_t(ibuf->x) * size_t(ibuf->y);

  for (size_t i = 0; i < pixels_num; i++) {
    if (tmpmask[i] != FILTER_MASK_NULL) {
      continue;
    }

    if (ibuf_ptr_fl) {
      ibuf_ptr_fl[i] = readbuf_ptr_fl[i];
    }
    if (ibuf_ptr_ch) {
      ibuf_ptr_ch[i] = readbuf_ptr_ch[i];
    }
  }

  IMB_freeImBuf(im_read);
  MEM_delete(mask);
  MEM_delete(tmpmask);
  
  clock_t end_time = clock();

  printf("INIT  Time: %f sec\n", float(raster_start - start) / CLOCKS_PER_SEC);
  printf("RASRT Time: %f sec\n", float(dijkstra_start - raster_start) / CLOCKS_PER_SEC);
  printf("DIJKS Time: %f sec\n", float(extend_start - dijkstra_start) / CLOCKS_PER_SEC);
  printf("EXTEN Time: %f sec\n", float(tables_build_start - extend_start) / CLOCKS_PER_SEC);
  printf("BTABL Time: %f sec\n", float(lookup_start - tables_build_start) / CLOCKS_PER_SEC);
  printf("LOOKU Time: %f sec\n", float(finalize_start - lookup_start) / CLOCKS_PER_SEC);
  printf("FINAL Time: %f sec\n", float(end_time - finalize_start) / CLOCKS_PER_SEC);
  printf("\nFULL  Time: %f sec\n", float(end_time - start) / CLOCKS_PER_SEC);
}

}  // namespace blender
