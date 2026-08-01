/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "ast.hh"
#include "diagnostic.hh"
#include "symbol_class.hh"
#include "symbol_function.hh"
#include "symbol_variable.hh"

#include <unordered_map>

namespace bsl {

struct StringPair {
  string str;
  string str_debug;
};

struct SymbolVariable;
struct SymbolFunction;
struct SymbolClass;
struct SymbolScope;

/* Left is nullptr for unary operators. */
using OperatorKey = tuple<SymbolClass * /* Left */, TokenType, SymbolClass * /* Right */>;

struct OperatorKeyHasher {
  template<class T> static void hash_combine(std::size_t &seed, const T &v)
  {
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  }

  std::size_t operator()(const OperatorKey &t) const
  {
    std::size_t seed = 0;
    hash_combine(seed, std::get<0>(t));
    hash_combine(seed, std::get<1>(t));
    hash_combine(seed, std::get<2>(t));
    return seed;
  }
};

struct SymbolTable {
  using OperatorMap = unordered_map<OperatorKey, SymbolFunction *, OperatorKeyHasher>;

  static constexpr string err_symbol = "ERROR_SYMBOL";

  /* TODO(fclem): Real memory arena. */
  template<typename T> struct Allocator {
   private:
    vector<unique_ptr<T>> arena;

   public:
    template<typename... Args> T *alloc(Args &&...args)
    {
      arena.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
      return arena.back().get();
    }
  };

  Allocator<SymbolVariable> var_arena;
  Allocator<SymbolFunction> fun_arena;
  Allocator<SymbolClass> cls_arena;
  Allocator<SymbolScope> scp_arena;
  Allocator<SymbolClassTemplate> tmp_cls_arena;
  Allocator<SymbolFunctionTemplate> tmp_fun_arena;

  SymbolClass *float_cls;
  SymbolClass *int_cls;
  SymbolClass *uint_cls;
  SymbolClass *str_cls;
  SymbolClass *err_cls;
  SymbolClass *bool_cls;
  SymbolClass *void_cls;
  SymbolClass *float2_cls;
  SymbolClass *float3_cls;
  SymbolClass *float4_cls;
  SymbolClass *packed_float2_cls;
  SymbolClass *packed_float3_cls;
  SymbolClass *packed_float4_cls;

  OperatorMap operators;

  unordered_map<int, string> image_formats;

  unordered_map<TokenType, const char *> unary_type_to_id;
  unordered_map<TokenType, const char *> binary_type_to_id;

  template<typename SymbolT> SymbolT *alloc(SymbolT &&sym);

  SymbolScope *root = nullptr;

  SymbolTable(ParserBase &builtin_parser);

  void parse(ast::LocalScope node, ErrorHandler &err_handler);

  static Result<StringPair> mangle_identifier(ast::TemplateArgList args,
                                              ast::TemplateParamList list,
                                              const SymbolScope &scope);
  Result<StringPair> mangle_identifier(const SymbolFunctionTemplate &tmp,
                                       ast::FuncParamList list,
                                       const SymbolScope &scope) const;

  Result<SymbolClass *> expr_type_analysis(const SymbolScope &scope, ast::Expr expr) const;

  Result<SymbolClass *> resolve_auto_type(SymbolScope &scope, ast::Declarator decl) const;

  static Result<int64_t> evaluate_constexpr(const SymbolScope &scope, ast::Node start);

  static Result<string> expr_to_string(const SymbolScope &scope,
                                       ast::Node start,
                                       int &node_count,
                                       bool allow_compilation_constant = false);

  SymbolClass *get_literal_type(string_view lit) const;
  bool is_convertible(const SymbolClass *from, const SymbolClass *to) const;

 private:
  struct BuiltinType {
    /* "int", "int2", ... */
    string name;
    /* "int", "uint", "float", "bool", ... */
    string base;
    /* 1 (scalar), 2, 3, 4 */
    int size;
  };

  struct BuiltinOp {
    string left;
    TokenType op;
    string right;
    string result;
  };

  struct BuiltinFunc {
    string return_type;
    string id;
    vector<string> arg_types;
  };

  void register_builtins(ast::LocalScope node);
  static vector<BuiltinType> generate_builtin_types();
  static vector<BuiltinOp> generate_all_operators(const vector<BuiltinType> &types);
  static vector<BuiltinFunc> generate_all_constructors(const vector<BuiltinType> &types);
  static vector<BuiltinFunc> generate_all_builtin_functions();
};

}  // namespace bsl
