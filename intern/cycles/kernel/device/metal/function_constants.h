/* SPDX-FileCopyrightText: 2021-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

enum {
  Kernel_DummyConstant,
#define KERNEL_STRUCT_MEMBER(parent, type, name) KernelData_##parent##_##name,
#include "kernel/data_template.h"

  KernelData_integrator_features,
  KernelData_node_features,
};

#ifdef __KERNEL_METAL__
#  define KERNEL_STRUCT_MEMBER(parent, type, name) \
    constant type kernel_data_##parent##_##name \
        [[function_constant(KernelData_##parent##_##name)]];
#  include "kernel/data_template.h"

constant uint kernel_data_integrator_features_value
    [[function_constant(KernelData_integrator_features)]];
constant uint kernel_data_node_features_value [[function_constant(KernelData_node_features)]];
#  define kernel_data_integrator_features \
    KernelIntegratorFeatureMask(kernel_data_integrator_features_value)
#  define kernel_data_node_features KernelNodeFeatureMask(kernel_data_node_features_value)
#endif
