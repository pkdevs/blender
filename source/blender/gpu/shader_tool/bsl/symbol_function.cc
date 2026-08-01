/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "symbol_function.hh"
#include "symbol_table.hh"

namespace bsl {

using namespace blender::gpu::shader::parser::ast;

Result<vector<SymbolClass *>> SymbolFunction::to_arg_types(const SymbolTable &table,
                                                           const SymbolScope &scope,
                                                           FuncParamList list)
{
  std::optional<AstNodeException> error;
  vector<SymbolClass *> arg_types;
  for (Expr expr : list.children_of_type<Expr>()) {
    arg_types.emplace_back(table.expr_type_analysis(scope, expr).unwrap(error));
  }
  return {arg_types, error};
}

Result<vector<SymbolClass *>> SymbolFunction::to_arg_types(const SymbolTable & /*table*/,
                                                           const SymbolScope &scope,
                                                           FuncArgList list)
{
  vector<SymbolClass *> arg_types;
  for (FuncArg arg : list.children_of_type<FuncArg>()) {
    auto [cls, _] = scope.lookup_class(arg.type().id());
    arg_types.emplace_back(cls);
  }
  return {arg_types, std::nullopt};
}

static string to_str(const vector<SymbolClass *> &types)
{
  string parameters = types.empty() ? ", " : "";
  for (SymbolClass *cls : types) {
    parameters += ", " + cls->identifier + "";
  }
  return "(" + parameters.substr(2) + ")";
}

Result<SymbolFunction *> SymbolFunction::lookup_overload(IdQualified id,
                                                         const SymbolTable &table,
                                                         const vector<SymbolClass *> &arg_types)
{
  for (SymbolFunction *fn = this; fn; fn = fn->overload_next) {
    if (fn->is_complete && fn->argument_matches(table, arg_types)) {
      return {fn, {}};
    }
  }
  /* List all candidates inside the error message. */
  string candidates;
  for (const SymbolFunction *fn = this; fn; fn = fn->overload_next) {
    if (fn->is_complete) {
      candidates += "\ncandidate: " + fn->return_type->original + " " + fn->original +
                    to_str(fn->arg_types);
    }
  }

  return {root_scope()->lookup_function(SymbolTable::err_symbol),
          AstNodeException(id, Diag::NoMatchingOverload, original, to_str(arg_types), candidates)};
}

void SymbolFunction::add_overload(SymbolFunction *fn)
{
  fn->overload_next = this->overload_next;
  this->overload_next = fn;
}

void SymbolFunction::add_argument(SymbolClass *type, Expr default_value)
{
  this->arg_types.emplace_back(type);
  this->arg_defaults.emplace_back(default_value);
}

bool SymbolFunction::argument_matches(const SymbolTable &table,
                                      const vector<SymbolClass *> &param_types) const
{
  /* First fast filter. */
  if (arg_types == param_types) {
    return true;
  }

  /* Early out if there is more parameters than arguments. */
  if (param_types.size() > arg_types.size()) {
    return false;
  }

  for (int i = 0; i < param_types.size(); ++i) {
    if (arg_types[i] != param_types[i] &&
        !table.is_convertible(param_types[i], arg_types[i], is_builtin_ctor))
    {
      return false;
    }
  }

  /* Check if remaining parameters are optional. */
  for (int i = param_types.size(); i < arg_types.size(); ++i) {
    if (!arg_defaults[i].is_valid()) {
      return false;
    }
  }

  return true;
}
}  // namespace bsl
