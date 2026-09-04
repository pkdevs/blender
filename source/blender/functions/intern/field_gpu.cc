/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fn
 */

#include "FN_field_evaluation.hh"
#include "FN_field_gpu.hh"

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "BLI_array.hh"
#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string_ref.hh"

#include "GPU_capabilities.hh"
#include "GPU_compute.hh"
#include "GPU_context.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_vertex_buffer.hh"

#include "intern/gpu_shader_create_info.hh"

namespace blender::fn {

namespace {

struct GPUFieldInstruction {
  uint32_t operation;
  uint32_t input_a;
  uint32_t input_b;
  uint32_t input_c;
};

enum class GPUFieldOperation : uint32_t {
  Input = 0,
  Constant = 1,
  Add = 2,
  Subtract = 3,
  Multiply = 4,
  Divide = 5,
  Cross = 6,
  Dot = 7,
  Distance = 8,
  Length = 9,
  Scale = 10,
  Normalize = 11,
  MultiplyAdd = 12,
};

enum class GPUFieldValueType : uint32_t {
  Float = 0,
  Float3 = 1,
};

constexpr int GPU_FIELD_MAX_INSTRUCTIONS = 128;
constexpr int GPU_FIELD_LOCAL_SIZE = 64;

static bool is_gpu_value_type(const CPPType &type)
{
  return type.is<float>() || type.is<float3>();
}

static std::optional<GPUFieldValueType> gpu_value_type_for_cpp_type(const CPPType &type)
{
  if (type.is<float>()) {
    return GPUFieldValueType::Float;
  }
  if (type.is<float3>()) {
    return GPUFieldValueType::Float3;
  }
  return std::nullopt;
}

static float4 constant_to_float4(const CPPType &type, const void *value)
{
  if (type.is<float>()) {
    return float4(*static_cast<const float *>(value), 0.0f, 0.0f, 0.0f);
  }
  BLI_assert(type.is<float3>());
  return float4(*static_cast<const float3 *>(value), 0.0f);
}

static std::optional<GPUFieldOperation> gpu_operation_for_function(
    const mf::MultiFunction &function, const CPPType &output_type)
{
  const StringRef name = function.name();
  if (output_type.is<float3>()) {
    if (name == "float3 + float3") {
      return GPUFieldOperation::Add;
    }
    if (name == "float3 - float3") {
      return GPUFieldOperation::Subtract;
    }
    if (name == "float3 * float3") {
      return GPUFieldOperation::Multiply;
    }
    if (name == "float3 / float3") {
      return GPUFieldOperation::Divide;
    }
    if (name == "cross_product(float3, float3)") {
      return GPUFieldOperation::Cross;
    }
    if (name == "float3 * float3 + float3") {
      return GPUFieldOperation::MultiplyAdd;
    }
    if (name == "float3 * float") {
      return GPUFieldOperation::Scale;
    }
    if (name == "normalize(float3)") {
      return GPUFieldOperation::Normalize;
    }
  }
  else if (output_type.is<float>()) {
    if (name == "dot_product(float3, float3)") {
      return GPUFieldOperation::Dot;
    }
    if (name == "distance(float3, float3)") {
      return GPUFieldOperation::Distance;
    }
    if (name == "length(float3)") {
      return GPUFieldOperation::Length;
    }
  }
  return std::nullopt;
}

struct GPUFieldIR {
  Vector<GPUFieldInstruction> instructions;
  Vector<GPUFieldValueType> value_types;
  Vector<float4> constants;
  Vector<const FieldInput *> inputs;
  enum class InputSource : uint8_t {
    CPU,
    GPU,
  };
  Vector<InputSource> input_sources;
  int result = -1;
};

static int gpu_field_component_count(const GPUFieldValueType type)
{
  return type == GPUFieldValueType::Float ? 1 : 3;
}

static int gpu_field_input_component_count(const GPUFieldIR &ir)
{
  int component_count = 0;
  for (const int input_index : ir.inputs.index_range()) {
    if (ir.input_sources[input_index] == GPUFieldIR::InputSource::GPU) {
      continue;
    }
    const FieldInput *input = ir.inputs[input_index];
    component_count += gpu_field_component_count(*gpu_value_type_for_cpp_type(input->cpp_type()));
  }
  return component_count;
}

static int gpu_field_input_component_offset(const GPUFieldIR &ir, const int input_index)
{
  int component_offset = 0;
  for (const int i : IndexRange(input_index)) {
    if (ir.input_sources[i] == GPUFieldIR::InputSource::GPU) {
      continue;
    }
    component_offset += gpu_field_component_count(
        *gpu_value_type_for_cpp_type(ir.inputs[i]->cpp_type()));
  }
  return component_offset;
}

class GPUFieldIRBuilder {
 private:
  GPUFieldIR ir_;
  FieldHashDeep deep_hashes_;
  Map<UniqueHash, int> value_by_hash_;

  std::optional<int> add_instruction(const GPUFieldInstruction instruction,
                                     const GPUFieldValueType value_type)
  {
    if (ir_.instructions.size() >= GPU_FIELD_MAX_INSTRUCTIONS) {
      return std::nullopt;
    }
    const int index = ir_.instructions.append_and_get_index(instruction);
    ir_.value_types.append(value_type);
    return index;
  }

  std::optional<int> add_field(const GFieldRef &field)
  {
    const UniqueHash hash = deep_hashes_.ensure(field);
    if (const int *existing = value_by_hash_.lookup_ptr(hash)) {
      return *existing;
    }

    std::optional<int> result;
    std::visit(
        [&](const auto &value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, GFieldRef::Value>) {
            if (!is_gpu_value_type(*value.type)) {
              return;
            }
            const int constant_index = ir_.constants.append_and_get_index(
                constant_to_float4(*value.type, value.value));
            result = this->add_instruction(
                {uint32_t(GPUFieldOperation::Constant), uint32_t(constant_index), 0, 0},
                *gpu_value_type_for_cpp_type(*value.type));
          }
          else if constexpr (std::is_same_v<T, GFieldRef::Input>) {
            if (!is_gpu_value_type(value.node->cpp_type())) {
              return;
            }
            int input_index = -1;
            for (const int i : ir_.inputs.index_range()) {
              if (ir_.inputs[i] == value.node) {
                input_index = i;
                break;
              }
            }
            if (input_index == -1) {
              input_index = ir_.inputs.append_and_get_index(value.node);
            }
            result = this->add_instruction(
                {uint32_t(GPUFieldOperation::Input), uint32_t(input_index), 0, 0},
                *gpu_value_type_for_cpp_type(value.node->cpp_type()));
          }
          else if constexpr (std::is_same_v<T, GFieldRef::MultiFn>) {
            if (value.output_i != 0) {
              return;
            }
            const FieldOperation &operation = *value.node;
            const mf::MultiFunction &function = operation.multi_function();
            const std::optional<GPUFieldOperation> gpu_operation = gpu_operation_for_function(
                function, field.cpp_type());
            if (!gpu_operation) {

              return;
            }

            const Span<GField> inputs = operation.inputs();
            const int expected_input_count = [&]() {
              switch (*gpu_operation) {
                case GPUFieldOperation::Normalize:
                case GPUFieldOperation::Length:
                  return 1;
                case GPUFieldOperation::Scale:
                case GPUFieldOperation::Add:
                case GPUFieldOperation::Subtract:
                case GPUFieldOperation::Multiply:
                case GPUFieldOperation::Divide:
                case GPUFieldOperation::Cross:
                case GPUFieldOperation::Dot:
                case GPUFieldOperation::Distance:
                  return 2;
                case GPUFieldOperation::MultiplyAdd:
                  return 3;
                case GPUFieldOperation::Input:
                case GPUFieldOperation::Constant:
                  BLI_assert_unreachable();
                  return 0;
              }
              BLI_assert_unreachable();
              return 0;
            }();
            if (inputs.size() != expected_input_count) {
              return;
            }

            Array<int, 3> input_values(expected_input_count);
            for (const int i : IndexRange(expected_input_count)) {
              const std::optional<int> input_value = this->add_field(inputs[i]);
              if (!input_value) {
                return;
              }
              input_values[i] = *input_value;
            }

            result = this->add_instruction(
                {uint32_t(*gpu_operation),
                 uint32_t(input_values[0]),
                 uint32_t(expected_input_count > 1 ? input_values[1] : 0),
                 uint32_t(expected_input_count > 2 ? input_values[2] : 0)},
                *gpu_value_type_for_cpp_type(field.cpp_type()));
          }
        },
        field.variant());

    if (result) {
      value_by_hash_.add_new(hash, *result);
    }
    return result;
  }

 public:
  std::optional<GPUFieldIR> build(const GField &field)
  {
    if (!is_gpu_value_type(field.cpp_type())) {
      return std::nullopt;
    }
    const std::optional<int> result = this->add_field(GFieldRef(field));
    if (!result) {
      return std::nullopt;
    }
    ir_.result = *result;
    ir_.input_sources = Vector<GPUFieldIR::InputSource>(ir_.inputs.size(),
                                                        GPUFieldIR::InputSource::CPU);
    return std::move(ir_);
  }
};

static void append_input_values(const GVArray &input,
                                const IndexMask &mask,
                                const int input_component_count,
                                const int input_component_offset,
                                MutableSpan<float> r_values)
{
  if (input.type().is<float>()) {
    const VArray<float> typed_input = input.typed<float>();
    mask.foreach_index([&](const int64_t index) {
      r_values[index * input_component_count + input_component_offset] = typed_input[index];
    });
    return;
  }

  BLI_assert(input.type().is<float3>());
  const VArray<float3> typed_input = input.typed<float3>();
  mask.foreach_index([&](const int64_t index) {
    const float3 value = typed_input[index];
    const int64_t offset = index * input_component_count + input_component_offset;
    r_values[offset] = value.x;
    r_values[offset + 1] = value.y;
    r_values[offset + 2] = value.z;
  });
}

static gpu::StorageBuf *create_storage_buffer(const size_t element_size,
                                              const size_t element_count)
{
  const size_t size = std::max(element_size * element_count, size_t(1));
  return GPU_storagebuf_create_ex(size, nullptr, GPU_USAGE_DYNAMIC, __func__);
}

/** Reusable buffers keyed by program and exact input/output sizes. */
struct GPUFieldBufferSet {
  /** Interleaved packed input components: [element][input component]. */
  Array<float> input_data;
  Array<float4> constants;
  gpu::VertBuf *input_buffer = nullptr;
  gpu::StorageBuf *constant_buffer = nullptr;
  gpu::StorageBuf *output_buffer = nullptr;
  uint64_t last_used = 0;

  GPUFieldBufferSet() = default;
  GPUFieldBufferSet(const GPUFieldBufferSet &) = delete;

  ~GPUFieldBufferSet()
  {
    if (input_buffer != nullptr) {
      GPU_vertbuf_discard(input_buffer);
    }
    GPU_storagebuf_free(constant_buffer);
    GPU_storagebuf_free(output_buffer);
  }

  bool initialize(const int input_component_count,
                  const int64_t element_count,
                  const int64_t output_count,
                  const int constant_count)
  {
    input_data.reinitialize(std::max<int64_t>(int64_t(input_component_count) * element_count, 1));
    constants.reinitialize(std::max(constant_count, 1));

    GPUVertFormat input_format = {};
    GPU_vertformat_attr_add(&input_format, "field_input", gpu::VertAttrType::SFLOAT_32);
    input_buffer = GPU_vertbuf_create_with_format_ex(input_format, GPU_USAGE_DYNAMIC);
    if (input_buffer != nullptr) {
      GPU_vertbuf_data_alloc(*input_buffer, uint(input_data.size()));
    }
    constant_buffer = create_storage_buffer(sizeof(float4), constants.size());
    /* Packed float storage permits readback directly into the destination float3 span. */
    output_buffer = create_storage_buffer(sizeof(float), size_t(output_count) * 3);

    return input_buffer != nullptr && constant_buffer != nullptr && output_buffer != nullptr;
  }
};

class GPUFieldContextRuntime {
 private:
  using BufferKey = std::pair<uint64_t, std::pair<int64_t, int64_t>>;
  static constexpr int max_buffer_sets_ = 4;

  Map<BufferKey, std::shared_ptr<GPUFieldBufferSet>> buffer_sets_;
  uint64_t use_counter_ = 0;

 public:
  GPUFieldContextRuntime() = default;
  GPUFieldContextRuntime(const GPUFieldContextRuntime &) = delete;

  GPUFieldBufferSet *ensure_buffers(const uint64_t program_key,
                                    const int64_t element_count,
                                    const int64_t output_count,
                                    const int input_component_count,
                                    const int constant_count,
                                    std::shared_ptr<GPUFieldBufferSet> *r_owner = nullptr)
  {
    const BufferKey key{program_key, {element_count, output_count}};
    if (std::shared_ptr<GPUFieldBufferSet> *existing = buffer_sets_.lookup_ptr(key)) {
      if (existing->use_count() == 1) {
        (*existing)->last_used = ++use_counter_;
        if (r_owner != nullptr) {
          *r_owner = *existing;
        }
        return existing->get();
      }
      /* Retained results must not be overwritten by another evaluation of the same program. */
      buffer_sets_.remove(key);
    }

    auto buffers = std::make_shared<GPUFieldBufferSet>();
    if (!buffers->initialize(input_component_count, element_count, output_count, constant_count)) {
      return nullptr;
    }
    buffers->last_used = ++use_counter_;
    GPUFieldBufferSet *result = buffers.get();
    if (r_owner != nullptr) {
      *r_owner = buffers;
    }
    buffer_sets_.add_new(key, std::move(buffers));

    while (buffer_sets_.size() > max_buffer_sets_) {
      const BufferKey *least_recent_key = nullptr;
      uint64_t least_recent_use = UINT64_MAX;
      for (const auto item : buffer_sets_.items()) {
        if (item.value->last_used < least_recent_use) {
          least_recent_use = item.value->last_used;
          least_recent_key = &item.key;
        }
      }
      BLI_assert(least_recent_key != nullptr);
      buffer_sets_.remove(*least_recent_key);
    }
    return result;
  }
};

static const char gpu_field_context_runtime_key = 0;

static void *gpu_field_context_runtime_create()
{
  return new GPUFieldContextRuntime();
}

static void gpu_field_context_runtime_free(void *data)
{
  delete static_cast<GPUFieldContextRuntime *>(data);
}

static GPUFieldContextRuntime &gpu_field_context_runtime()
{
  GPUContext *context = GPU_context_active_get();
  BLI_assert(context != nullptr);
  return *static_cast<GPUFieldContextRuntime *>(
      GPU_context_data_ensure(context,
                              &gpu_field_context_runtime_key,
                              gpu_field_context_runtime_create,
                              gpu_field_context_runtime_free));
}

static uint64_t gpu_field_program_cache_key(const GPUFieldIR &ir)
{
  /* Constants, input contents, and dispatch sizes are runtime data, excluded from the key. */
  constexpr uint32_t key_version = 5;
  uint64_t hash = get_default_hash(key_version,
                                   uint32_t(ir.result),
                                   uint32_t(ir.instructions.size()),
                                   uint32_t(ir.inputs.size()));
  hash = hash * 33 ^ get_default_hash(uint32_t(ir.constants.size()));
  for (const int instruction_index : ir.instructions.index_range()) {
    const GPUFieldInstruction &instruction = ir.instructions[instruction_index];
    hash = hash * 33 ^ get_default_hash(instruction.operation,
                                        instruction.input_a,
                                        instruction.input_b,
                                        instruction.input_c);
    hash = hash * 33 ^ get_default_hash(uint32_t(ir.value_types[instruction_index]));
  }
  for (const int input_index : ir.inputs.index_range()) {
    hash = hash * 33 ^ get_default_hash(uint32_t(ir.input_sources[input_index]));
  }
  return hash;
}

static std::string gpu_field_value_name(const int instruction_index)
{
  return "value_" + std::to_string(instruction_index);
}

static std::string gpu_field_generated_source(const GPUFieldIR &ir)
{
  std::string source = R"GLSL(
void main()
{
  const uint local_index = gl_GlobalInvocationID.x;
  if (local_index >= uint(dispatch_count)) {
    return;
  }

  const uint element_index = uint(element_offset) + local_index;
)GLSL";

  for (const int instruction_index : ir.instructions.index_range()) {
    source += "  float4 " + gpu_field_value_name(instruction_index) + ";\n";
  }

  for (const int instruction_index : ir.instructions.index_range()) {
    const GPUFieldInstruction &instruction = ir.instructions[instruction_index];
    const GPUFieldOperation operation = GPUFieldOperation(instruction.operation);
    const std::string result = gpu_field_value_name(instruction_index);
    const std::string input_a = gpu_field_value_name(instruction.input_a);
    const std::string input_b = gpu_field_value_name(instruction.input_b);
    const std::string input_c = gpu_field_value_name(instruction.input_c);

    switch (operation) {
      case GPUFieldOperation::Input: {
        const GPUFieldValueType input_type = *gpu_value_type_for_cpp_type(
            ir.inputs[instruction.input_a]->cpp_type());
        if (ir.input_sources[instruction.input_a] == GPUFieldIR::InputSource::GPU) {
          BLI_assert(input_type == GPUFieldValueType::Float3);
          const std::string input_index = "(element_index - uint(gpu_input_offset)) * 3u";
          source += "  " + result + " = float4(field_gpu_inputs[" + input_index +
                    "], field_gpu_inputs[" + input_index + " + 1u], field_gpu_inputs[" +
                    input_index + " + 2u], 0.0f);\n";
        }
        else {
          const int component_offset = gpu_field_input_component_offset(ir, instruction.input_a);
          const int component_count = gpu_field_input_component_count(ir);
          const std::string input_index = "element_index * " + std::to_string(component_count) +
                                          "u + " + std::to_string(component_offset) + "u";
          if (input_type == GPUFieldValueType::Float) {
            source += "  " + result + " = float4(field_inputs[" + input_index +
                      "], 0.0f, 0.0f, 0.0f);\n";
          }
          else {
            source += "  " + result + " = float4(field_inputs[" + input_index +
                      "], field_inputs[" + input_index + " + 1u], field_inputs[" + input_index +
                      " + 2u], 0.0f);\n";
          }
        }
        break;
      }
      case GPUFieldOperation::Constant:
        source += "  " + result + " = field_constants[" + std::to_string(instruction.input_a) +
                  "];\n";
        break;
      case GPUFieldOperation::Add:
        source += "  " + result + " = float4(" + input_a + ".xyz + " + input_b + ".xyz, 0.0f);\n";
        break;
      case GPUFieldOperation::Subtract:
        source += "  " + result + " = float4(" + input_a + ".xyz - " + input_b + ".xyz, 0.0f);\n";
        break;
      case GPUFieldOperation::Multiply:
        source += "  " + result + " = float4(" + input_a + ".xyz * " + input_b + ".xyz, 0.0f);\n";
        break;
      case GPUFieldOperation::Divide:
        source += "  " + result + " = float4(" + input_b + ".x != 0.0f ? " + input_a + ".x / " +
                  input_b + ".x : 0.0f, " + input_b + ".y != 0.0f ? " + input_a + ".y / " +
                  input_b + ".y : 0.0f, " + input_b + ".z != 0.0f ? " + input_a + ".z / " +
                  input_b + ".z : 0.0f, 0.0f);\n";
        break;
      case GPUFieldOperation::Cross:
        source += "  " + result + " = float4(cross(" + input_a + ".xyz, " + input_b +
                  ".xyz), 0.0f);\n";
        break;
      case GPUFieldOperation::Dot:
        source += "  " + result + " = float4(dot(" + input_a + ".xyz, " + input_b +
                  ".xyz), 0.0f, 0.0f, 0.0f);\n";
        break;
      case GPUFieldOperation::Distance:
        source += "  " + result + " = float4(distance(" + input_a + ".xyz, " + input_b +
                  ".xyz), 0.0f, 0.0f, 0.0f);\n";
        break;
      case GPUFieldOperation::Length:
        source += "  " + result + " = float4(length(" + input_a + ".xyz), 0.0f, 0.0f, 0.0f);\n";
        break;
      case GPUFieldOperation::Scale:
        source += "  " + result + " = float4(" + input_a + ".xyz * " + input_b + ".x, 0.0f);\n";
        break;
      case GPUFieldOperation::Normalize:
        source += "  { const float3 value = " + input_a +
                  ".xyz; const float length_squared = dot(value, value); " + result +
                  " = length_squared > 1.0e-35f ? float4(value / sqrt(length_squared), 0.0f) : " +
                  "float4(0.0f); }\n";
        break;
      case GPUFieldOperation::MultiplyAdd:
        source += "  " + result + " = float4(" + input_a + ".xyz * " + input_b + ".xyz + " +
                  input_c + ".xyz, 0.0f);\n";
        break;
    }
  }

  source += "  const uint result_index = (element_index - uint(result_offset)) * 3u;\n";
  source += "  const float3 result_value = " + gpu_field_value_name(ir.result) + ".xyz;\n";
  source += "  field_results[result_index] = result_value.x;\n";
  source += "  field_results[result_index + 1u] = result_value.y;\n";
  source += "  field_results[result_index + 2u] = result_value.z;\n}\n";
  return source;
}

}  // namespace

struct GPUFieldResult {
  std::string name;
  GField field;
  GPUFieldContextKey context_key;
  int64_t element_count = 0;
  IndexRange range;
  GPUContext *gpu_context = nullptr;
  std::shared_ptr<GPUFieldBufferSet> buffers;

  explicit GPUFieldResult(const GField &field) : field(field) {}
};

struct GPUFieldChainScope::Impl {
  mutable std::mutex mutex;
  Vector<std::shared_ptr<GPUFieldResult>> results;

  void add_result(std::string name,
                  const GField &field,
                  const GPUFieldContextKey &context_key,
                  const int64_t element_count,
                  const IndexRange range,
                  GPUContext *gpu_context,
                  std::shared_ptr<GPUFieldBufferSet> buffers)
  {
    std::lock_guard lock(mutex);
    auto result = std::make_shared<GPUFieldResult>(field);
    result->name = std::move(name);
    result->context_key = context_key;
    result->element_count = element_count;
    result->range = range;
    result->gpu_context = gpu_context;
    result->buffers = std::move(buffers);
    results.append(std::move(result));
  }

  std::shared_ptr<GPUFieldResult> lookup(const StringRef name,
                                         const GPUFieldContextKey &context_key,
                                         const int64_t element_count,
                                         const IndexRange range,
                                         GPUContext *gpu_context) const
  {
    std::lock_guard lock(mutex);
    for (const std::shared_ptr<GPUFieldResult> &result : results) {
      if (result->name == name && result->context_key == context_key &&
          result->element_count == element_count && result->range.start() == range.start() &&
          result->range.size() == range.size() &&
          (gpu_context == nullptr || result->gpu_context == gpu_context))
      {
        return result;
      }
    }
    return nullptr;
  }
};

GPUFieldChainScope::GPUFieldChainScope() : impl_(std::make_unique<Impl>()) {}

GPUFieldChainScope::~GPUFieldChainScope() = default;

GVArray GPUFieldChainScope::get_varray_for_input(const FieldInput &input,
                                                 const GPUFieldContextKey &context_key,
                                                 const IndexMask &mask,
                                                 const FieldContext &fallback_context) const
{
  GPUContext *context = GPU_context_active_get();
  if (!input.cpp_type().is<float3>() || !mask.to_range()) {
    return {};
  }
  const std::shared_ptr<GPUFieldResult> result = impl_->lookup(
      input.debug_name(), context_key, mask.min_array_size(), *mask.to_range(), nullptr);
  if (result == nullptr) {
    return {};
  }
  Array<float3> values(mask.min_array_size());
  if (context == result->gpu_context) {
    GPU_storagebuf_read(result->buffers->output_buffer, values.data() + result->range.start());
  }
  else {
    /* A consumer on another thread cannot read this context's GPU resources. */
    FieldEvaluator evaluator(fallback_context, &mask);
    evaluator.add_with_destination(result->field, values.as_mutable_span());
    evaluator.evaluate();
  }
  return VArray<float3>::from_container(std::move(values));
}

bool try_evaluate_field_gpu_impl(const GField &field,
                                 const FieldContext &context,
                                 const IndexMask &mask,
                                 const GMutableSpan dst,
                                 const GPUFieldContextKey *context_key,
                                 GPUFieldChainScope *chain_scope,
                                 const StringRef transient_result_name)
{

  const bool write_to_cpu = transient_result_name.is_empty();
  if (GPU_context_active_get() == nullptr || mask.is_empty() ||
      (!write_to_cpu && (context_key == nullptr || chain_scope == nullptr)))
  {
    return false;
  }
  if (field.cpp_type() != CPPType::get<float3>() ||
      (write_to_cpu &&
       (dst.type_ptr() != &CPPType::get<float3>() || dst.size() < mask.min_array_size())))
  {
    return false;
  }
  if (!mask.to_range()) {
    /* Sparse masks use the CPU evaluator. */
    return false;
  }

  GPUFieldIRBuilder ir_builder;
  std::optional<GPUFieldIR> ir = ir_builder.build(field);
  if (!ir) {
    return false;
  }

  GPUContext *active_gpu_context = GPU_context_active_get();
  const IndexRange range = *mask.to_range();
  int gpu_input_count = 0;
  Array<std::shared_ptr<GPUFieldResult>> gpu_input_results(ir->inputs.size());
  if (chain_scope != nullptr && context_key != nullptr) {
    for (const int input_index : ir->inputs.index_range()) {
      const FieldInput *input = ir->inputs[input_index];
      const std::shared_ptr<GPUFieldResult> result = chain_scope->impl_->lookup(
          input->debug_name(), *context_key, mask.min_array_size(), range, active_gpu_context);
      if (result == nullptr) {
        continue;
      }
      if (input->cpp_type() != CPPType::get<float3>()) {
        return false;
      }
      ir->input_sources[input_index] = GPUFieldIR::InputSource::GPU;
      gpu_input_results[input_index] = result;
      gpu_input_count++;
    }
    if (gpu_input_count > 1) {
      return false;
    }
  }
  const int input_component_count = gpu_field_input_component_count(*ir);

  const uint64_t program_key = gpu_field_program_cache_key(*ir);

  ResourceScope scope;
  Array<GVArray> input_varrays(ir->inputs.size());
  for (const int input_index : ir->inputs.index_range()) {
    if (ir->input_sources[input_index] == GPUFieldIR::InputSource::GPU) {
      continue;
    }
    const FieldInput *input = ir->inputs[input_index];
    GVArray varray = context.get_varray_for_input(*input, mask, scope);
    if (!varray || !is_gpu_value_type(varray.type()) || varray.size() < mask.min_array_size()) {
      return false;
    }
    input_varrays[input_index] = std::move(varray);
  }

  const int64_t array_size = mask.min_array_size();
  GPUFieldContextRuntime &context_runtime = gpu_field_context_runtime();
  std::shared_ptr<GPUFieldBufferSet> buffer_owner;
  GPUFieldBufferSet *buffers = context_runtime.ensure_buffers(program_key,
                                                              array_size,
                                                              mask.size(),
                                                              input_component_count,
                                                              ir->constants.size(),
                                                              &buffer_owner);

  if (buffers == nullptr) {
    return false;
  }

  for (const int input_index : ir->inputs.index_range()) {
    if (ir->input_sources[input_index] == GPUFieldIR::InputSource::GPU) {
      continue;
    }
    append_input_values(input_varrays[input_index],
                        mask,
                        input_component_count,
                        gpu_field_input_component_offset(*ir, input_index),
                        buffers->input_data.as_mutable_span());
  }

  buffers->constants.as_mutable_span().take_front(ir->constants.size()).copy_from(ir->constants);

  std::shared_ptr<GPUFieldResult> gpu_input_result;
  for (const std::shared_ptr<GPUFieldResult> &result : gpu_input_results) {
    if (result != nullptr) {
      gpu_input_result = result;
      break;
    }
  }

  gpu::Shader *shader = GPU_shader_generated_cache_lookup(program_key);
  if (shader == nullptr) {
    std::string source = gpu_field_generated_source(*ir);

    using namespace gpu::shader;
    const std::string shader_name = "gpu_field_compiled_" + std::to_string(program_key);
    ShaderCreateInfo create_info(shader_name.c_str());
    create_info.local_group_size(GPU_FIELD_LOCAL_SIZE, 1, 1);
    create_info.storage_buf(0, Qualifier::read, "float", "field_inputs[]");
    create_info.storage_buf(1, Qualifier::read, "float4", "field_constants[]");
    create_info.storage_buf(2, Qualifier::write, "float", "field_results[]");
    if (gpu_input_result != nullptr) {
      create_info.storage_buf(3, Qualifier::read, "float", "field_gpu_inputs[]");
    }
    create_info.push_constant(Type::int_t, "element_offset");
    create_info.push_constant(Type::int_t, "dispatch_count");
    create_info.push_constant(Type::int_t, "result_offset");
    create_info.push_constant(Type::int_t, "gpu_input_offset");
    create_info.compute_source_generated = std::move(source);

    shader = GPU_shader_create_from_info_generated_cached(
        program_key, reinterpret_cast<GPUShaderCreateInfo *>(&create_info));
  }
  if (shader == nullptr) {
    return false;
  }

  if (input_component_count > 0) {
    GPU_vertbuf_update_sub(buffers->input_buffer,
                           0,
                           uint(buffers->input_data.size() * sizeof(float)),
                           buffers->input_data.data());
  }

  GPU_storagebuf_update(buffers->constant_buffer, buffers->constants.data());

  GPU_shader_bind(shader);
  GPU_vertbuf_bind_as_ssbo(buffers->input_buffer,
                           GPU_shader_get_ssbo_binding(shader, "field_inputs"));
  GPU_storagebuf_bind(buffers->constant_buffer,
                      GPU_shader_get_ssbo_binding(shader, "field_constants"));
  GPU_storagebuf_bind(buffers->output_buffer,
                      GPU_shader_get_ssbo_binding(shader, "field_results"));
  GPU_shader_uniform_1i(shader, "result_offset", int(range.start()));
  if (gpu_input_result != nullptr) {
    GPU_storagebuf_bind(gpu_input_result->buffers->output_buffer,
                        GPU_shader_get_ssbo_binding(shader, "field_gpu_inputs"));
    GPU_shader_uniform_1i(shader, "gpu_input_offset", int(gpu_input_result->range.start()));
  }

  const int max_groups = std::max(GPU_max_work_group_count(0), 1);
  for (int64_t offset = range.start(); offset < range.one_after_last();) {
    const int64_t remaining = range.one_after_last() - offset;
    const int64_t max_elements = int64_t(max_groups) * GPU_FIELD_LOCAL_SIZE;
    const int64_t dispatch_elements = std::min(remaining, max_elements);
    const uint groups = uint((dispatch_elements + GPU_FIELD_LOCAL_SIZE - 1) /
                             GPU_FIELD_LOCAL_SIZE);

    GPU_shader_uniform_1i(shader, "element_offset", int(offset));
    GPU_shader_uniform_1i(shader, "dispatch_count", int(dispatch_elements));

    GPU_compute_dispatch(shader, groups, 1, 1);
    offset += dispatch_elements;
  }

  GPU_shader_unbind();

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_BUFFER_UPDATE);

  if (write_to_cpu) {
    const MutableSpan<float3> typed_dst = dst.typed<float3>();
    GPU_storagebuf_read(buffers->output_buffer, typed_dst.data() + range.start());
  }

  GPU_storagebuf_unbind(buffers->constant_buffer);
  GPU_storagebuf_unbind(buffers->output_buffer);
  if (gpu_input_result != nullptr) {
    GPU_storagebuf_unbind(gpu_input_result->buffers->output_buffer);
  }

  if (!write_to_cpu) {
    chain_scope->impl_->add_result(std::string(transient_result_name),
                                   field,
                                   *context_key,
                                   mask.min_array_size(),
                                   range,
                                   active_gpu_context,
                                   std::move(buffer_owner));
  }

  return true;
}

bool try_evaluate_field_gpu(const GField &field,
                            const FieldContext &context,
                            const IndexMask &mask,
                            const GMutableSpan dst,
                            const GPUFieldContextKey *context_key,
                            GPUFieldChainScope *chain_scope)
{
  return try_evaluate_field_gpu_impl(field, context, mask, dst, context_key, chain_scope, {});
}

bool try_evaluate_field_gpu_to_transient_result(const GField &field,
                                                const FieldContext &context,
                                                const IndexMask &mask,
                                                const GPUFieldContextKey &context_key,
                                                const StringRef result_name,
                                                GPUFieldChainScope &chain_scope)
{
  return try_evaluate_field_gpu_impl(
      field, context, mask, {}, &context_key, &chain_scope, result_name);
}

}  // namespace blender::fn
