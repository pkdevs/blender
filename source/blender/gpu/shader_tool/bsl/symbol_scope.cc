/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "symbol_class.hh"
#include "symbol_function.hh"
#include "symbol_scope.hh"
#include "symbol_table.hh"

namespace bsl {

using namespace blender::gpu::shader::parser::ast;

template<typename T, typename AstNodeT>
static Result<T *> resolve_template_instantiation(T *sym, AstNodeT id, const SymbolScope &scope)
{
  if (TemplateParamList param = id.template_params();
      param.is_valid() && sym && sym->template_data)
  {
    auto [inst, _, err] = sym->template_data->lookup_inst(param, scope);
    return {inst, err};
  }
  return {sym, std::nullopt};
}

#if 0 /* For debugging. */
#  define LOOKUP_LOG(a) cerr << a;
#else
#  define LOOKUP_LOG(a)
#endif

Result<SymbolFunction *> SymbolScope::lookup_function(IdQualified id) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  SymbolFunction *func = lookup_generic<SymbolFunction>(id, id.front());
  return resolve_template_instantiation(func, id, *this);
}

template<typename ArgOrParamList>
Result<SymbolFunction *> SymbolScope::lookup_function(IdQualified id,
                                                      ArgOrParamList params,
                                                      const SymbolTable &table,
                                                      const SymbolScope &arguments_scope) const
{
  auto [func, err] = lookup_function(id);
  if (func->is_error) {
    /* Try to resolve type constructors (only for builtins for now). */
    if (auto [cls, err_cls] = lookup_class(id); !cls->is_error && cls && cls->is_builtin) {
      return {root_scope()->lookup_function(cls->identifier), {}};
    }
    return {func, err ? err : AstNodeException{id, Diag::UnknownFunction}};
  }

  auto arg_types = SymbolFunction::to_arg_types(table, arguments_scope, params).unwrap(err);
  func = func->lookup_overload(id, table, arg_types).unwrap(err);
  return {func, err};
}

template Result<SymbolFunction *> SymbolScope::lookup_function(
    IdQualified id,
    FuncParamList params,
    const SymbolTable &table,
    const SymbolScope &arguments_scope) const;
template Result<SymbolFunction *> SymbolScope::lookup_function(
    IdQualified id,
    FuncArgList params,
    const SymbolTable &table,
    const SymbolScope &arguments_scope) const;

Result<SymbolClass *> SymbolScope::lookup_class(IdQualified id) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  SymbolClass *cls = lookup_generic<SymbolClass>(id, id.front());
  return resolve_template_instantiation(cls, id, *this);
}

Result<SymbolClass *> SymbolScope::lookup_class(Id id, Id last) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  SymbolClass *cls = lookup_generic_nested<SymbolClass>(id, last, id.front());
  return resolve_template_instantiation(cls, last, *this);
}

SymbolFunction *SymbolScope::lookup_function_base(IdQualified id) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  return lookup_generic<SymbolFunction>(id, id.front());
}

SymbolClass *SymbolScope::lookup_class_base(IdQualified id) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  return lookup_generic<SymbolClass>(id, id.front());
}

SymbolVariable *SymbolScope::lookup_variable(IdQualified id) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  return lookup_generic<SymbolVariable>(id, id.front());
}

const SymbolScope *SymbolScope::root_scope() const
{
  const SymbolScope *scope = this;
  while (scope->parent) {
    scope = scope->parent;
  }
  return scope;
}

SymbolScope *SymbolScope::root_scope()
{
  SymbolScope *scope = this;
  while (scope->parent) {
    scope = scope->parent;
  }
  return scope;
}

/* Helper for debug print. */
template<typename T> const char *get_symbol_type_name()
{
  if constexpr (std::is_same_v<T, SymbolFunction>) {
    return "Function";
  }
  else if constexpr (std::is_same_v<T, SymbolClass>) {
    return "Class";
  }
  else if constexpr (std::is_same_v<T, SymbolVariable>) {
    return "Variable";
  }
  else {
    return "Unknown";
  }
}

template<typename T>
T *SymbolScope::lookup_generic(IdQualified id, const SourceLocation &loc) const
{
  assert(id.is_valid());

  LOOKUP_LOG("[Lookup] Looking up " << get_symbol_type_name<T>() << " '" << id.str()
                                    << "' in scope '" << identifier << "'\n")

  /* Bubble up immediately if it's a global lookup. */
  if (id.is_global() && parent) {
    LOOKUP_LOG("[Lookup] Global lookup: Bubbling up to parent scope " << parent->identifier
                                                                      << "\n")
    return parent->lookup_generic<T>(id, loc);
  }

  /* Try to match within this scope. */
  if (auto v = lookup_generic_nested<T>(id.namespace_start(), id.child_last(NodeType::Id), loc)) {
    LOOKUP_LOG("[Lookup] Symbol found locally '" << identifier << "'.\n")
    return v;
  }

  /* Delegate to parent scope if not found locally. */
  if (parent) {
    LOOKUP_LOG("[Lookup] Not found locally. Delegating to parent scope\n")
    return parent->lookup_generic<T>(id, loc);
  }

  /* Lookup failure at root level, try to return error symbol. */
  LOOKUP_LOG("[Lookup] FAILED at root scope.\n")

  if constexpr (is_same_v<T, SymbolFunction>) {
    if (auto it = functions.find(SymbolTable::err_symbol); it != functions.end()) {
      return it->second.second;
    }
  }
  else if constexpr (is_same_v<T, SymbolClass>) {
    if (auto it = classes.find(SymbolTable::err_symbol); it != classes.end()) {
      return it->second.second;
    }
  }
  else if constexpr (is_same_v<T, SymbolVariable>) {
    if (auto it = variables.find(SymbolTable::err_symbol); it != variables.end()) {
      return it->second.second;
    }
  }
  else {
    static_assert(false);
  }
  /* Couldn't find the error symbol. Catastrophic failure. */
  assert(0);
  return nullptr;
}

template<typename T>
T *SymbolScope::lookup_generic_nested(Id id, Id last, const SourceLocation &loc) const
{
  LOOKUP_LOG("[Nested] Looking up " << get_symbol_type_name<T>() << " '"
                                    << id.front().buf_->substr(id.front(), last.front())
                                    << "' in scope '" << identifier << "'\n")

  /* Look up anonymous namespace first. */
  if (auto it = scopes.find(""); it != scopes.end()) {
    LOOKUP_LOG("[Nested] Looking up anonymous namespace scope\n")
    Id next_search_id = id.id != last.id ? id.next_id() : id;
    if (auto ret = it->second->lookup_generic_nested<T>(next_search_id, last, loc)) {
      return ret;
    }
  }

  /* Resolve target based on whether it's a namespace or the final symbol. */
  if (id.id != last.id) {
    /* Only classes can have template specifiers. */
    if (TemplateParamList list = id.template_params(); list.is_valid()) {
      LOOKUP_LOG("[Nested] Segment '" << id.str() << "' has template params. Checking classes.\n")
      if (auto it = classes.find(string(id.str())); it != classes.end()) {
        if (SymbolClass *cls = it->second.second->as_class(); cls && cls->template_data) {
          if (auto [tmp, _, err] = cls->template_data->lookup_inst(list, *this); !tmp->is_error) {
            LOOKUP_LOG("[Nested] Found template instance scope '" << tmp->identifier << "'.\n")
            return tmp->lookup_generic_nested<T>(id.next_id(), last, loc);
          }
        }
      }
      LOOKUP_LOG("[Nested] Template instance lookup failed for '" << id.str() << "'.\n")
      return nullptr;
    }

    if (auto it = scopes.find(string(id.str())); it != scopes.end()) {
      LOOKUP_LOG("[Nested] Match sub-scope/namespace '"
                 << id.str() << "' at " << it->second->identifier << ". Descending...\n")
      return it->second->lookup_generic_nested<T>(id.next_id(), last, loc);
    }
    LOOKUP_LOG("[Nested] Mid-segment '" << id.str() << "' could not be resolved as a scope.\n")
    return nullptr;
  }

  if constexpr (is_same_v<T, SymbolFunction>) {
    if (auto it = functions.find(string(id.str())); it != functions.end()) {
      if (it->second.first <= loc) {
        LOOKUP_LOG("[Nested] Found function match: " << it->second.second->identifier << "\n")
        return it->second.second;
      }
      LOOKUP_LOG(
          "[Nested] Found function, but declared AFTER current source location "
          "(shadowed/invisible).\n")
    }
  }
  else if constexpr (is_same_v<T, SymbolClass>) {
    if (auto it = classes.find(string(id.str())); it != classes.end()) {
      if (it->second.first <= loc) {
        LOOKUP_LOG("[Nested] Found class match: " << it->second.second->identifier << "\n")
        return it->second.second;
      }
      LOOKUP_LOG(
          "[Nested] Found class, but declared AFTER current source location "
          "(shadowed/invisible).\n")
    }
  }
  else if constexpr (is_same_v<T, SymbolVariable>) {
    if (auto it = variables.find(string(id.str())); it != variables.end()) {
      if (it->second.first <= loc) {
        if (TemplateParamList list = id.template_params(); list.is_valid()) {
          LOOKUP_LOG(
              "[Nested] Found variable match, but rejected: Variables cannot be templated.\n")
          return nullptr;
        }
        LOOKUP_LOG("[Nested] Found variable match: " << it->second.second->identifier << "\n")
        return it->second.second;
      }
      LOOKUP_LOG(
          "[Nested] Found variable, but declared AFTER current source location "
          "(shadowed/invisible).\n")
    }
  }
  else {
    static_assert(false);
  }
  return nullptr;
}

SymbolClass *SymbolScope::lookup_class(string id) const
{
  return classes.find(id)->second.second;
}

SymbolFunction *SymbolScope::lookup_function(string id) const
{
  return functions.find(id)->second.second;
}

SymbolVariable *SymbolScope::lookup_variable(string id) const
{
  return variables.find(id)->second.second;
}

SymbolClass *SymbolScope::as_class()
{
  return type == CLASS ? static_cast<SymbolClass *>(this) : nullptr;
}

SymbolFunction *SymbolScope::as_function()
{
  return type == FUNCTION ? static_cast<SymbolFunction *>(this) : nullptr;
}

SymbolFunction *SymbolScope::parent_function()
{
  SymbolScope *fn = this;
  while (fn && fn->type != FUNCTION) {
    fn = fn->parent;
  }
  return fn ? fn->as_function() : nullptr;
}

bool SymbolScope::function_emplace(SymbolFunction *fn, bool builtin)
{
  bool result = false;
  if (auto it = functions.emplace(fn); !it.second) {
    /* If function already exists, insert overload in the linked list. */
    it.first->add_overload(fn);
    if (!builtin) {
      /* Add suffix to the function identifier to reduce chance of hitting an overload at runtime.
       * This way, the dead code eliminator can discard more functions.
       * This suffix needs to be the same whatever the file include order is, as it can differ from
       * shader to shader. We use the line index for that. It is short enough to not clutter the
       * resulting source file. */
      fn->identifier += to_string(fn->loc.tok.line_number());
    }
    result = true;
  }
  /* Builtins functions aren't parsed and don't need access to the scope.
   * Avoid bloating global namespace. */
  if (!builtin) {
    scopes.emplace(unique_id(), fn);
  }
  return result;
}

vector<SymbolVariable *> SymbolScope::non_static_variables_in_declaration_order() const
{
  assert(this->type == CLASS);
  vector<SymbolVariable *> members;
  for (const auto &[k, v] : variables) {
    members.emplace_back(v.second);
  }

  /* Sort elements to guarantee deterministic order. */
  sort(members.begin(), members.end(), [](const SymbolVariable *a, const SymbolVariable *b) {
    return a->loc < b->loc;
  });

  return members;
}

void SymbolScope::print() const
{
  enum class SymbolType { Variable, FunctionTemplate, ClassTemplate, Function, Class, Scope };
  /* Helper lambda to handle formatting and tree indentation. */
  auto print_line = [](const SourceLocation src_loc,
                       SymbolType type,
                       const string &identifier,
                       const SymbolClass *decl_type,
                       const string &identifier_mangled,
                       int depth,
                       const vector<bool> &is_last) {
    string loc = src_loc;
    if (loc.starts_with("builtin:") && identifier != "<root>") {
      return;
    }

    /* Create indentation based on tree depth. */
    int padding_size = max(0, 55 - static_cast<int>(loc.size()));
    string padding(padding_size, ' ');

    cout << loc << padding;

    /* Print the tree branch lines based on depth and ancestor sibling status. */
    for (int i = 0; i < depth - 1; ++i) {
      if (is_last[i]) {
        cout << "  "; /* Ancestor was the last sibling, leave space. */
      }
      else {
        cout << "│ "; /* Ancestor has more siblings, draw vertical line. */
      }
    }

    /* Print the branch for the current node. */
    if (depth > 0) {
      if (is_last.back()) {
        cout << "└─"; /* Last sibling. */
      }
      else {
        cout << "├─"; /* Not the last sibling. */
      }
    }

    /* Print the current node type and identifier. */
    cout << "o ";
    switch (type) {
      case SymbolType::Variable:
        cout << "Decl";
        break;
      case SymbolType::FunctionTemplate:
        cout << "TmpF";
        break;
      case SymbolType::ClassTemplate:
        cout << "TmpC";
        break;
      case SymbolType::Function:
        cout << "Func";
        break;
      case SymbolType::Class:
        cout << "Type";
        break;
      case SymbolType::Scope:
        cout << "Scop";
        break;
    }
    if (!identifier.empty()) {
      cout << " " << identifier;
    }
    if (decl_type) {
      cout << " : " << decl_type->identifier;
    }
    if (!identifier_mangled.empty()) {
      cout << " -> " << identifier_mangled;
    }
    cout << "\n";
  };

  /* Print the root node. */
  print_line(loc,
             SymbolType::Scope,
             this->identifier.empty() ? "<root>" : this->identifier,
             nullptr,
             "",
             0,
             {});

  /* Recursive lambda to traverse all symbol branches. */
  auto print_node = [&](auto &self, auto *node, int depth, const vector<bool> &is_last) -> void {
    struct Child {
      SymbolType type;
      string sym;
      const Symbol *resolved;
      const SymbolClass *cls;
    };

    vector<Child> children;
    /* Gather children based on the current node type. */
    for (const auto &[name, var] : node->variables) {
      children.emplace_back(
          SymbolType::Variable, var.second->identifier, var.second, var.second->type);
    }
    for (const auto &[name, func] : node->functions) {
      children.emplace_back(SymbolType::Function, func.second->identifier, func.second, nullptr);
    }
    for (const auto &[name, cls] : node->classes) {
      children.emplace_back(SymbolType::Class, name, cls.second, nullptr);
    }
    for (const auto &[name, scope_ptr] : node->scopes) {
      children.emplace_back(SymbolType::Scope, scope_ptr->identifier, nullptr, nullptr);
    }
    /* Sort elements to guarantee deterministic output (Row -> Col -> Alphabetical). */
    sort(children.begin(), children.end(), [](const Child &a, const Child &b) {
      return a.resolved->loc < b.resolved->loc;
    });

    /* Iterate and print the sorted children. */
    for (size_t i = 0; i < children.size(); ++i) {
      bool child_is_last = (i == children.size() - 1);

      vector<bool> next_is_last = is_last;
      next_is_last.push_back(child_is_last);

      print_line(children[i].resolved->loc,
                 children[i].type,
                 children[i].sym,
                 children[i].cls,
                 children[i].resolved ? children[i].resolved->identifier : "",
                 depth + 1,
                 next_is_last);

      if (children[i].type == SymbolType::Scope) {
        const auto *next_node = static_cast<const SymbolScope *>(children[i].resolved);
        /* Recurse deeper if the child acts as a container for other symbols. */
        self(self, next_node, depth + 1, next_is_last);
      }
    }
  };

  print_node(print_node, this, 0, {});
}

}  // namespace bsl
