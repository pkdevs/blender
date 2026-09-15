/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_JOLT

/* Has to be included before the other Jolt headers.*/
#  include <Jolt/Jolt.h>

#  include <Jolt/Core/Factory.h>
#  include <Jolt/RegisterTypes.h>

#  include "MEM_guardedalloc.h"

#  include "GEO_jolt.hh"

namespace blender::geometry::jolt {

struct JoltStartupAndExit {
  JoltStartupAndExit()
  {
    initialize_jolt_allocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
  }

  static void initialize_jolt_allocator()
  {
    constexpr const char *func = __func__;
    JPH::Allocate = [](size_t size) {
      /* Jolt requires 16-byte alignment when doing a normal allocation. */
      return MEM_new_uninitialized_aligned(size, 16, func);
    };
    JPH::Reallocate = [](void *mem, size_t /*old_size*/, size_t new_size) {
      if (mem == nullptr) {
        return JPH::Allocate(new_size);
      }
      return MEM_realloc_uninitialized_id(mem, new_size, func);
    };
    JPH::Free = [](void *mem) { MEM_delete_void(mem); };
    JPH::AlignedAllocate = [](size_t size, size_t alignment) {
      return MEM_new_uninitialized_aligned(size, alignment, func);
    };
    JPH::AlignedFree = [](void *mem) { MEM_delete_void(mem); };
  }

  ~JoltStartupAndExit()
  {
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
  }
};

void ensure_initialization()
{
  static JoltStartupAndExit jolt_startup_and_exit;
}

}  // namespace blender::geometry::jolt

#else

namespace blender::geometry::jolt {

void ensure_initialization() {}

}  // namespace blender::geometry::jolt

#endif
