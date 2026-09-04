/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <memory>

/** \file
 * \ingroup fn
 */

#include "BLI_string_ref.hh"

#include "FN_field.hh"

namespace blender::fn {

/** Identifies the geometry context shared by two adjacent Vector Field GPU nodes. */
struct GPUFieldContextKey {
  const void *geometry = nullptr;
  int component_type = -1;
  int domain = -1;
  int subcontext = -1;

  friend bool operator==(const GPUFieldContextKey &a, const GPUFieldContextKey &b)
  {
    return a.geometry == b.geometry && a.component_type == b.component_type &&
           a.domain == b.domain && a.subcontext == b.subcontext;
  }
};

/** Owns intermediate GPU results for one Geometry Nodes evaluation. */
class GPUFieldChainScope {
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend bool try_evaluate_field_gpu(const GField &field,
                                     const FieldContext &context,
                                     const IndexMask &mask,
                                     GMutableSpan dst,
                                     const GPUFieldContextKey *context_key,
                                     GPUFieldChainScope *chain_scope);
  friend bool try_evaluate_field_gpu_to_transient_result(const GField &field,
                                                         const FieldContext &context,
                                                         const IndexMask &mask,
                                                         const GPUFieldContextKey &context_key,
                                                         StringRef result_name,
                                                         GPUFieldChainScope &chain_scope);
  friend bool try_evaluate_field_gpu_impl(const GField &field,
                                          const FieldContext &context,
                                          const IndexMask &mask,
                                          GMutableSpan dst,
                                          const GPUFieldContextKey *context_key,
                                          GPUFieldChainScope *chain_scope,
                                          StringRef transient_result_name);

 public:
  GPUFieldChainScope();
  ~GPUFieldChainScope();
  GPUFieldChainScope(const GPUFieldChainScope &) = delete;
  GPUFieldChainScope &operator=(const GPUFieldChainScope &) = delete;

  /** Read a retained input when its consumer requires CPU evaluation. */
  GVArray get_varray_for_input(const FieldInput &input,
                               const GPUFieldContextKey &context_key,
                               const IndexMask &mask,
                               const FieldContext &fallback_context) const;
};

/**
 * Evaluate a vector field into a caller-owned span. Requires an active GPU context and a
 * contiguous mask. Returns false for unsupported fields so the caller can use CPU evaluation.
 */
bool try_evaluate_field_gpu(const GField &field,
                            const FieldContext &context,
                            const IndexMask &mask,
                            GMutableSpan dst,
                            const GPUFieldContextKey *context_key = nullptr,
                            GPUFieldChainScope *chain_scope = nullptr);

/** Store a result in the scope without readback for a subsequent Vector Field GPU node. */
bool try_evaluate_field_gpu_to_transient_result(const GField &field,
                                                const FieldContext &context,
                                                const IndexMask &mask,
                                                const GPUFieldContextKey &context_key,
                                                StringRef result_name,
                                                GPUFieldChainScope &chain_scope);

}  // namespace blender::fn
