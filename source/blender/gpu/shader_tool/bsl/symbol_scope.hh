/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "ast.hh"
#include "diagnostic.hh"
#include "source_location.hh"
#include "symbol_variable.hh"

#include <unordered_map>

namespace bsl {

struct SymbolParser;
struct SymbolTable;

struct SymbolFunction;
struct SymbolClass;

template<typename SymbolT>
struct SymbolMap : unordered_map<string, pair<SourceLocation, SymbolT *>> {
  pair<SymbolT *, bool> emplace(SymbolT *sym)
  {
    auto [it, success] = this->try_emplace(sym->identifier,
                                           pair<SourceLocation, SymbolT *>(sym->loc, sym));
    return pair<SymbolT *, bool>(it->second.second, success);
  }

  pair<SymbolT *, bool> emplace(const string &id, SymbolT *sym)
  {
    auto [it, success] = this->try_emplace(id, pair<SourceLocation, SymbolT *>(sym->loc, sym));
    return pair<SymbolT *, bool>(it->second.second, success);
  }

  pair<SymbolT *, bool> emplace(const string &id, SymbolT *sym, SourceLocation loc)
  {
    auto [it, success] = this->try_emplace(id, pair<SourceLocation, SymbolT *>(loc, sym));
    return pair<SymbolT *, bool>(it->second.second, success);
  }

  SymbolT *operator[](const string &k)
  {

    return this->find(k)->second.second;
  }
};

struct SymbolScope : Symbol {
  friend SymbolParser;

  /* Namespaces or nested blocks. */
  unordered_map<string, SymbolScope *> scopes;

  SymbolMap<SymbolVariable> variables;
  SymbolMap<SymbolFunction> functions;
  SymbolMap<SymbolClass> classes;

  /* Function scope act as anonymous scopes but still have identifier. */
  enum Type { FUNCTION, CLASS, NAMESPACE, LOCAL, ROOT } type = ROOT;

  /* Root constructor. */
  SymbolScope(ast::LocalScope decl) : Symbol("", "", decl.back(), nullptr) {}

  SymbolScope(SymbolScope *parent, Token tok, const string &id, Type type)
      : Symbol(id, id, tok, parent), type(type)
  {
  }

  SymbolScope(SymbolScope *parent, ast::LocalScope decl)
      : SymbolScope(parent, decl.front(), parent->unique_id(), LOCAL)
  {
  }

  SymbolScope(SymbolScope *parent, ast::Id id, Type type)
      : SymbolScope(parent, id.front(), string(id.str()), type)
  {
  }

  SymbolVariable *lookup_variable(const SymbolTable &table, ast::IdQualified id) const;
  Result<SymbolFunction *> lookup_function(const SymbolTable &table, ast::IdQualified id) const;
  template<typename ArgOrParamList>
  Result<SymbolFunction *> lookup_function(const SymbolTable &table,
                                           ast::IdQualified id,
                                           ArgOrParamList params,
                                           const SymbolScope &arguments_scope) const;
  Result<SymbolClass *> lookup_class(const SymbolTable &table, ast::IdQualified id) const;

  Result<SymbolClass *> lookup_class(const SymbolTable &table, ast::Id id, ast::Id last) const;

  SymbolVariable *lookup_variable(string id) const;
  SymbolFunction *lookup_function(string id) const;
  SymbolClass *lookup_class(string id) const;

  /* Doesn't match last template in identifier. */
  SymbolFunction *lookup_function_base(const SymbolTable &table, ast::IdQualified id) const;
  SymbolClass *lookup_class_base(const SymbolTable &table, ast::IdQualified id) const;

  void print() const;

  const SymbolScope *root_scope() const;
  SymbolScope *root_scope();

  /* Can be null if this is not a class. */
  SymbolClass *as_class();
  /* Can be null if this is not a function. */
  SymbolFunction *as_function();

  /* Return the function containing this scope or the scope itself if it is a function.
   * Return nullptr if this scope is not inside a function. */
  SymbolFunction *parent_function();

  /* Returns true if the function is an overload or redefines the symbol. */
  bool function_emplace(SymbolFunction *fn, bool builtin = false);

  vector<SymbolVariable *> non_static_variables_in_declaration_order() const;

 private:
  template<typename T>
  T *lookup_generic(const SymbolTable &table,
                    ast::IdQualified id,
                    const SourceLocation &loc) const;
  template<typename T>
  T *lookup_generic_nested(const SymbolTable &table,
                           ast::Id id,
                           ast::Id last,
                           const SourceLocation &loc) const;

  int id = 0;
  /* Create a unique, non-reachable key.
   * Identifiers cannot start with digits.
   * This makes a scope impenetrable (can only leave the scope) during traversal. */
  string unique_id()
  {
    return to_string(id++);
  }
};

}  // namespace bsl
