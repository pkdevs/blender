/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#pragma once

#include <memory>

#include "GPU_batch.hh"
#include "MEM_guardedalloc.h"

namespace blender::draw {

/**
 * Wrapper around a #gpu::Batch, owning or borrowing it.
 *
 * \warning This type is named `Mesh`, which shadows the DNA `Mesh` struct for unqualified lookup
 * inside `namespace blender::draw`. Always refer to the DNA type as `blender::Mesh` within this
 * namespace: a bare `Mesh` silently means *this* type, so a missing qualifier compiles as a
 * different type rather than an error.
 */
struct Mesh {
 private:
  /** GPU batch wrapped by this draw-mesh. */
  gpu::Batch *batch_ = nullptr;
  /** Whether the batch is owned by this draw-mesh and discarded on destruction. */
  bool owns_batch_ = false;
  /**
   * When not #GPU_PRIM_NONE, this draw-mesh is procedural: no #gpu::Batch is stored and the batch
   * is resolved from the active GPU context on every access (see #batch_get).
   */
  GPUPrimType batch_prim_ = GPU_PRIM_NONE;

#if 0
  /* Commented out as meshlet integration will be added later.*/
  /**
   * Refers to meshlets of the batch.
   */
  Span<Meshlet> meshlets_;
  /**
   * Contains the extracted meshlet for non-owning batches. Would contain the data to draw the
   * whole batch and \a meshlets_ would point to this entry.
   */
  Meshlet meshlet_;
#endif

 public:
  MEM_CXX_CLASS_ALLOC_FUNCS("Mesh")

  /** Takes ownership of \a batch. The batch is discarded when the #Mesh is destroyed. */
  Mesh(gpu::Batch *batch);
  /**
   * Wraps \a batch, taking ownership only when \a owns_batch is true.
   *
   * Use `owns_batch = false` for batches that are not owned by the draw system, e.g. batches owned
   * by other data (motion paths). The batch must outlive every command that references it.
   */
  Mesh(gpu::Batch *batch, bool owns_batch);
  /**
   * Creates a procedural draw-mesh that stores no batch.
   *
   * The underlying batch is resolved from the active GPU context on access (see #batch_get), so
   * the same draw-mesh can be safely shared across threads and contexts.
   *
   * \a batch_prim must not be #GPU_PRIM_NONE, which is reserved as the sentinel for regular
   * (non-procedural) draw-meshes.
   */
  explicit Mesh(GPUPrimType batch_prim);

  ~Mesh()
  {
    if (owns_batch_) {
      GPU_BATCH_DISCARD_SAFE(batch_);
    }
    batch_ = nullptr;
  }

  Mesh(const Mesh &) = delete;
  Mesh &operator=(const Mesh &) = delete;
  Mesh(Mesh &&) = delete;
  Mesh &operator=(Mesh &&) = delete;

  /**
   * Resolve the underlying #gpu::Batch.
   *
   * For procedural draw-meshes this fetches the batch from the active GPU context, so the result
   * follows context and thread switches. For regular draw-meshes it returns the wrapped batch.
   */
  gpu::Batch *batch_get() const;

  static inline void clear(Mesh *mesh)
  {
    if (mesh) {
      BLI_assert(mesh->owns_batch_);
      GPU_BATCH_CLEAR_SAFE(mesh->batch_);
    }
  }
};

using MeshPtr = std::unique_ptr<Mesh>;

/**
 * Create a #MeshPtr that takes ownership of \a batch.
 *
 * \note The #Mesh owns \a batch and discards it on destruction. Only wrap batches that are
 * owned and not referenced by other code.
 */
inline MeshPtr make_mesh(gpu::Batch *batch)
{
  return std::make_unique<Mesh>(batch);
}

/**
 * Create a #MeshPtr that borrows \a batch, without taking ownership of it.
 *
 * Use this for batches that are not owned by the draw system but that need a #Mesh to outlive
 * the command recording, e.g. motion path batches. The batch must outlive every command that
 * references it.
 */
inline MeshPtr make_mesh_borrowed(gpu::Batch *batch)
{
  return std::make_unique<Mesh>(batch, /*owns_batch=*/false);
}

}  // namespace blender::draw
