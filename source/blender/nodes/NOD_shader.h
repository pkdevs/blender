/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

#include "BKE_node.hh"

namespace blender {

extern struct bke::bNodeTreeType *ntreeType_Shader;

void register_node_type_sh_custom_group(bke::bNodeType *ntype);

struct bNodeTreeExec *ntreeShaderBeginExecTree(struct bNodeTree *ntree);
void ntreeShaderEndExecTree(struct bNodeTreeExec *exec);

/**
 * Find an output node of the shader tree.
 *
 * \note it will only return output which is NOT in the group, which isn't how
 * render engines works but it's how the GPU shader compilation works. This we
 * can change in the future and make it a generic function, but for now it stays
 * private here.
 */
struct bNode *ntreeShaderOutputNode(struct bNodeTree *ntree, int target);

/**
 * This one needs to work on a local tree.
 */
void ntreeGPUMaterialNodes(struct bNodeTree *localtree, struct GPUMaterial *mat);

/** Evaluate Image Info outputs for constant folding in the shader inliner. */
struct ShaderImageInfoResult {
  int width = 0;
  int height = 0;
  bool has_alpha = false;
  int frame_count = 1;
  float fps = 0.0f;
};
struct ShaderImageInfoResult node_shader_evaluate_image_info(struct Image *image, int frame);

/**
 * Image from a linked Image input (e.g. ShaderNodeImage), else socket default, else node->id.
 */
struct Image *node_tex_image_get_image(const struct bNode &node);
/** Set an Image socket value and clear any leftover Image on node->id. */
void node_socket_set_image(struct bNode &node, UString identifier, struct Image *image);

}  // namespace blender
