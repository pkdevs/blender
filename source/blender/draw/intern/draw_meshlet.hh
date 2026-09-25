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

struct DrawMesh {
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
  // TODO: Vector<MeshLet> meshlets;
 public:
  /** Takes ownership of \a batch. The batch is discarded when the #DrawMesh is destroyed. */
  DrawMesh(gpu::Batch *batch);
  /**
   * Wraps \a batch without taking ownership.
   *
   * Use this for batches that are not owned by the draw system, e.g. batches owned by other data
   * (motion paths). The batch must outlive every command that references it.
   */
  DrawMesh(gpu::Batch *batch, bool owns_batch);
  /**
   * Creates a procedural draw-mesh that stores no batch.
   *
   * The underlying batch is resolved from the active GPU context on access (see #batch_get), so
   * the same draw-mesh can be safely shared across threads and contexts.
   *
   * \a batch_prim must not be #GPU_PRIM_NONE, which is reserved as the sentinel for regular
   * (non-procedural) draw-meshes.
   */
  explicit DrawMesh(GPUPrimType batch_prim);

  ~DrawMesh()
  {
    if (owns_batch_) {
      GPU_BATCH_DISCARD_SAFE(batch_);
    }
    batch_ = nullptr;
  }

  DrawMesh(const DrawMesh &) = delete;
  DrawMesh &operator=(const DrawMesh &) = delete;
  DrawMesh(DrawMesh &&) = delete;
  DrawMesh &operator=(DrawMesh &&) = delete;

  /**
   * Resolve the underlying #gpu::Batch.
   *
   * For procedural draw-meshes this fetches the batch from the active GPU context, so the result
   * follows context and thread switches. For regular draw-meshes it returns the wrapped batch.
   */
  gpu::Batch *batch_get() const;

  static inline void clear(DrawMesh *draw_mesh)
  {
    /* Never clear procedural draw-meshes: they reference the context-owned singleton batch. */
    if (draw_mesh && draw_mesh->batch_) {
      GPU_batch_clear(draw_mesh->batch_);
    }
  }
  static inline void discard(DrawMesh *&draw_mesh)
  {
    if (draw_mesh) {
      MEM_delete(draw_mesh);
      draw_mesh = nullptr;
    }
  }
};

/**
 * Deleter for #DrawMeshPtr. Frees the #DrawMesh; its destructor discards the GPU batch it owns.
 */
class DrawMeshDeleter {
 public:
  void operator()(DrawMesh *draw_mesh)
  {
    MEM_delete(draw_mesh);
  }
};

/** Owning pointer to a #DrawMesh. */
using DrawMeshPtr = std::unique_ptr<DrawMesh, DrawMeshDeleter>;

/**
 * Create a #DrawMeshPtr that takes ownership of \a batch.
 *
 * \note The #DrawMesh owns \a batch and discards it on destruction. Only wrap batches that are
 * owned and not referenced by other code.
 */
inline DrawMeshPtr make_draw_mesh(gpu::Batch *batch)
{
  return DrawMeshPtr(MEM_new<DrawMesh>(__func__, batch));
}

/**
 * Create a #DrawMeshPtr wrapping \a batch without taking ownership.
 *
 * Use this for batches that are not owned by the draw system but that need a #DrawMesh to outlive
 * the command recording, e.g. motion path batches.
 */
inline DrawMeshPtr make_draw_mesh(gpu::Batch *batch, bool owns_batch)
{
  return DrawMeshPtr(MEM_new<DrawMesh>(__func__, batch, owns_batch));
}

}  // namespace blender::draw
