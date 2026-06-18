/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

// "kernel/integrator/restir.h"

#pragma once

#include "kernel/integrator/state.h"
#include "kernel/light/common.h" // LightSample
#include "kernel/integrator/state_flow.h" // integrator_path_next

#include "kernel/sample/pattern.h"

CCL_NAMESPACE_BEGIN

struct ReSTIRReservoir {
    float W;
    float w_sum;

    uint M;

    uint candidate_type;

    float3 radiance;

    LightSample light;

    float target_pdf;

    uint rng_seed;
};


/**
 1. Camera starts the path and sets the next kernel to RESTIR_TEST.
 2. The Scheduler sees paths are queued for RESTIR_TEST and calls the kernel.
 3. The Kernel sets the next kernel to INTERSECT_CLOSEST.
 4. The Scheduler sees paths are now queued for INTERSECT_CLOSEST and the regular Cycles logic takes over.
 */
ccl_device void integrator_restir_test(KernelGlobals kg,
                                       IntegratorState state,
                                       ccl_global float *ccl_restrict render_buffer)
{
  // Simply transition the path to the next stage
  integrator_path_next(state, 
                       DEVICE_KERNEL_INTEGRATOR_RESTIR_TEST, 
                       DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
}

CCL_NAMESPACE_END