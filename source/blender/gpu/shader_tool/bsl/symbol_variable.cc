/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "symbol_table.hh"
#include "symbol_variable.hh"

namespace bsl {

using namespace blender::gpu::shader::parser::ast;

SymbolVariable::SymbolVariable(SymbolScope *parent, SymbolClass *type, Declarator decl)
    : Symbol(
          string(decl.identifier().str()), string(decl.identifier().str()), decl.front(), parent),
      type(type),
      array_dimensions(decl.array().dimensions()),
      is_static(decl.type().is_static()),
      is_compilation_const(AttrList(decl.type().prev()).contains_attr("compilation_constant"))
{
  if (ast::Const constant = decl.type().constant(); constant.is_valid()) {
    is_constexpr = constant.front() == Constexpr;
    is_const = true;
  }
  if (ArrayDecl array = decl.array(); array.is_valid()) {
    Subscript sub = array.sub();
    array_elements = 1;
    for (int i = 0; i < array_dimensions; ++i, sub = sub.next()) {
      Node node = sub.expr().child_first();
      if (!node.is_valid()) {
        array_elements = -1;
        break;
      }
      auto [value, err] = SymbolTable::evaluate_constexpr(*parent, node);
      if (err) {
        array_elements = -1;
        break;
      }
      array_elements *= value;
    }
  }
}

void SymbolVariable::set_offset(bool is_union, int &offset)
{
  if (is_union) {
    this->offset = 0;
  }
  else {
    this->offset = pad(offset, this->type->align);
    offset = this->offset;
    offset += this->type->size;
  }
}

/* Can be null if in global space. */
SymbolClass *SymbolVariable::parent_class()
{
  return this->parent->as_class();
}

string SymbolVariable::value_str() const
{
  assert(is_constexpr);
  if (type->identifier == "bool") {
    return (!!value) ? "true" : "false";
  }
  return to_string(value);
}

}  // namespace bsl
