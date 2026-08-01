/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "type_checker.hh"

namespace bsl {

Result<SymbolClass *> SymbolTable::expr_type_analysis(const SymbolScope &scope,
                                                      ast::Expr expr) const
{
  assert(expr.is_valid());
  return ExpressionTypeParser(this, &scope, expr.child_first()).eval();
}

}  // namespace bsl
