/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "guardedalloc_test_base.h"

namespace {

/** Allocations per thread in each run. */
constexpr int ALLOCS_NUM = 10'000'000;
/** Runs to take the fastest of, to reduce noise. */
constexpr int RUNS_NUM = 5;

/** Prevents the compiler from optimizing allocations away. */
thread_local void *volatile mem_sink;

/** Allocate and immediately free, so the allocator reuses the same block every time. */
void alloc_free_immediate(const int allocs_num)
{
  for (int i = 0; i < allocs_num; i++) {
    void *mem = MEM_new_uninitialized(32, __func__);
    mem_sink = mem;
    MEM_delete_void(mem);
  }
}

/** Allocate a batch of blocks and then free them all. */
void alloc_free_batch(const int allocs_num)
{
  constexpr int batch_size = 1000;
  std::vector<void *> mems(batch_size);
  for (int i = 0; i < allocs_num; i += batch_size) {
    for (void *&mem : mems) {
      mem = MEM_new_uninitialized(32, __func__);
    }
    for (void *mem : mems) {
      MEM_delete_void(mem);
    }
  }
}

/** Run the function on the given number of threads and return the best time per allocation. */
double best_ns_per_alloc(void (*fn)(int), const int threads_num)
{
  double best = std::numeric_limits<double>::max();
  for (int run = 0; run < RUNS_NUM; run++) {
    const auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < threads_num; i++) {
      threads.emplace_back(fn, ALLOCS_NUM);
    }
    for (std::thread &thread : threads) {
      thread.join();
    }

    const std::chrono::duration<double, std::nano> duration = std::chrono::steady_clock::now() -
                                                              start;
    best = std::min(best, duration.count() / ALLOCS_NUM);
  }
  return best;
}

void print_table(const char *title, void (*fn)(int))
{
  const int max_threads = int(std::max(1u, std::thread::hardware_concurrency()));
  const int threads_nums[] = {1, 4, max_threads};

  printf("\n#### %s\n\n(time per alloc + free, per thread)\n\n", title);
  printf("| threads | time |\n|---:|---:|\n");
  for (const int threads_num : threads_nums) {
    printf("| %d | %6.2f ns |\n", threads_num, best_ns_per_alloc(fn, threads_num));
  }
}

}  // namespace

TEST_F(LockFreeAllocatorTest, AllocFreeImmediate)
{
  print_table("Alloc and free immediately", alloc_free_immediate);
}

TEST_F(LockFreeAllocatorTest, AllocFreeBatch)
{
  print_table("Alloc batch then free batch", alloc_free_batch);
}
