/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_image_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Image>("Image"_ustr).optional_label();
  b.add_input<decl::Int>("Frame"_ustr)
      .min(0)
      .description(
          "Which frame to use for videos. Note that different frames in videos can "
          "have different resolutions");

  b.add_output<decl::Int>("Width"_ustr);
  b.add_output<decl::Int>("Height"_ustr);
  b.add_output<decl::Bool>("Has Alpha"_ustr).description("Whether the image has an alpha channel");

  b.add_output<decl::Int>("Frame Count"_ustr)
      .description("The number of animation frames. If a single image, then 1");
  b.add_output<decl::Float>("FPS"_ustr)
      .description("Animation playback speed in frames per second. If a single image, then 0");
}

}  // namespace nodes::node_shader_image_info_cc

void register_node_type_sh_image_info()
{
  namespace file_ns = nodes::node_shader_image_info_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeImageInfo"_ustr, SH_NODE_IMAGE_INFO);
  ntype.ui_name = "Image Info";
  ntype.ui_description = "Retrieve information about an image";
  ntype.enum_name_legacy = "IMAGE_INFO";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.default_width = bke::NodeWidth::_240;

  bke::node_register_type(ntype);
}

}  // namespace blender
