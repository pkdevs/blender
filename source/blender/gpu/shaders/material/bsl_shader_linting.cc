/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Compile shader files as C++ inside one compilation unit to lint syntax and get IDE integration.
 */

#include "gpu_shader_material_align_rotation_to_vector.bsl.hh" /* IWYU pragma: export */
#include "gpu_shader_material_axes_to_rotation.bsl.hh"         /* IWYU pragma: export */
#include "gpu_shader_material_axis_angle_to_rotation.bsl.hh"   /* IWYU pragma: export */
#include "gpu_shader_material_bevel.bsl.hh"                    /* IWYU pragma: export */
#include "gpu_shader_material_bit_math.bsl.hh"                 /* IWYU pragma: export */
#include "gpu_shader_material_blackbody.bsl.hh"                /* IWYU pragma: export */
#include "gpu_shader_material_boolean_math.bsl.hh"             /* IWYU pragma: export */
#include "gpu_shader_material_bright_contrast.bsl.hh"          /* IWYU pragma: export */
#include "gpu_shader_material_clamp.bsl.hh"                    /* IWYU pragma: export */
#include "gpu_shader_material_combine_color.bsl.hh"            /* IWYU pragma: export */
#include "gpu_shader_material_combine_transform.bsl.hh"        /* IWYU pragma: export */
#include "gpu_shader_material_combine_xyz.bsl.hh"              /* IWYU pragma: export */
#include "gpu_shader_material_compare.bsl.hh"                  /* IWYU pragma: export */
#include "gpu_shader_material_euler_to_rotation.bsl.hh"        /* IWYU pragma: export */
#include "gpu_shader_material_float_to_int.bsl.hh"             /* IWYU pragma: export */
#include "gpu_shader_material_fractal_noise.bsl.hh"            /* IWYU pragma: export */
#include "gpu_shader_material_fractal_voronoi.bsl.hh"          /* IWYU pragma: export */
#include "gpu_shader_material_gamma.bsl.hh"                    /* IWYU pragma: export */
#include "gpu_shader_material_get_vector_component.bsl.hh"     /* IWYU pragma: export */
#include "gpu_shader_material_hash_value.bsl.hh"               /* IWYU pragma: export */
#include "gpu_shader_material_hue_sat_val.bsl.hh"              /* IWYU pragma: export */
#include "gpu_shader_material_integer_math.bsl.hh"             /* IWYU pragma: export */
#include "gpu_shader_material_invert.bsl.hh"                   /* IWYU pragma: export */
#include "gpu_shader_material_invert_rotation.bsl.hh"          /* IWYU pragma: export */
#include "gpu_shader_material_light_iter_internal.bsl.hh"      /* IWYU pragma: export */
#include "gpu_shader_material_map_range.bsl.hh"                /* IWYU pragma: export */
#include "gpu_shader_material_mapping.bsl.hh"                  /* IWYU pragma: export */
#include "gpu_shader_material_matrix_svd.bsl.hh"               /* IWYU pragma: export */
#include "gpu_shader_material_mix_color.bsl.hh"                /* IWYU pragma: export */
#include "gpu_shader_material_noise.bsl.hh"                    /* IWYU pragma: export */
#include "gpu_shader_material_normal.bsl.hh"                   /* IWYU pragma: export */
#include "gpu_shader_material_particle_info.bsl.hh"            /* IWYU pragma: export */
#include "gpu_shader_material_quaternion_to_rotation.bsl.hh"   /* IWYU pragma: export */
/* Generates warning. */
// #include "gpu_shader_material_radial_tiling.bsl.hh"       /* IWYU pragma: export */
#include "gpu_shader_material_random_value.bsl.hh"           /* IWYU pragma: export */
#include "gpu_shader_material_rgb_to_bw.bsl.hh"              /* IWYU pragma: export */
#include "gpu_shader_material_rotate_rotation.bsl.hh"        /* IWYU pragma: export */
#include "gpu_shader_material_rotate_vector.bsl.hh"          /* IWYU pragma: export */
#include "gpu_shader_material_rotation_to_axis_angle.bsl.hh" /* IWYU pragma: export */
#include "gpu_shader_material_rotation_to_euler.bsl.hh"      /* IWYU pragma: export */
#include "gpu_shader_material_rotation_to_quaternion.bsl.hh" /* IWYU pragma: export */
#include "gpu_shader_material_separate_color.bsl.hh"         /* IWYU pragma: export */
#include "gpu_shader_material_separate_transform.bsl.hh"     /* IWYU pragma: export */
#include "gpu_shader_material_separate_xyz.bsl.hh"           /* IWYU pragma: export */
#include "gpu_shader_material_set.bsl.hh"                    /* IWYU pragma: export */
#include "gpu_shader_material_squeeze.bsl.hh"                /* IWYU pragma: export */
#include "gpu_shader_material_tex_brick.bsl.hh"              /* IWYU pragma: export */
#include "gpu_shader_material_tex_checker.bsl.hh"            /* IWYU pragma: export */
#include "gpu_shader_material_tex_environment.bsl.hh"        /* IWYU pragma: export */
#include "gpu_shader_material_tex_gabor.bsl.hh"              /* IWYU pragma: export */
#include "gpu_shader_material_tex_gradient.bsl.hh"           /* IWYU pragma: export */
#include "gpu_shader_material_tex_magic.bsl.hh"              /* IWYU pragma: export */
#include "gpu_shader_material_tex_sky.bsl.hh"                /* IWYU pragma: export */
#include "gpu_shader_material_tex_voronoi.bsl.hh"            /* IWYU pragma: export */
#include "gpu_shader_material_tex_wave.bsl.hh"               /* IWYU pragma: export */
#include "gpu_shader_material_tex_white_noise.bsl.hh"        /* IWYU pragma: export */
#include "gpu_shader_material_vector_math.bsl.hh"            /* IWYU pragma: export */
#include "gpu_shader_material_vector_rotate.bsl.hh"          /* IWYU pragma: export */
#include "gpu_shader_material_voronoi.bsl.hh"                /* IWYU pragma: export */
#include "gpu_shader_material_wavelength.bsl.hh"             /* IWYU pragma: export */

void main() {}
