/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "symbol_class.hh"

namespace bsl {

using namespace blender::gpu::shader::parser;

enum ExprFlag : uint8_t {
  None = 0,
  Constant,
  Constexpr,
};

struct ExpressionResult {
  SymbolClass *type;
  /* Value if constexpr. */
  ConstexprValue value = 0;
  /* Array dimension of the current temporary. */
  uint8_t array_dim = 0;

  ExprFlag flags = ExprFlag::None;
  bool is_temporary = false;

  ExpressionResult(SymbolVariable *var)
      : type(var->type),
        value(var->value),
        array_dim(var->array_dimensions),
        flags(ExprFlag((var->is_constexpr ? ExprFlag::Constexpr : ExprFlag::None) |
                       (var->is_const ? ExprFlag::Constant : ExprFlag::None)))
  {
  }

  ExpressionResult(SymbolClass *cls, ExprFlag flags = ExprFlag::None)
      : type(cls), flags(flags), is_temporary(true)
  {
  }

  ExpressionResult(SymbolClass *cls, ConstexprValue value)
      : type(cls),
        value(value),
        flags(ExprFlag(ExprFlag::Constexpr | ExprFlag::Constant)),
        is_temporary(true)
  {
  }

  bool is_constexpr() const
  {
    return flags & ExprFlag::Constexpr;
  }
  bool is_constant() const
  {
    return flags & ExprFlag::Constant;
  }
};

} /* namespace bsl */
