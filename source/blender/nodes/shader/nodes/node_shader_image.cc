/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_image_types.h"

#include "BKE_image.hh"

#include "IMB_colormanagement.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BLT_translation.hh"

#include "node_shader_util.hh"
#include "node_util.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

namespace nodes::node_shader_image_cc {

static void draw_image_colorspace(ui::Layout &layout, PointerRNA *imaptr)
{
  Image *image = static_cast<Image *>(imaptr->data);
  if (!image) {
    return;
  }

  ui::Layout &split = layout.split(0.33f, true);
  PointerRNA colorspace_settings_ptr = RNA_pointer_get(imaptr, "colorspace_settings");
  split.label(IFACE_("Color Space"), ICON_NONE);
  split.prop_with_menu(&colorspace_settings_ptr,
                       "name",
                       ui::ITEM_R_SPLIT_EMPTY_NAME,
                       "",
                       ICON_NONE,
                       "UI_MT_color_space_select");

  if (image->source != IMA_SRC_GENERATED) {
    ui::Layout &split_2 = layout.split(0.33f, true);
    split_2.label(IFACE_("Alpha"), ICON_NONE);
    split_2.prop(imaptr, "alpha_mode", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);

    const bool is_data = IMB_colormanagement_space_name_is_data(image->colorspace_settings.name);
    split_2.active_set(!is_data);
  }

  if (BKE_image_is_dirty(image)) {
    split.enabled_set(false);
  }
}

static void node_declare(NodeDeclarationBuilder &b)
{
  /* Layout before the output keeps Color Space above the Image selector row. */
  b.use_custom_socket_order();
  b.add_layout([](ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr) {
    PointerRNA imaptr = RNA_pointer_get(ptr, "image");
    draw_image_colorspace(layout, &imaptr);
  });
  b.add_output<decl::Image>("Image"_ustr).custom_draw([](CustomSocketDrawParams &params) {
    params.layout.alignment_set(ui::LayoutAlign::Expand);
    template_id(&params.layout,
                &params.C,
                &params.node_ptr,
                "image",
                "IMAGE_OT_new",
                "IMAGE_OT_open",
                nullptr);
  });
}

}  // namespace nodes::node_shader_image_cc

void register_node_type_sh_image()
{
  namespace file_ns = nodes::node_shader_image_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeImage"_ustr, SH_NODE_IMAGE);
  ntype.ui_name = "Image";
  ntype.ui_description = "Input an image data-block";
  ntype.enum_name_legacy = "IMAGE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.default_width = bke::NodeWidth::_240;
  ntype.labelfunc = node_image_label;

  bke::node_register_type(ntype);
}

}  // namespace blender
