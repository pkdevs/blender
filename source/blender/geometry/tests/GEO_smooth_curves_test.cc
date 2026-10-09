/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <tbb/task_arena.h>

#include "BKE_gtest_base.hh"
#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_virtual_array.hh"
#include "GEO_smooth_curves.hh"
#include "testing/testing.h"

namespace blender::geometry::tests {

class SmoothCurvesTest : public bke::BlenderGTestBase {};

/* 1. Single point domain should pass through unmodified. */
TEST_F(SmoothCurvesTest, gaussian_blur_1D_single_point)
{
  Array<float3> src = {float3(1.0f, 2.0f, 3.0f)};
  Array<float3> dst = src;
  auto influence = VArray<float>::from_single(1.0f, 1);

  gaussian_blur_1D(src.as_span(), 5, influence, false, false, false, dst.as_mutable_span());

  EXPECT_EQ(dst[0], src[0]);
}

/* 2. Uniform values should remain identical after blurring. */
TEST_F(SmoothCurvesTest, gaussian_blur_1D_uniform_values)
{
  const int64_t num_points = 100;
  const float3 initial_val(4.0f, -2.0f, 10.0f);
  Array<float3> src(num_points, initial_val);
  Array<float3> dst = src;
  auto influence = VArray<float>::from_single(1.0f, num_points);

  gaussian_blur_1D(src.as_span(), 10, influence, true, false, false, dst.as_mutable_span());

  for (int64_t i = 0; i < num_points; ++i) {
    EXPECT_NEAR(dst[i].x, initial_val.x, 1e-5f);
    EXPECT_NEAR(dst[i].y, initial_val.y, 1e-5f);
    EXPECT_NEAR(dst[i].z, initial_val.z, 1e-5f);
  }
}

/* 3. Non-smoothed ends must preserve original endpoint coordinates. */
TEST_F(SmoothCurvesTest, gaussian_blur_1D_fixed_endpoints)
{
  const int64_t num_points = 50;
  Array<float3> src(num_points);
  for (int64_t i = 0; i < num_points; ++i) {
    src[i] = float3(i, i * 2.0f, 0.0f);
  }
  Array<float3> dst = src;
  auto influence = VArray<float>::from_single(1.0f, num_points);

  /* smooth_ends = false, is_cyclic = false */
  gaussian_blur_1D(src.as_span(), 5, influence, false, false, false, dst.as_mutable_span());

  EXPECT_EQ(dst[0], src[0]);
  EXPECT_EQ(dst[num_points - 1], src[num_points - 1]);
}

/* 4. Cyclic domain symmetry test. */
TEST_F(SmoothCurvesTest, gaussian_blur_1D_cyclic)
{
  const int64_t num_points = 10;
  Array<float3> src(num_points, float3(0.0f));
  src[0] = float3(10.0f, 0.0f, 0.0f); /* Pulse at index 0 */

  Array<float3> dst = src;
  auto influence = VArray<float>::from_single(1.0f, num_points);

  /* is_cyclic = true */
  gaussian_blur_1D(src.as_span(), 2, influence, false, false, true, dst.as_mutable_span());

  /* Index 1 and Index 9 (last_pt) should receive identical symmetrical weights */
  EXPECT_NEAR(dst[1].x, dst[num_points - 1].x, 1e-5f);
}

/* Heavy workload benchmark (High Point Counts up to 20M & Iterations with Memory Metrics). */
TEST_F(SmoothCurvesTest, benchmark_large_workloads)
{
  struct BenchmarkConfig {
    int64_t num_points;
    int iterations;
    int repetitions;
    const char *label;
  };

  const BenchmarkConfig configs[] = {
      {1000000, 20, 3, "1M pts, 20 iters"},
      {1000000, 50, 3, "1M pts, 50 iters"},
      {5000000, 20, 2, "5M pts, 20 iters"},
      {5000000, 50, 2, "5M pts, 50 iters"},
      {10000000, 20, 1, "10M pts, 20 iters"},
      {10000000, 50, 1, "10M pts, 50 iters"},
      {20000000, 20, 1, "20M pts, 20 iters"},
      {20000000, 50, 1, "20M pts, 50 iters"},
      {20000000, 100, 1, "20M pts, 100 iters"},
      {40000000, 20, 1, "40M pts, 20 iters"},
      {40000000, 50, 1, "40M pts, 50 iters"},
      {40000000, 100, 1, "40M pts, 100 iters"},
  };

  const int default_threads = tbb::this_task_arena::max_concurrency();

  std::cout << "\n======================================================================================================\n";
  std::cout << "               GAUSSIAN BLUR 1D - LARGE WORKLOAD BENCHMARK (Multithreaded: " << default_threads << " T)\n";
  std::cout << "======================================================================================================\n";
  std::cout << std::left
            << std::setw(24) << "Workload"
            << std::setw(14) << "Memory (MB)"
            << std::setw(22) << "Time Taken"
            << std::setw(16) << "Throughput"
            << std::setw(16) << "Est. Bandwidth"
            << "\n------------------------------------------------------------------------------------------------------\n";

  for (const auto &cfg : configs) {
    Array<float3> src(cfg.num_points);
    for (int64_t i = 0; i < cfg.num_points; ++i) {
      src[i] = float3(std::sin(float(i) * 0.01f), std::cos(float(i) * 0.01f), float(i));
    }
    Array<float3> dst = src;
    auto influence = VArray<float>::from_single(1.0f, cfg.num_points);

    /* Memory footprint: src (float3), dst (float3), total_weight (double) */
    const double memory_mb = double(cfg.num_points * (sizeof(float3) * 2 + sizeof(double))) /
                             (1024.0 * 1024.0);

    /* Warm-up */
    gaussian_blur_1D(src.as_span(), cfg.iterations, influence, false, false, false, dst.as_mutable_span());

    const auto start = std::chrono::high_resolution_clock::now();
    for (int rep = 0; rep < cfg.repetitions; ++rep) {
      gaussian_blur_1D(
          src.as_span(), cfg.iterations, influence, false, false, false, dst.as_mutable_span());
    }
    const auto end = std::chrono::high_resolution_clock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(end - start).count();
    const double avg_ms = total_ms / cfg.repetitions;
    const double mpts_per_sec = (double(cfg.num_points) / (avg_ms * 1e-3)) / 1e6;

    /* Est bandwidth: (3 reads + 1 write float3 + 2 double access) * N * I */
    const double bytes_per_call = double(cfg.num_points) * double(cfg.iterations) *
                                  (sizeof(float3) * 4 + sizeof(double) * 2);
    const double gb_per_sec = (bytes_per_call / (avg_ms * 1e-3)) / (1024.0 * 1024.0 * 1024.0);

    std::ostringstream mem_ss, time_ss, tp_ss, bw_ss;
    mem_ss << std::fixed << std::setprecision(1) << memory_mb << " MB";

    if (avg_ms >= 1000.0) {
      time_ss << std::fixed << std::setprecision(2) << (avg_ms / 1000.0) << " s ("
              << std::fixed << std::setprecision(1) << avg_ms << " ms)";
    }
    else {
      time_ss << std::fixed << std::setprecision(2) << avg_ms << " ms";
    }

    tp_ss << std::fixed << std::setprecision(2) << mpts_per_sec << " M pts/s";
    bw_ss << std::fixed << std::setprecision(2) << gb_per_sec << " GB/s";

    std::cout << std::left
              << std::setw(24) << cfg.label
              << std::setw(14) << mem_ss.str()
              << std::setw(22) << time_ss.str()
              << std::setw(16) << tp_ss.str()
              << std::setw(16) << bw_ss.str()
              << "\n";
  }
  std::cout << "======================================================================================================\n\n";
}

/* Multi-threading Scaling Benchmark (1, 2, 4, 8, 16, 32, and Auto threads). */
TEST_F(SmoothCurvesTest, benchmark_thread_scaling)
{
  const int64_t num_points = 2000000; /* 2 Million points */
  const int iterations = 20;
  const int repetitions = 2;

  Array<float3> src(num_points);
  for (int64_t i = 0; i < num_points; ++i) {
    src[i] = float3(std::sin(float(i) * 0.01f), std::cos(float(i) * 0.01f), float(i));
  }
  Array<float3> dst = src;
  auto influence = VArray<float>::from_single(1.0f, num_points);

  struct ThreadTestConfig {
    int max_concurrency;
    std::string label;
  };

  const int auto_threads = tbb::this_task_arena::max_concurrency();

  const ThreadTestConfig configs[] = {
      {1, "1 T"},
      {2, "2 T"},
      {4, "4 T"},
      {8, "8 T"},
      {16, "16 T"},
      {32, "32 T"},
      {tbb::task_arena::automatic, "Auto (" + std::to_string(auto_threads) + " T)"},
  };

  double single_thread_time_ms = 0.0;

  std::cout << "\n========================================================================================\n";
  std::cout << "               THREAD SCALING BENCHMARK (2M points, 20 iterations)               \n";
  std::cout << "========================================================================================\n";
  std::cout << std::left
            << std::setw(18) << "Thread Setting"
            << std::setw(22) << "Time Taken"
            << std::setw(12) << "Speedup"
            << std::setw(14) << "Efficiency"
            << std::setw(16) << "Throughput"
            << "\n----------------------------------------------------------------------------------------\n";

  for (const auto &cfg : configs) {
    tbb::task_arena arena(cfg.max_concurrency);

    /* Warm-up in arena */
    arena.execute([&]() {
      gaussian_blur_1D(
          src.as_span(), iterations, influence, false, false, false, dst.as_mutable_span());
    });

    const auto start = std::chrono::high_resolution_clock::now();
    for (int rep = 0; rep < repetitions; ++rep) {
      arena.execute([&]() {
        gaussian_blur_1D(
            src.as_span(), iterations, influence, false, false, false, dst.as_mutable_span());
      });
    }
    const auto end = std::chrono::high_resolution_clock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(end - start).count();
    const double avg_ms = total_ms / repetitions;

    if (cfg.max_concurrency == 1) {
      single_thread_time_ms = avg_ms;
    }

    const int effective_threads = (cfg.max_concurrency == tbb::task_arena::automatic) ?
                                      auto_threads :
                                      cfg.max_concurrency;

    const double speedup = single_thread_time_ms / avg_ms;
    const double efficiency = (speedup / double(effective_threads)) * 100.0;
    const double mpts_per_sec = (double(num_points) / (avg_ms * 1e-3)) / 1e6;

    std::ostringstream time_ss, speedup_ss, eff_ss, tp_ss;
    if (avg_ms >= 1000.0) {
      time_ss << std::fixed << std::setprecision(2) << (avg_ms / 1000.0) << " s ("
              << std::fixed << std::setprecision(1) << avg_ms << " ms)";
    }
    else {
      time_ss << std::fixed << std::setprecision(2) << avg_ms << " ms";
    }

    speedup_ss << std::fixed << std::setprecision(2) << speedup << "x";
    eff_ss << std::fixed << std::setprecision(1) << efficiency << "%";
    tp_ss << std::fixed << std::setprecision(2) << mpts_per_sec << " M pts/s";

    std::cout << std::left
              << std::setw(18) << cfg.label
              << std::setw(22) << time_ss.str()
              << std::setw(12) << speedup_ss.str()
              << std::setw(14) << eff_ss.str()
              << std::setw(16) << tp_ss.str()
              << "\n";
  }
  std::cout << "========================================================================================\n\n";
}

}  // namespace blender::geometry::tests

