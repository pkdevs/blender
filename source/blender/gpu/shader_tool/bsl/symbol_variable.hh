/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "resource.hh"
#include "source_location.hh"

#include <variant>

namespace bsl {

struct SymbolScope;
struct SymbolClass;
struct SymbolTable;

struct Symbol {
  string identifier;
  /* Unresolved identifier */
  string original;
  SourceLocation loc;
  SymbolScope *parent = nullptr;
};

using ConstexprValue = std::variant<bool, int, uint, float>;

template<typename T> T value_as(const ConstexprValue &v)
{
  return std::visit([](auto &&arg) -> T { return static_cast<T>(arg); }, v);
}

struct SymbolVariable : Symbol {
  SymbolClass *type = nullptr; /* TODO rename to type */

  /* Valid if the variable is a local reference.
   * The variable will be substituted to this statement. */
  ast::Expr reference_value = {};

  /* Value if constexpr. */
  ConstexprValue value = 0;

  ResourceType res_type = ResourceType::NONE;

  int array_dimensions = 0;
  /* Number of array elements in the declaration.
   * 0 if not array.
   * -1 if not known at compile time. */
  int array_elements = 0;
  /* In bytes. From scope start, taking alignment into account. */
  int offset = 0;

  bool is_static = false;
  bool is_error = false;
  bool is_constexpr = false;
  bool is_const = false;
  bool is_compilation_const = false;

  SymbolVariable(SymbolScope *parent,
                 SymbolClass *type,
                 ast::Declarator decl,
                 const SymbolTable &table);

  SymbolVariable(SymbolScope *parent, SymbolClass *type, ast::EnumValue decl)
      : Symbol(string(decl.identifier().str()),
               string(decl.identifier().str()),
               decl.front(),
               parent),
        type(type),
        is_static(true)
  {
  }

  SymbolVariable(SymbolScope *parent, SymbolClass *type, Token id, const string &str)
      : Symbol(str, str, id, parent), type(type)
  {
  }

  void set_offset(bool is_union, int &offset);

  /* Can be null if in global space. */
  SymbolClass *parent_class();

  string value_str() const;
};

}  // namespace bsl
