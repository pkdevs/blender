/* SPDX-FileCopyrightText: 2013 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 *
 * Evaluation engine entry-points for Depsgraph Engine.
 */

#pragma once

namespace blender::deg {

struct Depsgraph;

/**
 * Evaluate all nodes tagged for updating,
 * \warning This is usually done as part of main loop, but may also be
 * called from frame-change update.
 *
 * \note Time sources should be all valid!
 */
void deg_evaluate_on_refresh(Depsgraph *graph);

/**
 * Evaluate all Copy On Eval nodes in the depsgraph. This ensures the depsgraph is in a state where
 * it doesn't have to read from Main anymore.
 */
void deg_evaluate_copy_on_eval(Depsgraph *graph);

}  // namespace blender::deg
