/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "symbol_table.hh"

namespace bsl {

/* Generates all scalar and vector types (e.g. int, int2, int3, int4). */
vector<SymbolTable::BuiltinType> SymbolTable::generate_builtin_types()
{
  vector<BuiltinType> types{
      {char_cls, char_cls},    {short_cls, short_cls},    {int_cls, int_cls},
      {uchar_cls, uchar_cls},  {ushort_cls, ushort_cls},  {uint_cls, uint_cls},
      {half_cls, half_cls},    {float_cls, float_cls},

      {char2_cls, char_cls},   {short2_cls, short_cls},   {int2_cls, int_cls},
      {uchar2_cls, uchar_cls}, {ushort2_cls, ushort_cls}, {uint2_cls, uint_cls},
      {half2_cls, half_cls},   {float2_cls, float_cls},

      {char3_cls, char_cls},   {short3_cls, short_cls},   {int3_cls, int_cls},
      {uchar3_cls, uchar_cls}, {ushort3_cls, ushort_cls}, {uint3_cls, uint_cls},
      {half3_cls, half_cls},   {float3_cls, float_cls},

      {char4_cls, char_cls},   {short4_cls, short_cls},   {int4_cls, int_cls},
      {uchar4_cls, uchar_cls}, {ushort4_cls, ushort_cls}, {uint4_cls, uint_cls},
      {half4_cls, half_cls},   {float4_cls, float_cls},
  };
  return types;
}

bool SymbolTable::is_numeric(const SymbolClass *base)
{
  return base != bool_cls;
}

bool SymbolTable::is_integer(const SymbolClass *base)
{
  return base != float_cls;
}

int SymbolTable::get_rank(const SymbolClass *base)
{
  if (base == char_cls) {
    return 1;
  }
  if (base == uchar_cls) {
    return 2;
  }
  if (base == short_cls) {
    return 3;
  }
  if (base == ushort_cls) {
    return 4;
  }
  if (base == int_cls) {
    return 5;
  }
  if (base == uint_cls) {
    return 6;
  }
  if (base == float_cls) {
    return 7;
  }
  return 0;
}

SymbolClass *SymbolTable::get_promoted_base(SymbolClass *b1, SymbolClass *b2)
{
  return (get_rank(b1) >= get_rank(b2)) ? b1 : b2;
}

SymbolClass *SymbolTable::make_type(SymbolClass *base, int size)
{
  /* Size 1 corresponds to the scalar base type itself. */
  if (size == 1) {
    return base;
  }

  if (base == float_cls) {
    switch (size) {
      case 2:
        return float2_cls;
      case 3:
        return float3_cls;
      case 4:
        return float4_cls;
      default:
        break;
    }
  }
  else if (base == int_cls) {
    switch (size) {
      case 2:
        return int2_cls;
      case 3:
        return int3_cls;
      case 4:
        return int4_cls;
      default:
        break;
    }
  }
  else if (base == uint_cls) {
    switch (size) {
      case 2:
        return uint2_cls;
      case 3:
        return uint3_cls;
      case 4:
        return uint4_cls;
      default:
        break;
    }
  }
  else if (base == bool_cls) {
    switch (size) {
      case 2:
        return bool2_cls;
      case 3:
        return bool3_cls;
      case 4:
        return bool4_cls;
      default:
        break;
    }
  }
  else if (base == half_cls) {
    switch (size) {
      case 2:
        return half2_cls;
      case 3:
        return half3_cls;
      case 4:
        return half4_cls;
      default:
        break;
    }
  }
  else if (base == char_cls) {
    switch (size) {
      case 2:
        return char2_cls;
      case 3:
        return char3_cls;
      case 4:
        return char4_cls;
      default:
        break;
    }
  }
  else if (base == uchar_cls) {
    switch (size) {
      case 2:
        return uchar2_cls;
      case 3:
        return uchar3_cls;
      case 4:
        return uchar4_cls;
      default:
        break;
    }
  }
  else if (base == short_cls) {
    switch (size) {
      case 2:
        return short2_cls;
      case 3:
        return short3_cls;
      case 4:
        return short4_cls;
      default:
        break;
    }
  }
  else if (base == ushort_cls) {
    switch (size) {
      case 2:
        return ushort2_cls;
      case 3:
        return ushort3_cls;
      case 4:
        return ushort4_cls;
      default:
        break;
    }
  }
  else if (base == float2_cls) {
    switch (size) {
      case 2:
        return float2x2_cls;
      case 3:
        return float3x2_cls;
      case 4:
        return float4x2_cls;
      default:
        break;
    }
  }
  else if (base == float3_cls) {
    switch (size) {
      case 2:
        return float2x3_cls;
      case 3:
        return float3x3_cls;
      case 4:
        return float4x3_cls;
      default:
        break;
    }
  }
  else if (base == float4_cls) {
    switch (size) {
      case 2:
        return float2x4_cls;
      case 3:
        return float3x4_cls;
      case 4:
        return float4x4_cls;
      default:
        break;
    }
  }
  return nullptr;
}

void SymbolTable::add_operator(
    SourceLocation loc, SymbolClass *left, TokenType op, SymbolClass *right, SymbolClass *ret)
{
  string id((!left ? unary_type_to_id : binary_type_to_id).find(op)->second);
  SymbolFunction *sym = fun_arena.alloc(root, loc.tok, ret, id, SymbolFunction::GLOBAL);
  operators.emplace(OperatorKey(left, op, right), sym);
}

/* Generates every single legal BSL operator overload dynamically. */
void SymbolTable::generate_builtin_operators(SourceLocation loc, const vector<BuiltinType> &types)
{
  /* Unary Operators */
  for (const auto &t : types) {
    if (is_numeric(t.base)) {
      add_operator(loc, nullptr, Plus, t.type, t.type);
      add_operator(loc, nullptr, Minus, t.type, t.type);
      add_operator(loc, nullptr, Increment, t.type, t.type);
      add_operator(loc, nullptr, Decrement, t.type, t.type);
    }
    if (is_integer(t.base)) {
      add_operator(loc, nullptr, BitwiseNot, t.type, t.type);
    }
  }

  add_operator(loc, nullptr, Not, bool_cls, bool_cls);
  add_operator(loc, nullptr, Not, bool32_t_cls, bool_cls);

  /* Binary Operators */
  for (const auto &t1 : types) {
    for (const auto &t2 : types) {
      int target_size = 0;
      if (t1.type->comp_len == t2.type->comp_len) {
        target_size = t1.type->comp_len;
      }
      else if (t1.type->comp_len == 1) {
        /* Scalar-Vector Promotion. */
        target_size = t2.type->comp_len;
      }
      else if (t2.type->comp_len == 1) {
        /* Vector-Scalar Promotion. */
        target_size = t1.type->comp_len;
      }
      else {
        /* Size mismatch (e.g., float2 with float3). */
        continue;
      }

      /* Arithmetic (+, -, *, /). */
      if (is_numeric(t1.base) && is_numeric(t2.base)) {
        SymbolClass *res_base = get_promoted_base(t1.base, t2.base);
        SymbolClass *res_type = make_type(res_base, target_size);
        add_operator(loc, t1.type, Plus, t2.type, res_type);
        add_operator(loc, t1.type, Minus, t2.type, res_type);
        add_operator(loc, t1.type, Multiply, t2.type, res_type);
        add_operator(loc, t1.type, Divide, t2.type, res_type);
      }

      /* Modulo (%). */
      if (is_integer(t1.base) && is_integer(t2.base)) {
        SymbolClass *res_base = get_promoted_base(t1.base, t2.base);
        SymbolClass *res_type = make_type(res_base, target_size);
        add_operator(loc, t1.type, Modulo, t2.type, res_type);
      }

      /* Bitwise Logic (&, |, ^). */
      if (is_integer(t1.base) && is_integer(t2.base)) {
        SymbolClass *res_base = get_promoted_base(t1.base, t2.base);
        SymbolClass *res_type = make_type(res_base, target_size);
        add_operator(loc, t1.type, And, t2.type, res_type);
        add_operator(loc, t1.type, Or, t2.type, res_type);
        add_operator(loc, t1.type, Xor, t2.type, res_type);
      }

      /* Bitwise Shifts (<<, >>). */
      /* L can be scalar/vector, R can be scalar or matching size vector. */
      if (is_integer(t1.base) && is_integer(t2.base)) {
        if (t2.type->comp_len == 1 || t2.type->comp_len == t1.type->comp_len) {
          add_operator(loc, t1.type, LShift, t2.type, t1.type);
          add_operator(loc, t1.type, RShift, t2.type, t1.type);
        }
      }

      /* Equality (==, !=) */
      /* Do not support vector-vector comparisons for MSL compatibility. */
      if (t1.type->comp_len == 1 && t2.type->comp_len == 1) {
        if (t1.base == t2.base) {
          add_operator(loc, t1.type, Equal, t2.type, bool_cls);
          add_operator(loc, t1.type, NotEqual, t2.type, bool_cls);
        }
      }
    }
  }

  /* Relational (<, >, <=, >=).
   * Only valid on scalar numeric types. */
  for (const auto &t1 : types) {
    if (t1.type->comp_len != 1 || !is_numeric(t1.base)) {
      continue;
    }
    for (const auto &t2 : types) {
      if (t2.type->comp_len != 1 || !is_numeric(t2.base)) {
        continue;
      }
      add_operator(loc, t1.type, LThan, t2.type, bool_cls);
      add_operator(loc, t1.type, GThan, t2.type, bool_cls);
      add_operator(loc, t1.type, LEqual, t2.type, bool_cls);
      add_operator(loc, t1.type, GEqual, t2.type, bool_cls);
    }
  }

  /* Logical Operators (&&, ||).
   * Only valid on scalar bool. */
  add_operator(loc, bool_cls, LogicalAnd, bool_cls, bool_cls);
  add_operator(loc, bool_cls, LogicalOr, bool_cls, bool_cls);

  /* Add packed_float3 operators in order to support most auto casts. */
  add_operator(loc, nullptr, Plus, packed_float3_cls, packed_float3_cls);
  add_operator(loc, nullptr, Minus, packed_float3_cls, packed_float3_cls);
}

}  // namespace bsl
