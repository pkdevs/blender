/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "symbol.hh"

namespace blender::gpu::shader::parser {

/* Generates all scalar and vector types (e.g. int, int2, int3, int4). */
vector<SymbolTable::BuiltinType> SymbolTable::generate_builtin_types()
{
  vector<BuiltinType> types;
  vector<string> bases = {"int", "uint", "float", "bool"};
  for (const auto &base : bases) {
    types.emplace_back(base, base, 1);  // Scalar
    for (int size = 2; size <= 4; ++size) {
      types.emplace_back(base + to_string(size), base, size);
    }
  }
  return types;
}

static bool is_numeric(const string &base)
{
  return base != "bool";
}

static bool is_integer(const string &base)
{
  return base == "int" || base == "uint";
}

static int get_rank(const string &base)
{
  if (base == "int") {
    return 1;
  }
  if (base == "uint") {
    return 2;
  }
  if (base == "float") {
    return 3;
  }
  if (base == "double") {
    return 4;
  }
  return 0;
}

static string get_promoted_base(const string &b1, const string &b2)
{
  return (get_rank(b1) >= get_rank(b2)) ? b1 : b2;
}

static string make_type_name(const string &base, int size)
{
  if (size == 1) {
    return base;
  }
  return base + to_string(size);
}

/* Generates every single legal BSL operator overload dynamically. */
vector<SymbolTable::BuiltinOp> SymbolTable::generate_all_operators(
    const vector<BuiltinType> &types)
{
  vector<BuiltinOp> ops;

  /* Unary Operators */
  for (const auto &t : types) {
    if (is_numeric(t.base)) {
      ops.emplace_back("", Plus, t.name, t.name);
      ops.emplace_back("", Minus, t.name, t.name);
    }
    if (is_integer(t.base)) {
      ops.emplace_back("", BitwiseNot, t.name, t.name);
    }
    if (t.name == "bool") {
      ops.emplace_back("", Not, "bool", "bool");
    }
  }

  /* Binary Operators */
  for (const auto &t1 : types) {
    for (const auto &t2 : types) {
      int target_size = 0;
      if (t1.size == t2.size) {
        target_size = t1.size;
      }
      else if (t1.size == 1) {
        /* Scalar-Vector Promotion. */
        target_size = t2.size;
      }
      else if (t2.size == 1) {
        /* Vector-Scalar Promotion. */
        target_size = t1.size;
      }
      else {
        /* Size mismatch (e.g., float2 with float3). */
        continue;
      }

      /* Arithmetic (+, -, *, /). */
      if (is_numeric(t1.base) && is_numeric(t2.base)) {
        string res_base = get_promoted_base(t1.base, t2.base);
        string res_type = make_type_name(res_base, target_size);
        ops.emplace_back(t1.name, Plus, t2.name, res_type);
        ops.emplace_back(t1.name, Minus, t2.name, res_type);
        ops.emplace_back(t1.name, Multiply, t2.name, res_type);
        ops.emplace_back(t1.name, Divide, t2.name, res_type);
      }

      /* Modulo (%). */
      if (is_integer(t1.base) && is_integer(t2.base)) {
        string res_base = get_promoted_base(t1.base, t2.base);
        string res_type = make_type_name(res_base, target_size);
        ops.emplace_back(t1.name, Modulo, t2.name, res_type);
      }

      /* Bitwise Logic (&, |, ^). */
      if (is_integer(t1.base) && is_integer(t2.base)) {
        string res_base = get_promoted_base(t1.base, t2.base);
        string res_type = make_type_name(res_base, target_size);
        ops.emplace_back(t1.name, And, t2.name, res_type);
        ops.emplace_back(t1.name, Or, t2.name, res_type);
        ops.emplace_back(t1.name, Xor, t2.name, res_type);
      }

      /* Bitwise Shifts (<<, >>). */
      /* L can be scalar/vector, R can be scalar or matching size vector. */
      if (is_integer(t1.base) && is_integer(t2.base)) {
        if (t2.size == 1 || t2.size == t1.size) {
          /* TODO */
          // ops.emplace_back(t1.name, LeftShift, t2.name, t1.name);
          // ops.emplace_back(t1.name, RightShift, t2.name, t1.name);
        }
      }

      /* Equality (==, !=) */
      /* Do not support vector-vector comparisons for MSL compatibility. */
      if (t1.size == 1 && t2.size == 1) {
        if (t1.base == "bool" && t2.base == "bool") {
          ops.emplace_back(t1.name, Equal, t2.name, "bool");
          ops.emplace_back(t1.name, NotEqual, t2.name, "bool");
        }
      }
    }
  }

  /* Relational (<, >, <=, >=).
   * Only valid on scalar numeric types. */
  for (const auto &t1 : types) {
    if (t1.size != 1 || !is_numeric(t1.base)) {
      continue;
    }
    for (const auto &t2 : types) {
      if (t2.size != 1 || !is_numeric(t2.base)) {
        continue;
      }
      ops.emplace_back(t1.name, LThan, t2.name, "bool");
      ops.emplace_back(t1.name, GThan, t2.name, "bool");
      ops.emplace_back(t1.name, LEqual, t2.name, "bool");
      ops.emplace_back(t1.name, GEqual, t2.name, "bool");
    }
  }

  /* Logical Operators (&&, ||).
   * Only valid on scalar bool. */
  ops.emplace_back("bool", LogicalAnd, "bool", "bool");
  ops.emplace_back("bool", LogicalOr, "bool", "bool");

  return ops;
}

}  // namespace blender::gpu::shader::parser
