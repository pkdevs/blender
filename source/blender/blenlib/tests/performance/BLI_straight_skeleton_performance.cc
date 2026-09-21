/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

#include "testing/testing.h"

#ifdef WITH_TRACY
#  include "tracy/Tracy.hpp"
#endif

#include "BLI_array.hh"
#include "BLI_function_ref.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_straight_skeleton.hh"
#include "BLI_time.hh"
#include "BLI_vector.hh"

namespace blender::straightskeleton {

/* -------------------------------------------------------------------- */
/** \name Timing and Table Formatting Helpers
 * \{ */

static double run_iterations(const FunctionRef<void()> function, const int default_iterations = 1)
{
  int iterations = default_iterations;
  if (const char *env = std::getenv("BENCHMARK_ITERATIONS")) {
    iterations = std::max(1, std::atoi(env));
  }
  double min_time = 1e30;
  for (int repetition = 0; repetition < iterations; repetition++) {
    const double time_before = BLI_time_now_seconds();
    function();
    const double time_after = BLI_time_now_seconds();
    min_time = std::min(min_time, time_after - time_before);
  }
  return min_time;
}

static void print_time(const double seconds)
{
  if (seconds < 0.001) {
    printf(" %8.1f µs |", seconds * 1000000.0);
  }
  else if (seconds < 1.0) {
    printf(" %8.2f ms |", seconds * 1000.0);
  }
  else {
    printf(" %8.3f s  |", seconds);
  }
}

static void print_table_header(const char *title, const char *n_label)
{
  printf("\n#### %s\n\n", title);
  printf("| %s | vertices | full skeleton | 5 segments | events | flips | faces |\n", n_label);
  printf("|---:|---------:|--------------:|-----------:|-------:|------:|------:|\n");
  fflush(stdout);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Geometry Storage and Construction Helpers
 * \{ */

struct GeneratedPolygon {
  Array<double2> vert;
  Vector<int> outer_polygon;
  Vector<int> hole_offsets_buf;
  Vector<int> hole_vert_indices;

  StraightSkeletonInput to_input() const
  {
    StraightSkeletonInput input;
    input.vert = vert.as_span();
    input.outer_polygon = outer_polygon.as_span();
    if (!hole_offsets_buf.is_empty()) {
      input.hole_offsets = OffsetIndices<int>(hole_offsets_buf.as_span());
      input.hole_vert_indices = hole_vert_indices.as_span();
    }
    return input;
  }
};

/**
 * Circle approximating a regular n-gon with n vertices, oriented CCW.
 */
static GeneratedPolygon make_circle(const int n, const double radius = 100.0)
{
  GeneratedPolygon geom;
  geom.vert.reinitialize(n);
  geom.outer_polygon.reserve(n);
  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    geom.vert[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    geom.outer_polygon.append(i);
  }
  return geom;
}

/**
 * Square with subdivided edges, where n is the total number of vertices on each side.
 * Each side has n vertices (n - 1 segments). Total vertices: 4 * (n - 1), oriented CCW.
 */
static GeneratedPolygon make_subdivided_square(const int n, const double half_size = 100.0)
{
  BLI_assert(n >= 2);
  const int segments_per_side = n - 1;
  const int total_verts_num = 4 * segments_per_side;

  GeneratedPolygon geom;
  geom.vert.reinitialize(total_verts_num);
  geom.outer_polygon.reserve(total_verts_num);

  const double l = half_size;
  int idx = 0;

  /* Side 0: Bottom edge (left to right, -l to +l at y = -l). */
  for (const int k : IndexRange(segments_per_side)) {
    const double x = -l + (2.0 * l * double(k)) / double(segments_per_side);
    geom.vert[idx] = double2(x, -l);
    geom.outer_polygon.append(idx);
    idx++;
  }

  /* Side 1: Right edge (bottom to top, -l to +l at x = +l). */
  for (const int k : IndexRange(segments_per_side)) {
    const double y = -l + (2.0 * l * double(k)) / double(segments_per_side);
    geom.vert[idx] = double2(l, y);
    geom.outer_polygon.append(idx);
    idx++;
  }

  /* Side 2: Top edge (right to left, +l to -l at y = +l). */
  for (const int k : IndexRange(segments_per_side)) {
    const double x = l - (2.0 * l * double(k)) / double(segments_per_side);
    geom.vert[idx] = double2(x, l);
    geom.outer_polygon.append(idx);
    idx++;
  }

  /* Side 3: Left edge (top to bottom, +l to -l at x = -l). */
  for (const int k : IndexRange(segments_per_side)) {
    const double y = l - (2.0 * l * double(k)) / double(segments_per_side);
    geom.vert[idx] = double2(-l, y);
    geom.outer_polygon.append(idx);
    idx++;
  }

  return geom;
}

/**
 * Circle with circular holes inside:
 * n vertices on outer circle (CCW), and n smaller non-touching circular 8-gon holes (CW)
 * centered on the circle at half-radius.
 */
static GeneratedPolygon make_circle_with_holes(const int n,
                                               const int hole_verts_num = 8,
                                               const double radius = 100.0)
{
  BLI_assert(n >= 3);
  const int total_verts_num = n + n * hole_verts_num;

  GeneratedPolygon geom;
  geom.vert.reinitialize(total_verts_num);
  geom.outer_polygon.reserve(n);

  /* Outer circle (CCW). */
  for (const int i : IndexRange(n)) {
    const double theta = (2.0 * M_PI * double(i)) / double(n);
    geom.vert[i] = double2(radius * std::cos(theta), radius * std::sin(theta));
    geom.outer_polygon.append(i);
  }

  /* Holes centered at half-radius. */
  const double r_mid = radius * 0.5;
  const double d_adj = 2.0 * r_mid * std::sin(M_PI / double(n));
  const double r_hole = std::min(0.35 * r_mid, 0.35 * d_adj);

  geom.hole_offsets_buf.reserve(n + 1);
  geom.hole_offsets_buf.append(0);
  geom.hole_vert_indices.reserve(n * hole_verts_num);

  int v_idx = n;
  for (const int j : IndexRange(n)) {
    /* Offset by half-step so holes do not lie on radial rays of outer vertices. */
    const double phi = (2.0 * M_PI * (double(j) + 0.5)) / double(n);
    const double2 center(r_mid * std::cos(phi), r_mid * std::sin(phi));

    /* 8-gon hole oriented Clockwise (CW): angle decreases. */
    for (const int k : IndexRange(hole_verts_num)) {
      const double alpha = (-2.0 * M_PI * double(k)) / double(hole_verts_num);
      geom.vert[v_idx] = center + double2(r_hole * std::cos(alpha), r_hole * std::sin(alpha));
      geom.hole_vert_indices.append(v_idx);
      v_idx++;
    }
    geom.hole_offsets_buf.append(int(geom.hole_vert_indices.size()));
  }

  return geom;
}

/**
 * Koch snowflake fractal, where n is the number of refinement iterations.
 * Iteration 0 is an equilateral triangle (3 vertices, CCW).
 * Total vertices: 3 * 4^iterations.
 */
static GeneratedPolygon make_koch_snowflake(const int iterations, const double radius = 100.0)
{
  BLI_assert(iterations >= 0);

  /* Base equilateral triangle (iteration 0) oriented CCW. */
  Vector<double2> current_pts;
  current_pts.append(double2(0.0, radius));
  current_pts.append(
      double2(radius * std::cos(7.0 * M_PI / 6.0), radius * std::sin(7.0 * M_PI / 6.0)));
  current_pts.append(
      double2(radius * std::cos(11.0 * M_PI / 6.0), radius * std::sin(11.0 * M_PI / 6.0)));

  for ([[maybe_unused]] const int iter : IndexRange(iterations)) {
    Vector<double2> next_pts;
    const int m = int(current_pts.size());
    next_pts.reserve(4 * m);

    for (const int i : IndexRange(m)) {
      const double2 a = current_pts[i];
      const double2 b = current_pts[(i + 1) % m];
      const double2 v = b - a;

      const double2 p1 = a;
      const double2 p2 = a + (1.0 / 3.0) * v;
      /* Outward equilateral triangle apex on middle third. */
      const double2 p3 = a + 0.5 * v + (std::sqrt(3.0) / 6.0) * double2(v.y, -v.x);
      const double2 p4 = a + (2.0 / 3.0) * v;

      next_pts.append(p1);
      next_pts.append(p2);
      next_pts.append(p3);
      next_pts.append(p4);
    }
    current_pts = std::move(next_pts);
  }

  const int total_verts_num = int(current_pts.size());
  GeneratedPolygon geom;
  geom.vert.reinitialize(total_verts_num);
  geom.outer_polygon.reserve(total_verts_num);

  for (const int i : IndexRange(total_verts_num)) {
    geom.vert[i] = current_pts[i];
    geom.outer_polygon.append(i);
  }

  return geom;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Benchmark Row Runner
 * \{ */

static void run_benchmark_row(const char *name, const int n, const GeneratedPolygon &geom)
{
  char label_full[128];
  std::snprintf(label_full, sizeof(label_full), "%s n=%d (full)", name, n);
  StraightSkeletonInput input_full = geom.to_input();
  input_full.label = label_full;
  StraightSkeletonOutput output_full;

  /* Benchmark full straight skeleton computation. */
  const double time_full = run_iterations(
      [&]() { output_full = straight_skeleton_calc(input_full); });

  /* Benchmark banded offset computation (5 segments). */
  char label_segmented[128];
  std::snprintf(label_segmented, sizeof(label_segmented), "%s n=%d (5 segments)", name, n);
  StraightSkeletonInput input_segmented = geom.to_input();
  input_segmented.segments = 5;
  input_segmented.label = label_segmented;
  StraightSkeletonOutput output_segmented;
  const double time_segmented = run_iterations(
      [&]() { output_segmented = straight_skeleton_calc(input_segmented); });

  printf("| %3d | %8d |", n, int(geom.vert.size()));
  print_time(time_full);
  print_time(time_segmented);
  printf(" %6d | %5d | %5d |\n",
         output_full.events_processed_num,
         output_full.flips_num,
         int(output_full.face_offsets.size()));
  fflush(stdout);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Performance Tests
 * \{ */

TEST(straight_skeleton_performance, DISABLED_Circle)
{
  print_table_header("Straight Skeleton: Circle (n = vertices on perimeter)", "  n");
  const int n_values[] = {8, 16, 32, 64, 128, 256, 512};
  for (const int n : n_values) {
    const GeneratedPolygon geom = make_circle(n);
    run_benchmark_row("Circle", n, geom);
  }
  printf("\n");
}

TEST(straight_skeleton_performance, DISABLED_SubdividedSquare)
{
  print_table_header("Straight Skeleton: Subdivided Square (n = vertices per side)", "  n");
  const int n_values[] = {2, 4, 8, 16, 32, 64, 128, 256};
  for (const int n : n_values) {
    const GeneratedPolygon geom = make_subdivided_square(n);
    run_benchmark_row("Subdivided Square", n, geom);
  }
  printf("\n");
}

TEST(straight_skeleton_performance, DISABLED_CircleWithHoles)
{
  print_table_header("Straight Skeleton: Circle with Holes (n = outer vertices & hole count)",
                     "  n");
  const int n_values[] = {3, 6, 12, 24, 48, 96};
  for (const int n : n_values) {
    const GeneratedPolygon geom = make_circle_with_holes(n);
    run_benchmark_row("Circle with Holes", n, geom);
  }
  printf("\n");
}

TEST(straight_skeleton_performance, KochSnowflake)
{
#ifdef WITH_TRACY
  if (std::getenv("TRACY_WAIT") != nullptr) {
    printf("\nWaiting for Tracy profiler to connect on port 8086...\n");
    fflush(stdout);
    while (!TracyIsConnected) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("Connected to Tracy! Starting benchmark in 500ms...\n\n");
    fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
#endif

  print_table_header("Straight Skeleton: Koch Snowflake (n = refinement iterations)", "  n");
  const int n_values[] = {5};
  for (const int n : n_values) {
    const GeneratedPolygon geom = make_koch_snowflake(n);
    run_benchmark_row("Koch Snowflake", n, geom);
  }
  printf("\n");

#ifdef WITH_TRACY
  if (TracyIsConnected) {
    /* Allow network buffers to flush to profiler / capture server before exiting. */
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (std::getenv("TRACY_WAIT") != nullptr) {
      printf("\nBenchmark complete and profiled by Tracy! Waiting for disconnect or close...\n");
      fflush(stdout);
      while (TracyIsConnected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }
  }
#endif
}

/** \} */

}  // namespace blender::straightskeleton
