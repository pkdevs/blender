/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/enum.h"
#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* Kernel Feature Flags
 *
 * These are set from the host side depending on which features are used by the scene,
 * either for runtime checks or specialization.
 *
 * Shader node features and other kernel features are separate 32 bit types. Mixing them in
 * bitwise operations is a compile error. On the host they are combined in #KernelFeatures. */

/* Bit positions, also used by the preprocessor for adaptive compilation. */
#define KERNEL_FEATURE_NODE_BIT_BSDF 0
#define KERNEL_FEATURE_NODE_BIT_EMISSION 1
#define KERNEL_FEATURE_NODE_BIT_VOLUME 2
#define KERNEL_FEATURE_NODE_BIT_BUMP 3
#define KERNEL_FEATURE_NODE_BIT_BUMP_STATE 4
#define KERNEL_FEATURE_NODE_BIT_VORONOI_EXTRA 5
#define KERNEL_FEATURE_NODE_BIT_RAYTRACE 6
#define KERNEL_FEATURE_NODE_BIT_AOV 7
#define KERNEL_FEATURE_NODE_BIT_LIGHT_PATH 8
#define KERNEL_FEATURE_NODE_BIT_PRINCIPLED_HAIR 9
#define KERNEL_FEATURE_NODE_BIT_PORTAL 10

#define KERNEL_FEATURE_BIT_PATH_TRACING 0
#define KERNEL_FEATURE_BIT_POINTCLOUD 1
#define KERNEL_FEATURE_BIT_HAIR_RIBBON 2
#define KERNEL_FEATURE_BIT_HAIR_THICK 3
#define KERNEL_FEATURE_BIT_OBJECT_MOTION 4
#define KERNEL_FEATURE_BIT_BAKING 5
#define KERNEL_FEATURE_BIT_SUBSURFACE 6
#define KERNEL_FEATURE_BIT_VOLUME 7
#define KERNEL_FEATURE_BIT_TRANSPARENT 8
#define KERNEL_FEATURE_BIT_SHADOW_CATCHER 9
#define KERNEL_FEATURE_BIT_LIGHT_PASSES 10
#define KERNEL_FEATURE_BIT_AO_PASS 11
#define KERNEL_FEATURE_BIT_AO_ADDITIVE 12
#define KERNEL_FEATURE_BIT_MNEE 13
#define KERNEL_FEATURE_BIT_PATH_GUIDING 14
#define KERNEL_FEATURE_BIT_OSL_SHADING 15
#define KERNEL_FEATURE_BIT_OSL_CAMERA 16
#define KERNEL_FEATURE_BIT_LIGHT_LINKING 17
#define KERNEL_FEATURE_BIT_SHADOW_LINKING 18
#define KERNEL_FEATURE_BIT_DENOISING 19
#define KERNEL_FEATURE_BIT_LIGHT_TREE 20
#define KERNEL_FEATURE_BIT_GSPLATS 21

/* Shader nodes, also used to specialize shader evaluation for kernels. */
enum KernelNodeFeature : uint32_t {
  KERNEL_FEATURE_NODE_NONE = 0U,
  KERNEL_FEATURE_NODE_BSDF = (1U << KERNEL_FEATURE_NODE_BIT_BSDF),
  KERNEL_FEATURE_NODE_EMISSION = (1U << KERNEL_FEATURE_NODE_BIT_EMISSION),
  KERNEL_FEATURE_NODE_VOLUME = (1U << KERNEL_FEATURE_NODE_BIT_VOLUME),
  KERNEL_FEATURE_NODE_BUMP = (1U << KERNEL_FEATURE_NODE_BIT_BUMP),
  KERNEL_FEATURE_NODE_BUMP_STATE = (1U << KERNEL_FEATURE_NODE_BIT_BUMP_STATE),
  KERNEL_FEATURE_NODE_VORONOI_EXTRA = (1U << KERNEL_FEATURE_NODE_BIT_VORONOI_EXTRA),
  KERNEL_FEATURE_NODE_RAYTRACE = (1U << KERNEL_FEATURE_NODE_BIT_RAYTRACE),
  KERNEL_FEATURE_NODE_AOV = (1U << KERNEL_FEATURE_NODE_BIT_AOV),
  KERNEL_FEATURE_NODE_LIGHT_PATH = (1U << KERNEL_FEATURE_NODE_BIT_LIGHT_PATH),
  KERNEL_FEATURE_NODE_PRINCIPLED_HAIR = (1U << KERNEL_FEATURE_NODE_BIT_PRINCIPLED_HAIR),
  KERNEL_FEATURE_NODE_PORTAL = (1U << KERNEL_FEATURE_NODE_BIT_PORTAL),

  /* Shader node feature masks, to specialize shader evaluation for kernels. */
  KERNEL_FEATURE_NODE_MASK_SURFACE_LIGHT = (KERNEL_FEATURE_NODE_EMISSION |
                                            KERNEL_FEATURE_NODE_VORONOI_EXTRA |
                                            KERNEL_FEATURE_NODE_LIGHT_PATH |
                                            KERNEL_FEATURE_NODE_PORTAL),
  KERNEL_FEATURE_NODE_MASK_SURFACE_BACKGROUND = (KERNEL_FEATURE_NODE_MASK_SURFACE_LIGHT |
                                                 KERNEL_FEATURE_NODE_AOV),
  KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW =
      (KERNEL_FEATURE_NODE_BSDF | KERNEL_FEATURE_NODE_EMISSION | KERNEL_FEATURE_NODE_BUMP |
       KERNEL_FEATURE_NODE_BUMP_STATE | KERNEL_FEATURE_NODE_VORONOI_EXTRA |
       KERNEL_FEATURE_NODE_LIGHT_PATH | KERNEL_FEATURE_NODE_PRINCIPLED_HAIR |
       KERNEL_FEATURE_NODE_PORTAL),
  KERNEL_FEATURE_NODE_MASK_SURFACE = (KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW |
                                      KERNEL_FEATURE_NODE_RAYTRACE | KERNEL_FEATURE_NODE_AOV |
                                      KERNEL_FEATURE_NODE_LIGHT_PATH),
  KERNEL_FEATURE_NODE_MASK_VOLUME = (KERNEL_FEATURE_NODE_EMISSION | KERNEL_FEATURE_NODE_VOLUME |
                                     KERNEL_FEATURE_NODE_VORONOI_EXTRA |
                                     KERNEL_FEATURE_NODE_LIGHT_PATH | KERNEL_FEATURE_NODE_PORTAL),
  KERNEL_FEATURE_NODE_MASK_DISPLACEMENT = (KERNEL_FEATURE_NODE_VORONOI_EXTRA |
                                           KERNEL_FEATURE_NODE_BUMP |
                                           KERNEL_FEATURE_NODE_BUMP_STATE |
                                           KERNEL_FEATURE_NODE_PORTAL),
  KERNEL_FEATURE_NODE_MASK_BUMP = KERNEL_FEATURE_NODE_MASK_DISPLACEMENT,
};

enum KernelFeature : uint32_t {
  KERNEL_FEATURE_NONE = 0U,

  /* Use path tracing kernels. */
  KERNEL_FEATURE_PATH_TRACING = (1U << KERNEL_FEATURE_BIT_PATH_TRACING),

  /* BVH/sampling kernel features. */
  KERNEL_FEATURE_POINTCLOUD = (1U << KERNEL_FEATURE_BIT_POINTCLOUD),
  KERNEL_FEATURE_HAIR_RIBBON = (1U << KERNEL_FEATURE_BIT_HAIR_RIBBON),
  KERNEL_FEATURE_HAIR_THICK = (1U << KERNEL_FEATURE_BIT_HAIR_THICK),
  KERNEL_FEATURE_HAIR = (KERNEL_FEATURE_HAIR_RIBBON | KERNEL_FEATURE_HAIR_THICK),
  KERNEL_FEATURE_OBJECT_MOTION = (1U << KERNEL_FEATURE_BIT_OBJECT_MOTION),

  /* Denotes whether baking functionality is needed. */
  KERNEL_FEATURE_BAKING = (1U << KERNEL_FEATURE_BIT_BAKING),

  /* Use subsurface scattering materials. */
  KERNEL_FEATURE_SUBSURFACE = (1U << KERNEL_FEATURE_BIT_SUBSURFACE),

  /* Use volume materials. */
  KERNEL_FEATURE_VOLUME = (1U << KERNEL_FEATURE_BIT_VOLUME),

  /* Use Transparent shadows */
  KERNEL_FEATURE_TRANSPARENT = (1U << KERNEL_FEATURE_BIT_TRANSPARENT),

  /* Use shadow catcher. */
  KERNEL_FEATURE_SHADOW_CATCHER = (1U << KERNEL_FEATURE_BIT_SHADOW_CATCHER),

  /* Light render passes. */
  KERNEL_FEATURE_LIGHT_PASSES = (1U << KERNEL_FEATURE_BIT_LIGHT_PASSES),

  /* AO. */
  KERNEL_FEATURE_AO_PASS = (1U << KERNEL_FEATURE_BIT_AO_PASS),
  KERNEL_FEATURE_AO_ADDITIVE = (1U << KERNEL_FEATURE_BIT_AO_ADDITIVE),
  KERNEL_FEATURE_AO = (KERNEL_FEATURE_AO_PASS | KERNEL_FEATURE_AO_ADDITIVE),

  /* MNEE. */
  KERNEL_FEATURE_MNEE = (1U << KERNEL_FEATURE_BIT_MNEE),

  /* Path guiding. */
  KERNEL_FEATURE_PATH_GUIDING = (1U << KERNEL_FEATURE_BIT_PATH_GUIDING),

  /* OSL. */
  KERNEL_FEATURE_OSL_SHADING = (1U << KERNEL_FEATURE_BIT_OSL_SHADING),
  KERNEL_FEATURE_OSL_CAMERA = (1U << KERNEL_FEATURE_BIT_OSL_CAMERA),

  /* Light and shadow linking. */
  KERNEL_FEATURE_LIGHT_LINKING = (1U << KERNEL_FEATURE_BIT_LIGHT_LINKING),
  KERNEL_FEATURE_SHADOW_LINKING = (1U << KERNEL_FEATURE_BIT_SHADOW_LINKING),

  /* Use denoising kernels and output denoising passes. */
  KERNEL_FEATURE_DENOISING = (1U << KERNEL_FEATURE_BIT_DENOISING),

  /* Light tree. */
  KERNEL_FEATURE_LIGHT_TREE = (1U << KERNEL_FEATURE_BIT_LIGHT_TREE),

  /* Gaussian splats. */
  KERNEL_FEATURE_GSPLATS = (1U << KERNEL_FEATURE_BIT_GSPLATS),
};

CCL_ENUM_OPERATORS(KernelNodeFeature, uint32_t)
CCL_ENUM_OPERATORS(KernelFeature, uint32_t)

#define IF_KERNEL_NODES_FEATURE(feature) \
  if constexpr ((node_feature_mask & (KERNEL_FEATURE_NODE_##feature)) != 0U)
#define IF_NOT_KERNEL_NODES_FEATURE(feature) \
  if constexpr ((node_feature_mask & (KERNEL_FEATURE_NODE_##feature)) == 0U)

/* Kernel Feature Guards
 *
 * These are used throughout the code to disable code for certain features entirely.
 * They can be commented out for faster builds, or to measure the performance impact
 * of the existence of the extra code in the kernel.
 *
 * Commenting out the first set of feature guards should work well to reduce kernel
 * compile time while still working for many scenes. */

#define __AO__
#define __LIGHT_LINKING__
#define __MNEE__
#define __OBJECT_MOTION__
#define __POINTCLOUD__
#define __PRINCIPLED_HAIR__
#define __SHADER_RAYTRACE__
#define __SHADOW_CATCHER__
#define __SHADOW_LINKING__
#define __GSPLATS__
#define __SUBSURFACE__
#define __TRANSPARENT_SHADOWS__
#define __VOLUME__

#define __CAUSTICS_TRICKS__
#define __CLAMP_SAMPLE__
#define __DENOISING_FEATURES__
#define __DPDU__
#define __HAIR__
#define __LIGHT_TREE__
#define __PASSES__
#define __RAY_DIFFERENTIALS__
#define __VISIBILITY_FLAG__
#define __SVM__
#define __SPECTRAL__

/* Device specific features */

#ifdef WITH_OSL
#  define __OSL__
#  ifdef __KERNEL_OPTIX__
/* Kernels with OSL support are built separately in OptiX and don't need SVM. */
#    undef __SVM__
#  endif
#endif
#ifndef __KERNEL_GPU__
#  if defined(WITH_PATH_GUIDING)
#    define __PATH_GUIDING__
#  endif
#  define __VOLUME_RECORD_ALL__
#endif /* !__KERNEL_GPU__ */

/* Scene-based adaptive features compilation.
 *
 * __KERNEL_FEATURES__ and __KERNEL_NODE_FEATURES__ are then defined as
 * integers to indicate the features used. */

#ifdef __KERNEL_FEATURES__
#  if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_OBJECT_MOTION))
#    undef __OBJECT_MOTION__
#  endif
#  if !(__KERNEL_FEATURES__ & \
        ((1U << KERNEL_FEATURE_BIT_HAIR_RIBBON) | (1U << KERNEL_FEATURE_BIT_HAIR_THICK)))
#    undef __HAIR__
#  endif
#  if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_POINTCLOUD))
#    undef __POINTCLOUD__
#  endif
#  if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_VOLUME))
#    undef __VOLUME__
#    if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_TRANSPARENT))
#      undef __TRANSPARENT_SHADOWS__
#    endif
#  endif
#  if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_SUBSURFACE))
#    undef __SUBSURFACE__
#  endif
#  if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_SHADOW_CATCHER))
#    undef __SHADOW_CATCHER__
#  endif
#  if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_DENOISING))
#    undef __DENOISING_FEATURES__
#  endif
#  if !(__KERNEL_FEATURES__ & \
        ((1U << KERNEL_FEATURE_BIT_AO_PASS) | (1U << KERNEL_FEATURE_BIT_AO_ADDITIVE)))
#    undef __AO__
#  endif
#  if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_MNEE))
#    undef __MNEE__
#  endif
#  if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_PATH_GUIDING))
#    undef __PATH_GUIDING__
#  endif
#  if !(__KERNEL_NODE_FEATURES__ & (1U << KERNEL_FEATURE_NODE_BIT_PRINCIPLED_HAIR))
#    undef __PRINCIPLED_HAIR__
#  endif
#  if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_LIGHT_LINKING))
#    undef __LIGHT_LINKING__
#  endif
#  if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_SHADOW_LINKING))
#    undef __SHADOW_LINKING__
#  endif
#  if !(__KERNEL_FEATURES__ & (1U << KERNEL_FEATURE_BIT_GSPLATS))
#    undef __GSPLATS__
#  endif
#endif

#ifdef WITH_CYCLES_DEBUG_NAN
#  define __KERNEL_DEBUG_NAN__
#endif

/* Features that enable others */

#if defined(__SUBSURFACE__) || defined(__SHADER_RAYTRACE__)
#  define __BVH_LOCAL__
#endif

#ifndef __KERNEL_GPU__

/* Kernel and shader node features combined, for host side code. */

struct KernelFeatures {
  KernelFeature kernel = KERNEL_FEATURE_NONE;
  KernelNodeFeature nodes = KERNEL_FEATURE_NODE_NONE;

  KernelFeatures() = default;
  KernelFeatures(const KernelFeature kernel) : kernel(kernel) {}
  KernelFeatures(const KernelNodeFeature nodes) : nodes(nodes) {}
  KernelFeatures(const KernelFeature kernel, const KernelNodeFeature nodes)
      : kernel(kernel), nodes(nodes)
  {
  }

  KernelFeature operator&(const KernelFeature mask) const
  {
    return kernel & mask;
  }
  KernelNodeFeature operator&(const KernelNodeFeature mask) const
  {
    return nodes & mask;
  }
  KernelFeatures operator&(const KernelFeatures &mask) const
  {
    return {kernel & mask.kernel, nodes & mask.nodes};
  }
  KernelFeatures operator|(const KernelFeatures &other) const
  {
    return {kernel | other.kernel, nodes | other.nodes};
  }
  KernelFeatures &operator|=(const KernelFeatures &other)
  {
    return *this = *this | other;
  }

  bool operator==(const KernelFeatures &other) const
  {
    return kernel == other.kernel && nodes == other.nodes;
  }
  bool operator!=(const KernelFeatures &other) const
  {
    return !(*this == other);
  }
  explicit operator bool() const
  {
    return kernel != KERNEL_FEATURE_NONE || nodes != KERNEL_FEATURE_NODE_NONE;
  }
};

/* Feature mask for integrator states, to allocate state memory only when
 * certain features are on or off. */

struct KernelFeatureRequest {
  KernelFeatures on;
  KernelFeatures off;

  explicit KernelFeatureRequest(const KernelFeatures &on) : on(on) {}
  KernelFeatureRequest(const KernelFeatures &on, const KernelFeatures &off) : on(on), off(off) {}

  bool test(const KernelFeatures &features) const
  {
    return (features & on) && !(features & off);
  }
};

#endif /* !__KERNEL_GPU__ */

CCL_NAMESPACE_END
