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
  const RealizationOptions &realization_options = this->get_input().get_realization_options();
  const Domain &input_domain = this->get_input().domain();
  const Domain &output_domain = target_domain_;

  /* Translate the input such that it is centered in the virtual compositing space. */
  float2 input_translation = float2(-float2(input_domain.data_size) / 2.0f);

  /* Add any corrective translation if necessary */
  if (realization_options.interpolation == Interpolation::Nearest) {
    /* Bias translations in case of nearest interpolation to avoids the round-to-even behavior of
     * some GPUs at pixel boundaries. */
    input_translation += float2(std::numeric_limits<float>::epsilon() * 10e3f);
  }
  else {
    /* Assuming no transformations, if the input size is odd and output size is even or vice versa,
     * the centers of pixels of the input and output will be half a pixel away from each other due
     * to the centering translation. Which introduce fuzzy result due to interpolation. So if one
     * is odd and the other is even, detected by testing the low bit of the xor of the sizes, shift
     * the input by 1/2 pixel so the pixels align. */
    const int2 output_size = output_domain.data_size;
    const int2 input_size = input_domain.data_size;
    if ((input_size.x ^ output_size.x) & 1) {
      input_translation.x -= 0.5f;
    }
    if ((input_size.y ^ output_size.y) & 1) {
      input_translation.y -= 0.5f;
    }
  }

  const float3x3 input_transformation = math::translate(input_domain.transformation,
                                                        input_translation);

  /* Translate the output such that it is centered in the virtual compositing space. */
  const float2 output_translation = -float2(output_domain.data_size) / 2.0f;
  const float3x3 output_transformation = math::translate(output_domain.transformation,
                                                         output_translation);

  /* Get the transformation from the output space to the input space */
  float3x3 transformation = math::invert(input_transformation) * output_transformation;

  /* Transform from pixel centers rather than pixel corners */
  transformation *= math::from_location<float3x3>(float2(0.5f));
  /* Transform to normalized coordinates */
  float2 scale = 1.0f / float2(input_domain.data_size);
  transformation = math::from_scale<float3x3>(scale) * transformation;

  this->get_result().allocate_texture(output_domain);

  if (this->context().use_gpu()) {
    this->realize_on_domain_gpu(realization_options, transformation);
  }
  else {
    this->realize_on_domain_cpu(realization_options, transformation);
  }
}

void RealizeOnDomainOperation::realize_on_domain_gpu(const RealizationOptions &realization_options,
                                                     const float3x3 &transformation)
{
  Result &input = this->get_input();

  const char *shader_name = nullptr;
  switch (input.type()) {
    case ResultType::Float:
      if (realization_options.interpolation == Interpolation::Bicubic)
        shader_name = "compositor_realize_on_domain_bicubic_float";
      else
        shader_name = "compositor_realize_on_domain_float";
      break;
    case ResultType::Float2:
      if (realization_options.interpolation == Interpolation::Bicubic)
        shader_name = "compositor_realize_on_domain_bicubic_float2";
      else
        shader_name = "compositor_realize_on_domain_float2";
      break;
    case ResultType::Float3:
      /* Float3 is internally stored in a float4 texture due to GPU module limitations. */
    case ResultType::Float4:
    case ResultType::Color:
    case ResultType::Quaternion:
      if (realization_options.interpolation == Interpolation::Bicubic)
        shader_name = "compositor_realize_on_domain_bicubic_float4";
      else if (realization_options.interpolation == Interpolation::Anisotropic)
        shader_name = "compositor_realize_on_domain_anisotropic_float4";
      else
        shader_name = "compositor_realize_on_domain_float4";
      break;
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
      shader_name = "compositor_realize_on_domain_bool";
      break;
    case ResultType::Menu:
      shader_name = "compositor_realize_on_domain_menu";
      break;
    case ResultType::Float4x4:
      shader_name = "compositor_realize_on_domain_float4x4";
      break;
    case ResultType::String:
    case ResultType::Object:
    case ResultType::Image:
    case ResultType::Font:
    case ResultType::Scene:
    case ResultType::Text:
    case ResultType::Mask:
      /* Single only types do not support GPU code path. */
      BLI_assert(Result::is_single_value_only_type(this->get_input().type()));
      BLI_assert_unreachable();
      break;
  }
  gpu::Shader *shader = this->context().get_shader(shader_name);
  GPU_shader_bind(shader);

  GPU_shader_uniform_mat3_as_mat4(shader, "transformation", transformation.ptr());

  if (!GPU_texture_has_integer_format(input)) {
    /* The texture sampler should use bilinear interpolation for both the bilinear and bicubic
     * cases, as the logic used by the bicubic realization shader expects textures to use bilinear
     * interpolation. */
    if (realization_options.interpolation == Interpolation::Anisotropic) {
      GPU_texture_anisotropic_filter(input, true);
      GPU_texture_mipmap_mode(input, true, true);
    }
    else {
      GPU_texture_filter_mode(input, realization_options.interpolation != Interpolation::Nearest);
    }
  }

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
                              const RealizationOptions &realization_options,
                              const float3x3 &transformation)
{
  const float2x2 jacobian(transformation);
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

void RealizeOnDomainOperation::realize_on_domain_cpu(const RealizationOptions &realization_options,
                                                     const float3x3 &transformation)
{
  Result &input = this->get_input();
  Result &output = this->get_result();
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
                      math::Quaternion>([&]<typename T>() {
        realize_on_domain<T>(input, output, realization_options, transformation);
      });
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
