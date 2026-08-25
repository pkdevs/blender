/* SPDX-FileCopyrightText: 2007 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include <cctype>
#include <cstring>
#include <limits>

#include "DNA_image_types.h"
#include "DNA_node_types.h"

#include "BLI_listbase.hh"
#include "BLI_math_rotation_c.hh"
#include "BLI_memory_utils.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"
#include "BLI_utildefines.hh"

#include "BLT_translation.hh"

#include "BKE_colortools.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "MOV_read.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "MEM_guardedalloc.h"

#include "NOD_shader.h"

#include "node_util.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Storage Data
 * \{ */

void node_free_curves(bNode *node)
{
  BKE_curvemapping_free(static_cast<CurveMapping *>(node->storage));
}

void node_free_standard_storage(bNode *node)
{
  if (node->storage) {
    MEM_delete_void(node->storage);
  }
}

void node_copy_curves(bNodeTree * /*dest_ntree*/, bNode *dest_node, const bNode *src_node)
{
  dest_node->storage = BKE_curvemapping_copy(static_cast<CurveMapping *>(src_node->storage));
}

void node_copy_standard_storage(bNodeTree * /*dest_ntree*/,
                                bNode *dest_node,
                                const bNode *src_node)
{
  dest_node->storage = MEM_dupalloc_void(src_node->storage);
}

void *node_initexec_curves(bNodeExecContext * /*context*/, bNode *node, bNodeInstanceKey /*key*/)
{
  BKE_curvemapping_init(static_cast<CurveMapping *>(node->storage));
  return nullptr; /* unused return */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Updates
 * \{ */

void node_sock_label(bNodeSocket *sock, const char *name)
{
  STRNCPY_UTF8(sock->label, name);
}

void node_sock_label_clear(bNodeSocket *sock)
{
  if (sock->label[0] != '\0') {
    sock->label[0] = '\0';
  }
}

void node_math_update(bNodeTree *ntree, bNode *node)
{
  bNodeSocket *sock2 = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 1));
  bNodeSocket *sock3 = static_cast<bNodeSocket *>(BLI_findlink(&node->inputs, 2));
  bke::node_set_socket_availability(*ntree,
                                    *sock2,
                                    !ELEM(node->custom1,
                                          NODE_MATH_SQRT,
                                          NODE_MATH_SIGN,
                                          NODE_MATH_CEIL,
                                          NODE_MATH_SINE,
                                          NODE_MATH_ROUND,
                                          NODE_MATH_FLOOR,
                                          NODE_MATH_COSINE,
                                          NODE_MATH_ARCSINE,
                                          NODE_MATH_TANGENT,
                                          NODE_MATH_ABSOLUTE,
                                          NODE_MATH_RADIANS,
                                          NODE_MATH_DEGREES,
                                          NODE_MATH_FRACTION,
                                          NODE_MATH_ARCCOSINE,
                                          NODE_MATH_ARCTANGENT) &&
                                        !ELEM(node->custom1,
                                              NODE_MATH_INV_SQRT,
                                              NODE_MATH_TRUNC,
                                              NODE_MATH_EXPONENT,
                                              NODE_MATH_COSH,
                                              NODE_MATH_SINH,
                                              NODE_MATH_TANH));
  bke::node_set_socket_availability(*ntree,
                                    *sock3,
                                    ELEM(node->custom1,
                                         NODE_MATH_COMPARE,
                                         NODE_MATH_MULTIPLY_ADD,
                                         NODE_MATH_WRAP,
                                         NODE_MATH_SMOOTH_MIN,
                                         NODE_MATH_SMOOTH_MAX));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Labels
 * \{ */

void node_blend_label(const bNodeTree * /*ntree*/,
                      const bNode *node,
                      char *label,
                      int label_maxncpy)
{
  const char *name;
  bool enum_label = RNA_enum_name(rna_enum_ramp_blend_items, node->custom1, &name);
  if (!enum_label) {
    name = CTX_N_(BLT_I18NCONTEXT_COLOR, "Unknown");
  }
  BLI_strncpy_utf8(label, CTX_IFACE_(BLT_I18NCONTEXT_COLOR, name), label_maxncpy);
}

void node_image_label(const bNodeTree * /*ntree*/,
                      const bNode *node,
                      char *label,
                      int label_maxncpy)
{
  /* Prefer the default node name when the Image input is linked. */
  if (bNodeTree *tree = node->runtime->owner_tree) {
    tree->ensure_topology_cache();
  }
  if (const bNodeSocket *image_socket = bke::node_find_socket(*node, SOCK_IN, "Image"_ustr)) {
    if (bke::node_tree_runtime::topology_cache_is_available(*image_socket) &&
        image_socket->is_logically_linked())
    {
      BLI_strncpy_utf8(label, IFACE_(node->typeinfo->ui_name.c_str()), label_maxncpy);
      return;
    }
  }
  if (Image *image = node_tex_image_get_image(*node)) {
    BLI_strncpy_utf8(label, image->id.name + 2, label_maxncpy);
    return;
  }
  BLI_strncpy_utf8(label, IFACE_(node->typeinfo->ui_name.c_str()), label_maxncpy);
}

static Image *image_from_image_socket_origin(const bNodeSocket &origin)
{
  const bNode &origin_node = origin.owner_node();
  if (origin_node.is_type("ShaderNodeImage"_ustr) ||
      origin_node.is_type("GeometryNodeInputImage"_ustr))
  {
    return id_cast<Image *>(origin_node.id);
  }
  if (origin.type == SOCK_IMAGE && origin.default_value) {
    return origin.default_value_typed<bNodeSocketValueImage>()->value;
  }
  return nullptr;
}

/** Linked Image input, else socket default, else node->id. */
Image *node_tex_image_get_image(const bNode &node)
{
  const bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, "Image"_ustr);
  if (socket == nullptr || socket->type != SOCK_IMAGE) {
    return id_cast<Image *>(node.id);
  }

  if (bNodeTree *tree = node.runtime->owner_tree) {
    tree->ensure_topology_cache();
  }
  if (bke::node_tree_runtime::topology_cache_is_available(*socket) &&
      socket->is_logically_linked())
  {
    return image_from_image_socket_origin(*socket->logically_linked_sockets().first());
  }

  if (socket->default_value) {
    if (Image *image = socket->default_value_typed<bNodeSocketValueImage>()->value) {
      return image;
    }
  }
  return id_cast<Image *>(node.id);
}

void node_socket_set_image(bNode &node, const UString identifier, Image *image)
{
  bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, identifier);
  if (socket == nullptr || socket->type != SOCK_IMAGE || socket->default_value == nullptr) {
    return;
  }
  Image *&socket_image = socket->default_value_typed<bNodeSocketValueImage>()->value;
  if (socket_image != image) {
    id_us_min(id_cast<ID *>(socket_image));
    socket_image = image;
    id_us_plus(id_cast<ID *>(socket_image));
  }
  if (node.id && node.id->id_type() == ID_IM) {
    id_us_min(node.id);
    node.id = nullptr;
  }
}

ShaderImageInfoResult node_shader_evaluate_image_info(Image *image, const int frame)
{
  ShaderImageInfoResult result;
  if (!image) {
    return result;
  }

  ImageUser image_user;
  BKE_imageuser_default(&image_user);
  image_user.frames = std::numeric_limits<int>::max();
  image_user.framenr = BKE_image_is_animated(image) ? frame : 0;

  void *lock;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &image_user, &lock);
  BLI_SCOPED_DEFER([&]() { BKE_image_release_ibuf(image, ibuf, lock); });
  if (!ibuf) {
    return result;
  }

  result.has_alpha = ibuf->can_contain_alpha();
  result.width = ibuf->x;
  result.height = ibuf->y;

  if (ImageAnim *ianim = static_cast<ImageAnim *>(image->anims.first)) {
    MovieReader *anim = ianim->anim;
    if (anim) {
      result.frame_count = MOV_get_duration_frames(anim);
      result.fps = MOV_get_fps(anim);
    }
  }

  return result;
}

void node_math_label(const bNodeTree * /*ntree*/,
                     const bNode *node,
                     char *label,
                     int label_maxncpy)
{
  const char *name;
  bool enum_label = RNA_enum_name(rna_enum_node_math_items, node->custom1, &name);
  if (!enum_label) {
    name = CTX_N_(BLT_I18NCONTEXT_ID_NODETREE, "Unknown");
  }
  BLI_strncpy_utf8(label, CTX_IFACE_(BLT_I18NCONTEXT_ID_NODETREE, name), label_maxncpy);
}

void node_vector_math_label(const bNodeTree * /*ntree*/,
                            const bNode *node,
                            char *label,
                            int label_maxncpy)
{
  const char *name;
  bool enum_label = RNA_enum_name(rna_enum_node_vec_math_items, node->custom1, &name);
  if (!enum_label) {
    name = CTX_N_(BLT_I18NCONTEXT_ID_NODETREE, "Unknown");
  }
  BLI_strncpy_utf8(label, CTX_IFACE_(BLT_I18NCONTEXT_ID_NODETREE, name), label_maxncpy);
}

void node_combsep_color_label(const ListBaseT<bNodeSocket> *sockets, NodeCombSepColorMode mode)
{
  bNodeSocket *sock1 = static_cast<bNodeSocket *>(sockets->first);
  bNodeSocket *sock2 = sock1->next;
  bNodeSocket *sock3 = sock2->next;

  node_sock_label_clear(sock1);
  node_sock_label_clear(sock2);
  node_sock_label_clear(sock3);

  switch (mode) {
    case NODE_COMBSEP_COLOR_RGB:
      node_sock_label(sock1, "Red");
      node_sock_label(sock2, "Green");
      node_sock_label(sock3, "Blue");
      break;
    case NODE_COMBSEP_COLOR_HSL:
      node_sock_label(sock1, "Hue");
      node_sock_label(sock2, "Saturation");
      node_sock_label(sock3, "Lightness");
      break;
    case NODE_COMBSEP_COLOR_HSV:
      node_sock_label(sock1, "Hue");
      node_sock_label(sock2, "Saturation");
      node_sock_label(sock3, "Value");
      break;
    default: {
      BLI_assert_unreachable();
      break;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Link Insertion
 * \{ */

bool node_insert_link_default(bke::NodeInsertLinkParams & /*params*/)
{
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Default value RNA access
 * \{ */

int node_socket_get_int(bNodeTree *ntree, bNode * /*node*/, bNodeSocket *sock)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  return RNA_int_get(&ptr, "default_value");
}

void node_socket_set_int(bNodeTree *ntree, bNode * /*node*/, bNodeSocket *sock, int value)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  RNA_int_set(&ptr, "default_value", value);
}

bool node_socket_get_bool(bNodeTree *ntree, bNode * /*node*/, bNodeSocket *sock)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  return RNA_boolean_get(&ptr, "default_value");
}

void node_socket_set_bool(bNodeTree *ntree, bNode * /*node*/, bNodeSocket *sock, bool value)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  RNA_boolean_set(&ptr, "default_value", value);
}

float node_socket_get_float(bNodeTree *ntree, bNode * /*node*/, bNodeSocket *sock)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  return RNA_float_get(&ptr, "default_value");
}

void node_socket_set_float(bNodeTree *ntree, bNode * /*node*/, bNodeSocket *sock, float value)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  RNA_float_set(&ptr, "default_value", value);
}

void node_socket_get_color(bNodeTree *ntree, bNode * /*node*/, bNodeSocket *sock, float *value)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  RNA_float_get_array(&ptr, "default_value", value);
}

void node_socket_set_color(bNodeTree *ntree,
                           bNode * /*node*/,
                           bNodeSocket *sock,
                           const float *value)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  RNA_float_set_array(&ptr, "default_value", value);
}

void node_socket_get_vector(bNodeTree *ntree, bNode * /*node*/, bNodeSocket *sock, float *value)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  RNA_float_get_array(&ptr, "default_value", value);
}

void node_socket_set_vector(bNodeTree *ntree,
                            bNode * /*node*/,
                            bNodeSocket *sock,
                            const float *value)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  RNA_float_set_array(&ptr, "default_value", value);
}

void node_socket_get_rotation(bNodeTree *ntree, bNode * /*node*/, bNodeSocket *sock, float *value)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  float euler[3];
  RNA_float_get_array(&ptr, "default_value", euler);
  eul_to_quat(value, euler);
}

void node_socket_set_rotation(bNodeTree *ntree,
                              bNode * /*node*/,
                              bNodeSocket *sock,
                              const float *value)
{
  PointerRNA ptr = RNA_pointer_create_discrete(id_cast<ID *>(ntree), RNA_NodeSocket, sock);
  float euler[3];
  quat_to_eul(euler, value);
  RNA_float_set_array(&ptr, "default_value", euler);
}

/** \} */

}  // namespace blender
