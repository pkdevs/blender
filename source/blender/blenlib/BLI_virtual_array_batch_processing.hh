/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_devirtualize_parameters.hh"
#include "BLI_generic_virtual_array.hh"
#include "BLI_parameter_pack_utils.hh"

namespace blender::varray_batch {

enum class ParamCategory {
  Input,
  Output,
  Mutable,
};

template<ParamCategory Category, typename T> struct ParamTag {
  static constexpr ParamCategory category = Category;
  using base_type = T;
};

/** Method to execute a function in case devirtualization was not possible. */
enum class FallbackMode {
  /** Access all elements in virtual arrays through virtual function calls. */
  Simple,
  /** Process elements in chunks to reduce virtual function call overhead. */
  Materialized,
};

/**
 * These presets determine what code is generated for evaluation. Different presets make different
 * trade-offs between run-time performance and compile-time/binary size.
 */
namespace exec_presets {

/**
 * The "naive" method for batch processing. Every element is processed separately and input
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
      if constexpr (ParamTag::category == ParamCategory::Input) {
        const GVArrayImpl &varray_impl = *std::get<I>(loaded_params);
        return GVArrayDevirtualizer<T, true, true>{varray_impl};
      }
      else if constexpr (ELEM(ParamTag::category, ParamCategory::Output, ParamCategory::Mutable)) {
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

      if constexpr (ParamTag::category == ParamCategory::Input) {
        constexpr bool UseSpan = ValueSequence<size_t, Indices...>::template contains<I>();
        const GVArrayImpl &varray_impl = *std::get<I>(loaded_params);
        return GVArrayDevirtualizer<T, true, UseSpan>{varray_impl};
      }
      else if constexpr (ELEM(ParamTag::category, ParamCategory::Output, ParamCategory::Mutable)) {
        T *ptr = std::get<I>(loaded_params);
        return BasicDevirtualizer<T *>{ptr};
      }
    }()...);
  }
};

}  // namespace exec_presets

template<typename T>
concept ExecPresetC = requires {
  { T::use_devirtualization } -> std::convertible_to<bool>;
  { T::fallback_mode } -> std::convertible_to<FallbackMode>;

  requires(!T::use_devirtualization ||
           requires(const T preset, const std::tuple<const GVArrayImpl *> &loaded_params) {
             preset.create_devirtualizers(TypeSequence<ParamTag<ParamCategory::Input, int>>(),
                                          std::index_sequence<0>(),
                                          loaded_params);
           });
};

template<typename T>
concept ParamTagC = requires {
  typename T::base_type;
  { T::category } -> std::convertible_to<ParamCategory>;
};

enum class BatchArgMode {
  Unknown,
  Single,
  Span,
  Materialized,
};

template<typename ParamTag> struct BatchArgInfo {
  BatchArgMode mode = BatchArgMode::Unknown;
  const typename ParamTag::base_type *internal_span_data;
};

template<typename... ParamTags, typename ElementFn, typename... Chunks>
#if (defined(__GNUC__) && !defined(__clang__))
[[gnu::optimize("-funroll-loops")]] [[gnu::optimize("O3")]]
#endif
inline void patch_process_impl(TypeSequence<ParamTags...> /*param_tags*/,
                               const ElementFn &element_fn,
                               const int64_t size,
                               Chunks &&__restrict... chunks)
{
  for (int64_t i = 0; i < size; i++) {
    element_fn(chunks[i]...);
  }
}

template<int64_t ChunkSize,
         ParamTagC... ParamTags,
         size_t... I,
         typename ElementFn,
         typename... Params>
inline void batch_process(TypeSequence<ParamTags...> /*param_tags*/,
                          std::index_sequence<I...> /*indices*/,
                          ElementFn &&element_fn,
                          const IndexMaskSegment mask,
                          const std::tuple<Params...> &params)
{
  const int64_t mask_size = mask.size();
  const int64_t tmp_buffer_size = std::min(mask_size, ChunkSize);

  /* Local buffers that are used to temporarily store values for processing. */
  std::tuple<TypedBuffer<typename ParamTags::base_type, ChunkSize>...> temporary_buffers;

  /* Information about every parameter. */
  std::tuple<BatchArgInfo<ParamTags>...> args_info;

  (
      /* Setup information for all parameters. */
      [&] {
        /* Use `typedef` instead of `using` to work around a compiler bug. */
        using ParamTag = ParamTags;
        using T = typename ParamTag::base_type;
        [[maybe_unused]] BatchArgInfo<ParamTags> &arg_info = std::get<I>(args_info);
        if constexpr (ParamTag::category == ParamCategory::Input) {
          const auto &varray_impl = *std::get<I>(params);
          const CommonVArrayInfo common_info = varray_impl.common_info();
          if (common_info.type == CommonVArrayInfo::Type::Single) {
            /* If an input #VArray is a single value, we have to fill the buffer with that value
             * only once. The same unchanged buffer can then be reused in every chunk. */
            const T &in_single = *static_cast<const T *>(common_info.data);
            T *tmp_buffer = std::get<I>(temporary_buffers).ptr();
            uninitialized_fill_n(tmp_buffer, tmp_buffer_size, in_single);
            arg_info.mode = BatchArgMode::Single;
          }
          else if (common_info.type == CommonVArrayInfo::Type::Span) {
            /* Remember the span so that it doesn't have to be retrieved in every iteration. */
            arg_info.internal_span_data = static_cast<const T *>(common_info.data);
          }
          else {
            arg_info.internal_span_data = nullptr;
          }
        }
      }(),
      ...);

  IndexMaskFromSegment index_mask_from_segment;
  const int64_t segment_offset = mask.offset();

  /* Outer loop over all chunks. */
  for (int64_t chunk_start = 0; chunk_start < mask_size; chunk_start += ChunkSize) {
    const int64_t chunk_end = std::min<int64_t>(chunk_start + ChunkSize, mask_size);
    const int64_t chunk_size = chunk_end - chunk_start;
    const IndexMaskSegment sliced_mask = mask.slice(chunk_start, chunk_size);
    const int64_t mask_start = sliced_mask[0];
    const bool sliced_mask_is_range = unique_sorted_indices::non_empty_is_range(
        sliced_mask.base_span());

    /* Move mutable data into temporary array. */
    if (!sliced_mask_is_range) {
      (
          [&] {
            /* Use `typedef` instead of `using` to work around a compiler bug. */
            using ParamTag = ParamTags;
            using T = typename ParamTag::base_type;
            if constexpr (ParamTag::category == ParamCategory::Mutable) {
              T *tmp_buffer = std::get<I>(temporary_buffers).ptr();
              T *param_buffer = std::get<I>(params);
              for (int64_t i = 0; i < chunk_size; i++) {
                new (tmp_buffer + i) T(std::move(param_buffer[sliced_mask[i]]));
              }
            }
          }(),
          ...);
    }

    const IndexMask *current_segment_mask = nullptr;

    patch_process_impl(
        TypeSequence<ParamTags...>(),
        element_fn,
        chunk_size,
        /* Prepare every parameter for this chunk. */
        [&] {
          using ParamTag = ParamTags;
          using T = typename ParamTag::base_type;
          [[maybe_unused]] BatchArgInfo<ParamTags> &arg_info = std::get<I>(args_info);
          T *tmp_buffer = std::get<I>(temporary_buffers);
          if constexpr (ParamTag::category == ParamCategory::Input) {
            if (arg_info.mode == BatchArgMode::Single) {
              /* The single value has been filled into a buffer already reused for every chunk. */
              return const_cast<const T *>(tmp_buffer);
            }
            if (sliced_mask_is_range && arg_info.internal_span_data != nullptr) {
              /* In this case we can just use an existing span instead of "compressing" it into
               * a new temporary buffer. */
              arg_info.mode = BatchArgMode::Span;
              return arg_info.internal_span_data + mask_start;
            }
            const auto &varray_impl = *std::get<I>(params);
            if (current_segment_mask == nullptr) {
              current_segment_mask = &index_mask_from_segment.update(
                  {segment_offset, sliced_mask.base_span()});
            }
            /* As a fallback, do a virtual function call to retrieve all elements in the current
             * chunk. The elements are stored in a temporary buffer reused for every chunk. */
            varray_impl.materialize_compressed(*current_segment_mask, tmp_buffer, true);
            /* Remember that this parameter has been materialized, so that the values are
             * destructed properly when the chunk is done. */
            arg_info.mode = BatchArgMode::Materialized;
            return const_cast<const T *>(tmp_buffer);
          }
          else if constexpr (ELEM(ParamTag::category,
                                  ParamCategory::Output,
                                  ParamCategory::Mutable)) {
            /* For outputs, just pass a pointer. This is important so that `__restrict` works. */
            if (sliced_mask_is_range) {
              /* Can write into the caller-provided buffer directly. */
              T *param_buffer = std::get<I>(params);
              return param_buffer + mask_start;
            }
            /* Use the temporary buffer. The values will have to be copied out of that
             * buffer into the caller-provided buffer afterwards. */
            return tmp_buffer;
          }
        }()...);

    /* Relocate outputs from temporary buffers to buffers provided by caller. */
    if (!sliced_mask_is_range) {
      (
          [&] {
            /* Use `typedef` instead of `using` to work around a compiler bug. */
            using ParamTag = ParamTags;
            using T = typename ParamTag::base_type;
            if constexpr (ELEM(
                              ParamTag::category, ParamCategory::Output, ParamCategory::Mutable)) {
              T *tmp_buffer = std::get<I>(temporary_buffers).ptr();
              T *param_buffer = std::get<I>(params);
              for (int64_t i = 0; i < chunk_size; i++) {
                new (param_buffer + sliced_mask[i]) T(std::move(tmp_buffer[i]));
                std::destroy_at(tmp_buffer + i);
              }
            }
          }(),
          ...);
    }

    (
        /* Destruct values that have been materialized before. */
        [&] {
          /* Use `typedef` instead of `using` to work around a compiler bug. */
          using ParamTag = ParamTags;
          using T = typename ParamTag::base_type;
          [[maybe_unused]] BatchArgInfo<ParamTags> &arg_info = std::get<I>(args_info);
          if constexpr (ParamTag::category == ParamCategory::Input) {
            if (arg_info.mode == BatchArgMode::Materialized) {
              T *tmp_buffer = std::get<I>(temporary_buffers).ptr();
              destruct_n(tmp_buffer, chunk_size);
            }
          }
        }(),
        ...);
  }

  (
      /* Destruct buffers for single value inputs. */
      [&] {
        /* Use `typedef` instead of `using` to work around a compiler bug. */
        using ParamTag = ParamTags;
        using T = typename ParamTag::base_type;
        [[maybe_unused]] BatchArgInfo<ParamTags> &arg_info = std::get<I>(args_info);
        if constexpr (ParamTag::category == ParamCategory::Input) {
          if (arg_info.mode == BatchArgMode::Single) {
            T *tmp_buffer = std::get<I>(temporary_buffers).ptr();
            destruct_n(tmp_buffer, tmp_buffer_size);
          }
        }
      }(),
      ...);
}

template<typename Out, typename ElementFn, typename... In>
inline void batch_process__n_in__1_out(const IndexMask &mask,
                                       const VArray<In> &...in_params,
                                       MutableSpan<Out> out_param,
                                       ElementFn &&element_fn)
{
  constexpr auto param_tags =
      TypeSequence<ParamTag<ParamCategory::Input, In>..., ParamTag<ParamCategory::Output, Out>>();
  std::tuple params{in_params.get_implementation()..., out_param.data()};
  mask.foreach_segment([&](const IndexMaskSegment segment) {
    batch_process<64>(
        param_tags,
        std::make_index_sequence<param_tags.size()>(),
        [&](const In &...in, Out &out) { new (&out) Out(element_fn(in...)); },
        segment,
        params);
  });
}

template<typename In1, typename In2, typename Out1, typename ElementFn>
inline void batch_process__2_varray_in__1_span_out(const IndexMask &mask,
                                                   const VArray<In1> &in1,
                                                   const VArray<In2> &in2,
                                                   MutableSpan<Out1> out1,
                                                   ElementFn &&element_fn)
{
  batch_process__n_in__1_out<Out1, ElementFn, In1, In2>(
      mask, in1, in2, out1, std::forward<ElementFn>(element_fn));
}

}  // namespace blender::varray_batch
