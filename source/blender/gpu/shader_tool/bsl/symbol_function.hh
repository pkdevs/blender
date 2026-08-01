/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "symbol_scope.hh"
#include "symbol_template.hh"

namespace bsl {

struct SymbolFunction : SymbolScope {
  /* Single linked list of overloads. */
  SymbolFunction *overload_next = nullptr;

  SymbolFunctionTemplate *template_data = nullptr;
  SymbolClass *return_type = nullptr;

  vector<SymbolClass *> arg_types;
  vector<ast::Expr> arg_defaults;

  bool is_error = false;
  /* False if this is a template root and is not a symbol to match on. */
  bool is_complete = true;
  /* If true, will allow vector type casting. */
  bool is_builtin_ctor = false;

  enum Type { STATIC, MEMBER, GLOBAL } fn_type;
  enum class EntryPointType { FRAG, VERT, COMP, NONE } entry_point_type;

  SymbolFunction(SymbolScope *parent,
                 SymbolClass *return_type,
                 ast::FuncDecl decl,
                 const string &suffix = "")
      : SymbolScope(
            parent, decl.front(), string(decl.identifier().name().str()) + suffix, FUNCTION),
        return_type(return_type),
        fn_type(decl.is_method() ? (decl.is_static() ? STATIC : MEMBER) : GLOBAL),
        entry_point_type(EntryPointType::NONE)
  {
    if (decl.is_method()) {
      loc = decl.parent(ast::NodeType::ClassDecl).front();
    }
    for (ast::Attr attr : decl.attributes().children_of_type<ast::Attr>()) {
      string_view id = attr.identifier().str();
      if (id == "vertex") {
        entry_point_type = EntryPointType::VERT;
      }
      else if (id == "fragment") {
        entry_point_type = EntryPointType::FRAG;
      }
      else if (id == "compute") {
        entry_point_type = EntryPointType::COMP;
      }
    }
  }

  SymbolFunction(
      SymbolScope *parent, Token tok, SymbolClass *return_type, const string &id, Type fn_type)
      : SymbolScope(parent, tok, id, FUNCTION),
        return_type(return_type),
        fn_type(fn_type),
        entry_point_type(EntryPointType::NONE)
  {
  }

  /* Can be null if in global space. */
  SymbolClass *parent_class()
  {
    return this->parent->as_class();
  }

  bool is_entry_point() const
  {
    return entry_point_type != SymbolFunction::EntryPointType::NONE;
  }

  /* To be called on the first registered overload. */
  Result<SymbolFunction *> lookup_overload(ast::IdQualified id,
                                           const SymbolTable &table,
                                           const vector<SymbolClass *> &arg_types);

  void add_overload(SymbolFunction *fn);
  void add_argument(SymbolClass *type, ast::Expr default_value = {});

  /* Return true if the given argument types are compatible */
  bool argument_matches(const SymbolTable &table, const vector<SymbolClass *> &param_types) const;

  static Result<vector<SymbolClass *>> to_arg_types(const SymbolTable &table,
                                                    const SymbolScope &scope,
                                                    ast::FuncParamList list);
  static Result<vector<SymbolClass *>> to_arg_types(const SymbolTable &table,
                                                    const SymbolScope &scope,
                                                    ast::FuncArgList list);
};

}  // namespace bsl
