/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <limits>

#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

#include "GPU_shader.hh"
#include "GPU_texture.hh"

#include "COM_context.hh"
#include "COM_domain.hh"
#include "COM_input_descriptor.hh"
#include "COM_result.hh"
#include "COM_utilities.hh"

#include "COM_realize_on_domain_operation.hh"

namespace blender::compositor {

/* ------------------------------------------------------------------------------------------------
 * Realize On Domain Operation
 */

RealizeOnDomainOperation::RealizeOnDomainOperation(Context &context,
                                                   Domain target_domain,
                                                   ResultType type)
    : SimpleOperation(context), target_domain_(target_domain)
{
  InputDescriptor input_descriptor;
  input_descriptor.type = type;
  this->declare_input_descriptor(input_descriptor);
  this->populate_result(type);
}

void RealizeOnDomainOperation::execute()
{
  const Domain &input_domain = this->get_input().domain();
  const Domain &output_domain = target_domain_;

  /* Create a transformation matrix that transforms the pixels in the data window from the data
   * space to the virtual compositing space. This is done by first adding the data offset to go
   * from the data space to the display space, then subtracting the center of the display window to
   * go from the display space to the virtual compositing space. See the corrective translation
   * function for more information on its function. */
  const float2 input_center = float2(input_domain.display_size) / 2.0f;
  const float2 input_translation = float2(input_domain.data_offset) - input_center +
                                   this->compute_corrective_translation();
  const float3x3 input_data_to_virtual = math::translate(input_domain.transformation,
                                                         input_translation);

  /* Same as above but for the output domain. */
  const float2 output_center = float2(output_domain.display_size) / 2.0f;
  const float2 output_translation = float2(output_domain.data_offset) - output_center;
  const float3x3 output_data_to_virtual = math::translate(output_domain.transformation,
                                                          output_translation);

  /* Create a transformation matrix from the output data space to the input data space */
  const float3x3 virtual_to_input_data = math::invert(input_data_to_virtual);
  const float3x3 output_data_to_input_data = virtual_to_input_data * output_data_to_virtual;

  this->get_result().allocate_texture(output_domain);

  if (this->context().use_gpu()) {
    this->realize_on_domain_gpu(output_data_to_input_data);
  }
  else {
    this->realize_on_domain_cpu(output_data_to_input_data);
  }
}

float2 RealizeOnDomainOperation::compute_corrective_translation()
{
  if (this->get_input().get_realization_options().interpolation == Interpolation::Nearest) {
    /* Bias translations in case of nearest interpolation to avoids the round-to-even behavior of
     * some GPUs at pixel boundaries. */
    return float2(std::numeric_limits<float>::epsilon() * 10e3f);
  }

  /* Assuming no transformations, if the input size is odd and output size is even or vice versa,
   * the centers of pixels of the input and output will be half a pixel away from each other due
   * to the centering translation. Which introduce fuzzy result due to interpolation. So if one
   * is odd and the other is even, detected by testing the low bit of the xor of the sizes, shift
   * the input by 1/2 pixel so the pixels align. */
  const int2 output_size = this->compute_domain().data_size;
  const int2 input_size = this->get_input().domain().data_size;
  return float2(((input_size[0] ^ output_size[0]) & 1) ? -0.5f : 0.0f,
                ((input_size[1] ^ output_size[1]) & 1) ? -0.5f : 0.0f);
}

std::optional<float2x2> RealizeOnDomainOperation::compute_jacobian(const float3x3 &transformation)
{
  /* The Jacobian of a 2D transformation matrix is the matrix itself. */
  const float2x2 jacobian = float2x2(transformation);

  /* The gradients of the input across the output are the rows of the Jacobian. */
  const float2 x_gradient = float2(jacobian[0][0], jacobian[1][0]);
  const float2 y_gradient = float2(jacobian[0][1], jacobian[1][1]);

  /* Compute the size of the input pixels in normalized space. */
  const float2 pixel_size = math::rcp(float2(this->get_input().domain().data_size));

  /* If both gradients are less than that the input pixel size with a tolerance ratio of 1.1, then
   * each output pixel does not cover multiple input pixels and we can do simple point sampling,
   * otherwise, each output pixel covers multiple input pixels and we do area sampling. */
  constexpr float tolerance_ratio = 1.1f;
  if (math::length(x_gradient) < pixel_size.x * tolerance_ratio &&
      math::length(y_gradient) < pixel_size.y * tolerance_ratio)
  {
    return std::nullopt;
  }

  return jacobian;
}

void RealizeOnDomainOperation::realize_on_domain_gpu(const float3x3 &itransform)
{
  Result &input = this->get_input();
  const RealizationOptions &realization_options = input.get_realization_options();

  const float2 wh = math::fast_hypot(float2x2(itransform));

  const char *shader_name;
  bool use_bilinear = false;
  bool use_texels = false;
  if (input.is_interpolated()) {
    switch (realization_options.interpolation) {
      case Interpolation::Nearest:
        shader_name = "compositor_realize_on_domain_float4";
        break;
      case Interpolation::Bicubic:
        shader_name = "compositor_realize_on_domain_bspline_float4";
        use_bilinear = use_texels = true;
        break;
      default: /* Interpolation::Box */
        use_bilinear = true;
        if (wh.x < 1.1f && wh.y < 1.1f) {
          shader_name = "compositor_realize_on_domain_float4";
        }
        else {
          shader_name = "compositor_realize_on_domain_box_float4";
          use_texels = true;
        }
        break;
    }
  }
  else {
    switch (input.type()) {
      case ResultType::Int:
        shader_name = "compositor_realize_on_domain_int";
        break;
      case ResultType::Int2:
        shader_name = "compositor_realize_on_domain_int2";
        break;
      case ResultType::Int3:
        /* Int3 is internally stored in a int4 texture due to GPU module limitations. */
      case ResultType::Int4:
        shader_name = "compositor_realize_on_domain_int4";
        break;
      case ResultType::Bool:
      case ResultType::Menu:
        shader_name = "compositor_realize_on_domain_sint8";
        break;
      case ResultType::Float4x4:
        shader_name = "compositor_realize_on_domain_float4x4";
        break;
      default:
        shader_name = 0; /* fix compiler warning */
        /* Single only types do not support GPU code path. */
        BLI_assert(Result::is_single_value_only_type(this->get_input().type()));
        BLI_assert_unreachable();
        break;
    }
  }
  gpu::Shader *shader = this->context().get_shader(shader_name);
  GPU_shader_bind(shader);

  float3x3 transformation;
  if (use_texels) {
    /* Transform to texel centers */
    transformation = math::from_location<float3x3>(float2(-0.5f)) * itransform;
  }
  else {
    /* Transform to normalized coordinates */
    transformation = math::from_scale<float3x3>(1.0f / float2(input.domain().data_size)) *
                     itransform;
  }

  /* Transform from texel centers rather than corners */
  transformation = math::translate(transformation, float2(0.5f));

  GPU_shader_uniform_mat3_as_mat4(shader, "transformation", transformation.ptr());

  GPU_shader_uniform_2fv(shader, "wh", wh);

  GPU_texture_filter_mode(input, use_bilinear);

  GPU_texture_extend_mode_x(input,
                            map_extension_mode_to_extend_mode(realization_options.extension_x));
  GPU_texture_extend_mode_y(input,
                            map_extension_mode_to_extend_mode(realization_options.extension_y));

  input.bind_as_texture(shader, "input_tx");

  Result &output = this->get_result();
  output.bind_as_image(shader, "domain_img");

  compute_dispatch_threads_at_least(shader, output.domain().data_size);

  input.unbind_as_texture();
  output.unbind_as_image();
  GPU_shader_unbind();
}

template<typename T>
static void realize_on_domain(const Result &input,
                              Result &output,
                              const float3x3 &transformation,
                              const std::optional<float2x2> &jacobian)
{
  const RealizationOptions &realization_options = input.get_realization_options();
  parallel_for(output.domain().data_size, [&](const int2 texel) {
    const float2 coordinates = math::transform_point(transformation, float2(texel));
    T sample = input.sample<T>(coordinates,
                               realization_options.interpolation,
                               realization_options.extension_x,
                               realization_options.extension_y,
                               jacobian);
    output.store_pixel(texel, sample);
  });
}

void RealizeOnDomainOperation::realize_on_domain_cpu(const float3x3 &itransform)
{
  Result &input = this->get_input();
  Result &output = this->get_result();

  /* Transform to normalized coordinates */
  float3x3 transformation = math::from_scale<float3x3>(1.0f / float2(input.domain().data_size)) *
                            itransform;

  /* Transform from texel centers rather than corners */
  transformation = math::translate(transformation, float2(0.5f));

  const std::optional<float2x2> jacobian = this->compute_jacobian(transformation);

  input.get_cpp_type()
      .to_static_type<float,
                      float2,
                      float3,
                      float4,
                      Color,
                      int32_t,
                      int2,
                      int3,
                      int4,
                      bool,
                      float4x4,
                      nodes::MenuValue,
                      math::Quaternion>(
          [&]<typename T>() { realize_on_domain<T>(input, output, transformation, jacobian); });
}

Domain RealizeOnDomainOperation::compute_domain()
{
  return target_domain_;
}

SimpleOperation *RealizeOnDomainOperation::construct_if_needed(
    Context &context,
    const Result &input_result,
    const InputDescriptor &input_descriptor,
    const Domain &operation_domain)
{
  /* This input doesn't need realization, the operation is not needed. */
  if (input_descriptor.realization_mode == InputRealizationMode::None) {
    return nullptr;
  }

  /* The input expects a single value and if no single value is provided, it will be ignored and a
   * default value will be used, so no need to realize it and the operation is not needed. */
  if (input_descriptor.expects_single_value) {
    return nullptr;
  }

  /* Input result is a single value and does not need realization, the operation is not needed. */
  if (input_result.is_single_value()) {
    return nullptr;
  }

  /* If we are realizing on the operation domain, then our target domain is the operation domain,
   * otherwise, we are only realizing the transforms, then our target domain is the input's one. */
  const bool use_operation_domain = input_descriptor.realization_mode ==
                                    InputRealizationMode::OperationDomain;
  const Domain target_domain = use_operation_domain ? operation_domain : input_result.domain();

  const bool should_realize_translation = input_descriptor.realization_mode ==
                                          InputRealizationMode::Transforms;
  const Domain realized_target_domain = target_domain.realize_transformation(
      should_realize_translation);

  /* The input have an almost identical domain to the realized target domain, so no need to realize
   * it and the operation is not needed. */
  if (Domain::is_equal(input_result.domain(), realized_target_domain)) {
    return nullptr;
  }

  return new RealizeOnDomainOperation(context, realized_target_domain, input_descriptor.type);
}

}  // namespace blender::compositor
