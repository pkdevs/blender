/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup fn
 *
 * This file contains several utilities to create multi-functions with less redundant code.
 */

#include "BLI_virtual_array_batch_processing.hh"

#include "FN_multi_function.hh"

namespace blender {

namespace fn::multi_function::build {

/**
 * These presets determine what code is generated for a #CustomMF. Different presets make different
 * trade-offs between run-time performance and compile-time/binary size.
 */
namespace exec_presets {

/** Method to execute a function in case devirtualization was not possible. */
enum class FallbackMode {
  /** Access all elements in virtual arrays through virtual function calls. */
  Simple,
  /** Process elements in chunks to reduce virtual function call overhead. */
  Materialized,
};

/**
 * The "naive" method for executing a #CustomMF. Every element is processed separately and input
 * values are retrieved from the virtual arrays one by one. This generates the least amount of
 * code, but is also the slowest method.
 */
struct Simple {
  static constexpr bool use_devirtualization = false;
  static constexpr FallbackMode fallback_mode = FallbackMode::Simple;
};

/**
 * This is an improvement over the #Simple method. It still generates a relatively small amount of
 * code, because the function is only instantiated once. It's generally faster than #Simple,
 * because inputs are retrieved from the virtual arrays in chunks, reducing virtual method call
 * overhead.
 */
struct Materialized {
  static constexpr bool use_devirtualization = false;
  static constexpr FallbackMode fallback_mode = FallbackMode::Materialized;
};

/**
 * The most efficient preset, but also potentially generates a lot of code (exponential in the
 * number of inputs of the function). It generates separate optimized loops for all combinations of
 * inputs. This should be used for small functions of which all inputs are likely to be single
 * values or spans, and the number of inputs is relatively small.
 */
struct AllSpanOrSingle {
  static constexpr bool use_devirtualization = true;
  static constexpr FallbackMode fallback_mode = FallbackMode::Materialized;

  template<typename... ParamTags, typename... LoadedParams, size_t... I>
  auto create_devirtualizers(TypeSequence<ParamTags...> /*param_tags*/,
                             std::index_sequence<I...> /*indices*/,
                             const std::tuple<LoadedParams...> &loaded_params) const
  {
    return std::make_tuple([&]() {
      using ParamTag = ParamTags;
      using T = typename ParamTag::base_type;
      if constexpr (ParamTag::category == ParamCategory::SingleInput) {
        const GVArrayImpl &varray_impl = *std::get<I>(loaded_params);
        return GVArrayDevirtualizer<T, true, true>{varray_impl};
      }
      else if constexpr (ELEM(ParamTag::category,
                              ParamCategory::SingleOutput,
                              ParamCategory::SingleMutable))
      {
        T *ptr = std::get<I>(loaded_params);
        return BasicDevirtualizer<T *>{ptr};
      }
    }()...);
  }
};

/**
 * A slightly weaker variant of #AllSpanOrSingle. It generates less code, because it assumes that
 * some of the inputs are most likely single values. It should be used for small functions which
 * have too many inputs to make #AllSingleOrSpan a reasonable choice.
 */
template<size_t... Indices> struct SomeSpanOrSingle {
  static constexpr bool use_devirtualization = true;
  static constexpr FallbackMode fallback_mode = FallbackMode::Materialized;

  template<typename... ParamTags, typename... LoadedParams, size_t... I>
  auto create_devirtualizers(TypeSequence<ParamTags...> /*param_tags*/,
                             std::index_sequence<I...> /*indices*/,
                             const std::tuple<LoadedParams...> &loaded_params) const
  {
    return std::make_tuple([&]() {
      using ParamTag = ParamTags;
      using T = typename ParamTag::base_type;

      if constexpr (ParamTag::category == ParamCategory::SingleInput) {
        constexpr bool UseSpan = ValueSequence<size_t, Indices...>::template contains<I>();
        const GVArrayImpl &varray_impl = *std::get<I>(loaded_params);
        return GVArrayDevirtualizer<T, true, UseSpan>{varray_impl};
      }
      else if constexpr (ELEM(ParamTag::category,
                              ParamCategory::SingleOutput,
                              ParamCategory::SingleMutable))
      {
        T *ptr = std::get<I>(loaded_params);
        return BasicDevirtualizer<T *>{ptr};
      }
    }()...);
  }
};

}  // namespace exec_presets

namespace detail {

template<typename Arg> inline decltype(auto) execute_array_fetch(Arg &&arg, const int64_t i)
{
  using A = std::decay_t<Arg>;
  if constexpr (std::is_same_v<A, Span<bool>>) {
    /* Casting bools to chars for loading allows GCC to auto-vectorize. */
    return bool(reinterpret_cast<const char *>(arg.data())[i]);
  }
  else {
    return std::forward<Arg>(arg)[i];
  }
}

/**
 * Executes #element_fn for all indices in the mask. The passed in #args contain the input as well
 * as output parameters. Usually types in #args are devirtualized (e.g. a `Span<int>` is passed in
 * instead of a `VArray<int>`).
 */
template<typename MaskT, typename... Args, typename ElementFn>
/* Perform additional optimizations on this loop because it is a very hot loop. For example, the
 * math node in geometry nodes is processed here. */
#if (defined(__GNUC__) && !defined(__clang__))
[[gnu::optimize("-funroll-loops")]] [[gnu::optimize("O3")]]
#endif
inline void execute_array(const ElementFn &element_fn,
                          MaskT mask,
                          /* Use restrict to tell the compiler that pointer inputs do not alias
                           * each other. This is important for some compiler optimizations. */
                          Args &&__restrict... args)
{
  if constexpr (std::is_same_v<std::decay_t<MaskT>, IndexRange>) {
    /* Having this explicit loop is necessary for MSVC to be able to vectorize this. */
    const int64_t start = mask.start();
    const int64_t end = mask.one_after_last();
    for (int64_t i = start; i < end; i++) {
      element_fn(execute_array_fetch(args, i)...);
    }
  }
  else {
    for (const int64_t i : mask) {
      element_fn(execute_array_fetch(args, i)...);
    }
  }
}

/* TODO: Comment about converting multi-function ParamTags to varray_batch ParamTags. */
template<typename SrcParamTag> struct BatchParamTagFromMF {
  static constexpr varray_batch::ParamCategory category = [] {
    if constexpr (SrcParamTag::category == ParamCategory::SingleInput) {
      return varray_batch::ParamCategory::Input;
    }
    else if constexpr (SrcParamTag::category == ParamCategory::SingleOutput) {
      return varray_batch::ParamCategory::Output;
    }
    else {
      static_assert(SrcParamTag::category == ParamCategory::SingleMutable);
      return varray_batch::ParamCategory::Mutable;
    }
  }();
  using type = varray_batch::ParamTag<category, typename SrcParamTag::base_type>;
};

template<typename ElementFn, typename ExecPreset, typename... ParamTags, size_t... I>
inline void execute_element_fn_as_multi_function(const ElementFn &element_fn,
                                                 const ExecPreset exec_preset,
                                                 const IndexMask &mask,
                                                 Params params,
                                                 TypeSequence<ParamTags...> /*param_tags*/,
                                                 std::index_sequence<I...> /*indices*/)
{

  /* Load parameters from #Params. */
  /* Contains `const GVArrayImpl *` for inputs and `T *` for outputs. */
  const auto loaded_params = std::make_tuple([&]() {
    /* Use `typedef` instead of `using` to work around a compiler bug. */
    using ParamTag = ParamTags;
    using T = typename ParamTag::base_type;

    if constexpr (ParamTag::category == ParamCategory::SingleInput) {
      return params.readonly_single_input(I).get_implementation();
    }
    else if constexpr (ParamTag::category == ParamCategory::SingleOutput) {
      return static_cast<T *>(params.uninitialized_single_output(I).data());
    }
    else if constexpr (ParamTag::category == ParamCategory::SingleMutable) {
      return static_cast<T *>(params.single_mutable(I).data());
    }
  }()...);

  /* Try execute devirtualized if enabled and the input types allow it. */
  bool executed_devirtualized = false;
  if constexpr (ExecPreset::use_devirtualization) {
    /* Get segments before devirtualization to avoid generating this code multiple times. */
    const Vector<std::variant<IndexRange, IndexMaskSegment>, 16> mask_segments =
        mask.to_spans_and_ranges<16>();

    const auto devirtualizers = exec_preset.create_devirtualizers(
        TypeSequence<ParamTags...>(), std::index_sequence<I...>(), loaded_params);
    executed_devirtualized = call_with_devirtualized_parameters(
        devirtualizers, [&](auto &&...args) {
          for (const std::variant<IndexRange, IndexMaskSegment> &segment : mask_segments) {
            if (std::holds_alternative<IndexRange>(segment)) {
              const auto segment_range = std::get<IndexRange>(segment);
              execute_array(element_fn, segment_range, std::forward<decltype(args)>(args)...);
            }
            else {
              const auto segment_indices = std::get<IndexMaskSegment>(segment);
              execute_array(element_fn, segment_indices, std::forward<decltype(args)>(args)...);
            }
          }
        });
  }
  else {
    UNUSED_VARS(exec_preset);
  }

  /* If devirtualized execution was disabled or not possible, use a fallback method which is
   * slower but always works. */
  if (!executed_devirtualized) {
    /* The materialized method is most common because it avoids most virtual function overhead but
     * still instantiates the function only once. */
    if constexpr (ExecPreset::fallback_mode == exec_presets::FallbackMode::Materialized) {
      mask.foreach_segment([&](const IndexMaskSegment segment) {
        varray_batch::batch_process<64>(
            TypeSequence<typename BatchParamTagFromMF<ParamTags>::type...>(),
            std::index_sequence<I...>(),
            element_fn,
            segment,
            loaded_params);
      });
    }
    else {
      /* This fallback is slower because it uses virtual method calls for every element. */
      mask.foreach_segment([&](const IndexMaskSegment segment) {
        execute_array(element_fn, segment, [&]() {
          /* Use `typedef` instead of `using` to work around a compiler bug. */
          using ParamTag = ParamTags;
          using T = typename ParamTag::base_type;
          if constexpr (ParamTag::category == ParamCategory::SingleInput) {
            const GVArrayImpl &varray_impl = *std::get<I>(loaded_params);
            return GVArray(&varray_impl).typed<T>();
          }
          else if constexpr (ELEM(ParamTag::category,
                                  ParamCategory::SingleOutput,
                                  ParamCategory::SingleMutable))
          {
            T *ptr = std::get<I>(loaded_params);
            return ptr;
          }
        }()...);
      });
    }
  }
}

/**
 * `element_fn` is expected to return nothing and to have the following parameters:
 * - For single-inputs: const value or reference.
 * - For single-mutables: non-const reference.
 * - For single-outputs: non-const pointer.
 */
template<typename ElementFn, typename ExecPreset, typename... ParamTags>
inline auto build_multi_function_call_from_element_fn(ElementFn &&element_fn,
                                                      const ExecPreset exec_preset,
                                                      TypeSequence<ParamTags...> /*param_tags*/)
{
  return [element_fn = std::forward<ElementFn>(element_fn), exec_preset](const IndexMask &mask,
                                                                         Params params) {
    execute_element_fn_as_multi_function(element_fn,
                                         exec_preset,
                                         mask,
                                         params,
                                         TypeSequence<ParamTags...>(),
                                         std::make_index_sequence<sizeof...(ParamTags)>());
  };
}

/**
 * A multi function that just invokes the provided function in its #call method.
 */
template<typename CallFn, typename... ParamTags> class CustomMF : public MultiFunction {
 private:
  Signature signature_;
  CallFn call_fn_;

 public:
  CustomMF(const char *name, CallFn call_fn, TypeSequence<ParamTags...> /*param_tags*/)
      : call_fn_(std::move(call_fn))
  {
    SignatureBuilder builder{name, signature_};
    /* Loop over all parameter types and add an entry for each in the signature. */
    ([&] { builder.add(ParamTags(), ""); }(), ...);
    this->set_signature(&signature_);
  }

  void call(const IndexMask &mask, Params params, Context /*context*/) const override
  {
    call_fn_(mask, params);
  }
};

template<typename Out, typename... In, typename ElementFn, typename ExecPreset>
inline auto build_multi_function_with_n_inputs_one_output(const char *name,
                                                          ElementFn &&element_fn,
                                                          const ExecPreset exec_preset,
                                                          TypeSequence<In...> /*in_types*/)
{
  constexpr auto param_tags = TypeSequence<ParamTag<ParamCategory::SingleInput, In>...,
                                           ParamTag<ParamCategory::SingleOutput, Out>>();
  auto call_fn = build_multi_function_call_from_element_fn(
      [element_fn = std::forward<ElementFn>(element_fn)](const In &...in, Out &out) {
        new (&out) Out(element_fn(in...));
      },
      exec_preset,
      param_tags);
  return CustomMF(name, std::move(call_fn), param_tags);
}

template<typename Out1, typename Out2, typename... In, typename ElementFn, typename ExecPreset>
inline auto build_multi_function_with_n_inputs_two_outputs(const char *name,
                                                           ElementFn &&element_fn,
                                                           const ExecPreset exec_preset,
                                                           TypeSequence<In...> /*in_types*/)
{
  constexpr auto param_tags = TypeSequence<ParamTag<ParamCategory::SingleInput, In>...,
                                           ParamTag<ParamCategory::SingleOutput, Out1>,
                                           ParamTag<ParamCategory::SingleOutput, Out2>>();
  auto call_fn = build_multi_function_call_from_element_fn(
      std::forward<ElementFn>(element_fn), exec_preset, param_tags);
  return CustomMF(name, call_fn, param_tags);
}

}  // namespace detail

/** Build multi-function with 1 single-input and 1 single-output parameter. */
template<typename In1,
         typename Out1,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI1_SO(const char *name,
                   ElementFn &&element_fn,
                   const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_one_output<Out1>(
      name, std::forward<ElementFn>(element_fn), exec_preset, TypeSequence<In1>());
}

/** Build multi-function with 2 single-input and 1 single-output parameter. */
template<typename In1,
         typename In2,
         typename Out1,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI2_SO(const char *name,
                   ElementFn &&element_fn,
                   const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_one_output<Out1>(
      name, std::forward<ElementFn>(element_fn), exec_preset, TypeSequence<In1, In2>());
}

/** Build multi-function with 3 single-input and 1 single-output parameter. */
template<typename In1,
         typename In2,
         typename In3,
         typename Out1,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI3_SO(const char *name,
                   ElementFn &&element_fn,
                   const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_one_output<Out1>(
      name, std::forward<ElementFn>(element_fn), exec_preset, TypeSequence<In1, In2, In3>());
}

/** Build multi-function with 4 single-input and 1 single-output parameter. */
template<typename In1,
         typename In2,
         typename In3,
         typename In4,
         typename Out1,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI4_SO(const char *name,
                   ElementFn &&element_fn,
                   const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_one_output<Out1>(
      name, std::forward<ElementFn>(element_fn), exec_preset, TypeSequence<In1, In2, In3, In4>());
}

/** Build multi-function with 5 single-input and 1 single-output parameter. */
template<typename In1,
         typename In2,
         typename In3,
         typename In4,
         typename In5,
         typename Out1,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI5_SO(const char *name,
                   ElementFn &&element_fn,
                   const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_one_output<Out1>(
      name,
      std::forward<ElementFn>(element_fn),
      exec_preset,
      TypeSequence<In1, In2, In3, In4, In5>());
}

/** Build multi-function with 6 single-input and 1 single-output parameter. */
template<typename In1,
         typename In2,
         typename In3,
         typename In4,
         typename In5,
         typename In6,
         typename Out1,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI6_SO(const char *name,
                   ElementFn &&element_fn,
                   const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_one_output<Out1>(
      name,
      std::forward<ElementFn>(element_fn),
      exec_preset,
      TypeSequence<In1, In2, In3, In4, In5, In6>());
}

/** Build multi-function with 8 single-input and 1 single-output parameter. */
template<typename In1,
         typename In2,
         typename In3,
         typename In4,
         typename In5,
         typename In6,
         typename In7,
         typename In8,
         typename Out1,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI8_SO(const char *name,
                   ElementFn &&element_fn,
                   const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_one_output<Out1>(
      name,
      std::forward<ElementFn>(element_fn),
      exec_preset,
      TypeSequence<In1, In2, In3, In4, In5, In6, In7, In8>());
}

/** Build multi-function with 1 single-mutable parameter. */
template<typename Mut1, typename ElementFn, typename ExecPreset = exec_presets::AllSpanOrSingle>
inline auto SM(const char *name,
               ElementFn &&element_fn,
               const ExecPreset exec_preset = exec_presets::AllSpanOrSingle())
{
  constexpr auto param_tags = TypeSequence<ParamTag<ParamCategory::SingleMutable, Mut1>>();
  auto call_fn = detail::build_multi_function_call_from_element_fn(
      std::forward<ElementFn>(element_fn), exec_preset, param_tags);
  return detail::CustomMF(name, call_fn, param_tags);
}

/** Build multi-function with 1 single-input and 2 single-output parameter. */
template<typename In1,
         typename Out1,
         typename Out2,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI1_SO2(const char *name,
                    ElementFn &&element_fn,
                    const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_two_outputs<Out1, Out2>(
      name, std::forward<ElementFn>(element_fn), exec_preset, TypeSequence<In1>());
}

/** Build multi-function with 2 single-input and 2 single-output parameter. */
template<typename In1,
         typename In2,
         typename Out1,
         typename Out2,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI2_SO2(const char *name,
                    ElementFn &&element_fn,
                    const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_two_outputs<Out1, Out2>(
      name, std::forward<ElementFn>(element_fn), exec_preset, TypeSequence<In1, In2>());
}

/** Build multi-function with 3 single-input and 2 single-output parameter. */
template<typename In1,
         typename In2,
         typename In3,
         typename Out1,
         typename Out2,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI3_SO2(const char *name,
                    ElementFn &&element_fn,
                    const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_two_outputs<Out1, Out2>(
      name, std::forward<ElementFn>(element_fn), exec_preset, TypeSequence<In1, In2, In3>());
}

/** Build multi-function with 4 single-input and 2 single-output parameter. */
template<typename In1,
         typename In2,
         typename In3,
         typename In4,
         typename Out1,
         typename Out2,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI4_SO2(const char *name,
                    ElementFn &&element_fn,
                    const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_two_outputs<Out1, Out2>(
      name, std::forward<ElementFn>(element_fn), exec_preset, TypeSequence<In1, In2, In3, In4>());
}

/** Build multi-function with 5 single-input and 2 single-output parameter. */
template<typename In1,
         typename In2,
         typename In3,
         typename In4,
         typename In5,
         typename Out1,
         typename Out2,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI5_SO2(const char *name,
                    ElementFn &&element_fn,
                    const ExecPreset exec_preset = exec_presets::Materialized())
{
  return detail::build_multi_function_with_n_inputs_two_outputs<Out1, Out2>(
      name,
      std::forward<ElementFn>(element_fn),
      exec_preset,
      TypeSequence<In1, In2, In3, In4, In5>());
}

/** Build multi-function with 1 single-input and 3 single output parameter. */
template<typename In1,
         typename Out1,
         typename Out2,
         typename Out3,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI1_SO3(const char *name,
                    ElementFn &&element_fn,
                    const ExecPreset exec_preset = exec_presets::Materialized())
{
  constexpr auto param_tags = TypeSequence<ParamTag<ParamCategory::SingleInput, In1>,
                                           ParamTag<ParamCategory::SingleOutput, Out1>,
                                           ParamTag<ParamCategory::SingleOutput, Out2>,
                                           ParamTag<ParamCategory::SingleOutput, Out3>>();
  auto call_fn = detail::build_multi_function_call_from_element_fn(
      std::forward<ElementFn>(element_fn), exec_preset, param_tags);
  return detail::CustomMF(name, call_fn, param_tags);
}

/** Build multi-function with 1 single-input and 4 single output parameter. */
template<typename In1,
         typename Out1,
         typename Out2,
         typename Out3,
         typename Out4,
         typename ElementFn,
         typename ExecPreset = exec_presets::Materialized>
inline auto SI1_SO4(const char *name,
                    ElementFn &&element_fn,
                    const ExecPreset exec_preset = exec_presets::Materialized())
{
  constexpr auto param_tags = TypeSequence<ParamTag<ParamCategory::SingleInput, In1>,
                                           ParamTag<ParamCategory::SingleOutput, Out1>,
                                           ParamTag<ParamCategory::SingleOutput, Out2>,
                                           ParamTag<ParamCategory::SingleOutput, Out3>,
                                           ParamTag<ParamCategory::SingleOutput, Out4>>();
  auto call_fn = detail::build_multi_function_call_from_element_fn(
      std::forward<ElementFn>(element_fn), exec_preset, param_tags);
  return detail::CustomMF(name, call_fn, param_tags);
}

}  // namespace fn::multi_function::build

namespace fn::multi_function {

/**
 * A multi-function that outputs the same value every time. The value is not owned by an instance
 * of this function. If #make_value_copy is false, the caller is responsible for destructing and
 * freeing the value.
 */
class CustomMF_GenericConstant : public MultiFunction {
 public:
  /* For compatible hash with typed class. */
  static constexpr int8_t HASH_ID = 0;

 private:
  const CPPType &type_;
  const void *value_;
  Signature signature_;
  bool owns_value_;

  template<typename T> friend class CustomMF_Constant;

 public:
  CustomMF_GenericConstant(const CPPType &type, const void *value, bool make_value_copy);
  ~CustomMF_GenericConstant() override;
  void call(const IndexMask &mask, Params params, Context context) const override;
  void hash_unique(UniqueHashBytes &hash) const override;
  bool equals(const MultiFunction &other) const override;
};

/**
 * A multi-function that outputs the same array every time. The array is not owned by in instance
 * of this function. The caller is responsible for destructing and freeing the values.
 */
class CustomMF_GenericConstantArray : public MultiFunction {
 private:
  GSpan array_;
  Signature signature_;

 public:
  CustomMF_GenericConstantArray(GSpan array);
  void call(const IndexMask &mask, Params params, Context context) const override;
};

/**
 * Generates a multi-function that outputs a constant value.
 */
template<typename T> class CustomMF_Constant : public MultiFunction {
 private:
  T value_;
  Signature signature_;

 public:
  template<typename U> CustomMF_Constant(U &&value) : value_(std::forward<U>(value))
  {
    SignatureBuilder builder{"Constant", signature_};
    builder.single_output<T>("Value");
    this->set_signature(&signature_);
  }

  void call(const IndexMask &mask, Params params, Context /*context*/) const override
  {
    MutableSpan<T> output = params.uninitialized_single_output<T>(0);
    mask.foreach_index_optimized<int64_t>([&](const int64_t i) { new (&output[i]) T(value_); });
  }

  void hash_unique(UniqueHashBytes &hash) const override
  {
    hash.add(&CustomMF_GenericConstant::HASH_ID);
    hash_unique_default(value_, hash);
    hash.add(&CPPType::get<T>());
  }

  bool equals(const MultiFunction &other) const override
  {
    const CustomMF_Constant *other1 = dynamic_cast<const CustomMF_Constant *>(&other);
    if (other1 != nullptr) {
      return value_ == other1->value_;
    }
    const CustomMF_GenericConstant *other2 = dynamic_cast<const CustomMF_GenericConstant *>(
        &other);
    if (other2 != nullptr) {
      const CPPType &type = CPPType::get<T>();
      if (type == other2->type_) {
        return type.is_equal_or_false(static_cast<const void *>(&value_), other2->value_);
      }
    }
    return false;
  }
};

class CustomMF_DefaultOutput : public MultiFunction {
 private:
  int output_amount_;
  Signature signature_;

 public:
  CustomMF_DefaultOutput(Span<DataType> input_types, Span<DataType> output_types);
  void call(const IndexMask &mask, Params params, Context context) const override;
};

class CustomMF_GenericCopy : public MultiFunction {
 private:
  Signature signature_;

 public:
  CustomMF_GenericCopy(DataType data_type);
  void call(const IndexMask &mask, Params params, Context context) const override;
};

}  // namespace fn::multi_function

}  // namespace blender
