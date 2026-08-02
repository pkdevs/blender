/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "resource.hh"
#include "symbol_scope.hh"
#include "symbol_template.hh"

namespace bsl {

struct SymbolClass : SymbolScope {
  ResourceTableType srt_type = ResourceTableType::NONE;

  SymbolFunction *operator_subscript = nullptr;
  SymbolClassTemplate *template_data = nullptr;

  /* For enums, the value of the last declaration. */
  ConstexprValue enum_last_val = -1;

  int size = -1;  /* -1 is for not computed yet. */
  int align = -1; /* -1 is for not computed yet. */

  /* For builtin types, number of components. */
  int8_t comp_len = 0;

  bool is_builtin = false;
  bool is_anonymous = false;
  bool is_union = false;
  bool is_enum = false;
  bool is_std140_compatible = false;
  bool is_std430_compatible = false;
  bool is_error = false;

  SymbolClass(SymbolScope *parent, ast::ClassDecl decl, const string &suffix = "");

  SymbolClass(SymbolScope *parent,
              Token tok,
              const string &builtin_id,
              int size_in_bytes = 1,
              int align_in_bytes = 1)
      : SymbolScope(parent, tok, builtin_id, CLASS),
        size(size_in_bytes),
        align(align_in_bytes),
        is_builtin(true)
  {
  }

  void ensure_size_and_align(NodeErrorHandler *err_handler = nullptr, bool check_st430 = false);

  bool is_srt() const
  {
    return srt_type != ResourceTableType::NONE;
  }

  /* Structure to hold the flattened member data */
  struct MemberNode {
    SymbolClass *type;
    string path;
    int absolute_offset;
  };

  Result<vector<MemberNode>, TokenException> get_all_members_flat() const;

 private:
  static optional<TokenException> get_flat_members_recursive(const SymbolClass *cls,
                                                             const string &current_path,
                                                             int current_offset,
                                                             vector<MemberNode> &out_buffer);
};

}  // namespace bsl
