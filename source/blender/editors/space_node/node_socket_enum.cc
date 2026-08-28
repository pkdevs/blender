/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstdlib>

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "NOD_socket_search_link.hh"

#include "BKE_context.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_string.hh"

#include "node_intern.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface_layout.hh"

namespace blender::ed::space_node {

const EnumPropertyItem enum_fallback[] = {
    {0, "NONE", 0, "None", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem *enum_set_itemf(bContext *C,
                                              PointerRNA * /*ptr*/,
                                              PropertyRNA * /*prop*/,
                                              bool *r_free)
{
  *r_free = false;
  if (!C) {
    return enum_fallback;
  }
  const PointerRNA sock_ptr = CTX_data_pointer_get_type(C, "active_socket", RNA_NodeSocket);
  const bNodeSocket *sock = static_cast<const bNodeSocket *>(sock_ptr.data);
  if (!sock || !sock->runtime || !sock->runtime->declaration) {
    return enum_fallback;
  }
  const nodes::SocketDeclaration *decl = sock->runtime->declaration;
  if (!decl || !decl->enum_choices) {
    return enum_fallback;
  }
  return decl->enum_choices;
}

static wmOperatorStatus enum_set_exec(bContext *C, wmOperator *op)
{
  PointerRNA sock_ptr = CTX_data_pointer_get_type(C, "active_socket", RNA_NodeSocket);
  bNodeSocket *sock = static_cast<bNodeSocket *>(sock_ptr.data);
  if (!sock || !sock->runtime || !sock->runtime->declaration) {
    return OPERATOR_CANCELLED;
  }
  const nodes::SocketDeclaration *decl = sock->runtime->declaration;
  const EnumPropertyItem *choices = decl ? decl->enum_choices : nullptr;
  if (!choices) {
    return OPERATOR_CANCELLED;
  }

  /* "value" is an int enum; its value is the index. identifier = typed value string. */
  const int idx = RNA_enum_get(op->ptr, "value");
  const char *typed_value = nullptr;
  for (const EnumPropertyItem *it = choices; it->identifier; it++) {
    if (it->value == idx) {
      typed_value = it->identifier;
      break;
    }
  }
  if (!typed_value) {
    return OPERATOR_CANCELLED;
  }

  switch (sock->type) {
    case SOCK_STRING: {
      bNodeSocketValueString *sv = static_cast<bNodeSocketValueString *>(sock->default_value);
      STRNCPY(sv->value, typed_value);
      break;
    }
    case SOCK_FLOAT: {
      float parsed = std::strtof(typed_value, nullptr);
      static_cast<bNodeSocketValueFloat *>(sock->default_value)->value = parsed;
      break;
    }
    case SOCK_INT: {
      int parsed = std::strtol(typed_value, nullptr, 0);
      static_cast<bNodeSocketValueInt *>(sock->default_value)->value = parsed;
      break;
    }
    case SOCK_BOOLEAN: {
      const bool parsed = STREQ(typed_value, "true") || STREQ(typed_value, "1");
      static_cast<bNodeSocketValueBoolean *>(sock->default_value)->value = parsed;
      break;
    }
    default:
      break;
  }

  /* Propagate the change via the socket's runtime owner references. */
  if (sock->runtime && sock->runtime->owner_node) {
    bNodeTree &ntree = sock->owner_tree();
    bNode &node = sock->owner_node();
    BKE_ntree_update_tag_node_property(&ntree, &node);
    Main *bmain = CTX_data_main(C);
    BKE_main_ensure_invariants(*bmain, ntree.id);
  }
  return OPERATOR_FINISHED;
}

void NODE_OT_socket_enum_set(wmOperatorType *ot)
{
  ot->name = "Set Socket Enum";
  ot->idname = "NODE_OT_socket_enum_set";
  ot->description = "Set a socket to one value of a socket enum";
  ot->exec = enum_set_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "value",
                                   rna_enum_dummy_DEFAULT_items,
                                   0,
                                   "Value",
                                   "Enum choice to write to the socket");
  RNA_def_property_enum_funcs_runtime(prop, nullptr, nullptr, enum_set_itemf, nullptr, nullptr);
  ot->prop = prop;
}

}  // namespace blender::ed::space_node
