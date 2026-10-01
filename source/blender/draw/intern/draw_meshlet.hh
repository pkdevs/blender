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
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "draw_shader_shared.hh"

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
  friend class Manager;

  gpu::Batch *batch_ = nullptr;
  /** Whether the batch is owned by this draw-mesh and discarded on destruction. */
  bool owns_batch_ = false;
  /**
   * When not #GPU_PRIM_NONE, this draw-mesh is procedural: no #gpu::Batch is stored and the batch
   * is resolved from the active GPU context on every access (see #batch_get).
   */
  GPUPrimType batch_prim_ = GPU_PRIM_NONE;

  /** Bounds and draw range of the whole batch, or of the whole procedural primitive. */
  DrawMeshlet meshlet_;
  /**
   * Meshlets covering #batch_. Empty until a mesh carries more than one, in which case this
   * spans them and #meshlet_ is unused. Reserved for meshlet splitting.
   */
  Span<DrawMeshlet> meshlets_;
  /**
   * The manager `sync_counter_` of the frame in which #meshlets() was last registered with the
   * manager; 0 = never registered. Per-manager stamping keeps a mesh registered once per manager
   * per frame even when several managers (viewports) draw it.
   */
  uint meshlet_registered_sync_ = 0;
  /**
   * First #MeshletID of the registered meshlets. Valid while #meshlet_registered_sync_ matches
   * the manager's current sync counter.
   */
  MeshletID meshlet_id_ = 0;

 public:
  MEM_CXX_CLASS_ALLOC_FUNCS("Mesh")

  /**
   * Wraps \a batch. Takes ownership by default; pass `owns_batch = false` for batches owned by
   * other data (motion paths) that must outlive every command referencing them.
   *
   * Prefills #meshlet_ from the batch's draw parameters and encodes \a bounding_sphere into
   * #meshlet_.packed_bounds.
   */
  Mesh(gpu::Batch *batch, bool owns_batch = true, float4 bounding_sphere = float4(0.0f));
  /**
   * Creates a procedural draw-mesh that stores no batch.
   *
   * The underlying batch is resolved from the active GPU context on access (see #batch_get), so
   * the same draw-mesh can be safely shared across threads and contexts.
   *
   * The procedural batch is a per-context dummy whose draw count is supplied per call, not by
   * the batch, so #meshlet_ is left empty.
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
  /** Meshlets of this mesh. Never empty for a mesh with geometry. */
  Span<DrawMeshlet> meshlets() const
  {
    return meshlets_.is_empty() ? Span<DrawMeshlet>(&meshlet_, 1) : meshlets_;
  }

  /**
   * (Re)fill #meshlet_ from #batch_get()'s draw parameters.
   *
   * \a bounding_sphere is in object space. Its encoding into #meshlet_.packed_bounds is a full
   * AABB at 5 bits per axis-value quantized against the object's own bounds; see
   * `draw::DrawMeshlet`.
   *
   * The batch may be in the `DRW_batch_request` state (no geometry yet); the meshlet is then
   * left empty, and the cache owner calls this again after the batch has been filled.
   */
  void meshlet_resync(float4 bounding_sphere);

  static inline void clear(Mesh *mesh)
  {
    if (mesh) {
      BLI_assert(mesh->owns_batch_);
      GPU_BATCH_CLEAR_SAFE(mesh->batch_);
      mesh->meshlet_ = DrawMeshlet();
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
