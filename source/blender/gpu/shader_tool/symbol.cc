/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "expression.hh"
#include "processor.hh"
#include "symbol.hh"

namespace blender::gpu::shader::parser {

using namespace ast;
using namespace std;

static const char *err_symbol = "ERROR_SYMBOL";
static const char *subscript_operator_id = "sub_op_";
static const char *ns_sep = SourceProcessor::namespace_separator;

/* Return size padded to the given alignment. */
static int pad(int size, int align)
{
  return ((size + align - 1) / align) * align;
}

string SymbolTemplate::mangle_identifier(TemplateParamList list,
                                         const SymbolScope &scope,
                                         const string &sep)
{
  string str;
  list.foreach_child([&](Node node) {
    SymbolClass *type = scope.lookup_class(IdQualified(node));
    str += sep + type->resolved->identifier;
  });
  return str;
}

SymbolClass *SymbolTemplate::lookup_inst_cls(TemplateParamList list,
                                             const SymbolScope &scope) const
{
  string id = mangle_identifier(list, scope);
  auto it = instances_cls.find(id);
  if (it == instances_cls.end()) {
    return scope.root_scope()->lookup_class(err_symbol);
  }
  return it->second;
}

SymbolFunction *SymbolTemplate::lookup_inst_fn(TemplateParamList list,
                                               const SymbolScope &scope) const
{
  string id = mangle_identifier(list, scope);
  auto it = instances_fn.find(id);
  if (it == instances_fn.end()) {
    return scope.root_scope()->lookup_function(err_symbol);
  }
  return it->second;
}

SymbolClass::SymbolClass(SymbolScope *parent, ClassDecl decl, const string &suffix)
    : SymbolScope(parent,
                  decl.front(),
                  decl.identifier().str().empty() ?
                      string("a") + to_string(parent->classes.size()) :
                      string(decl.identifier().name().str()) + suffix,
                  CLASS)

{
  this->is_anonymous = decl.identifier().str().empty();
  this->is_union = decl.front() == Union;
}

void SymbolClass::ensure_size_and_align()
{
  if (size != -1) {
    return;
  }
  size = 0;
  align = 1;
  is_std140_compatible = true;
  is_std430_compatible = true;
  auto process_member_of_class = [&](SymbolClass &cls) {
    cls.ensure_size_and_align();
    if (this->is_union) {
      /* Consider class compatible if it has no implicit padding. */
      is_std140_compatible &= cls.size == size;
      is_std430_compatible &= cls.size == size;

      size = max(size, cls.size);
    }
    else {
      /* Padding to next member alignment. */
      int padded = pad(size, cls.align);
      /* Consider class compatible if it has no implicit padding. */
      is_std140_compatible &= padded == size;
      is_std430_compatible &= padded == size;
      size = padded;
      /* Add member size. */
      size += cls.size;
    }
    /* Update the overall alignment of the current struct. */
    align = max(align, cls.align);
    /* Inherit base compatibility from the member. */
    is_std140_compatible &= cls.is_std140_compatible;
    is_std430_compatible &= cls.is_std430_compatible;

    if (!cls.is_builtin && cls.align % 16 != 0) {
      is_std140_compatible = false;
    }
  };

  for (auto &[_, var] : variables) {
    process_member_of_class(*var->type);
  }

  for (auto &[_, cls] : classes) {
    if (cls->is_anonymous) {
      process_member_of_class(*cls);
    }
  }

  /* The total size of the struct must be a multiple of its alignment. */
  if (align > 0) {
    size = (size + align - 1) / align * align;
  }
  /* The overall size and alignment of a struct in std140 must be rounded up to the base alignment
   * of a vec4 (16 bytes). */
  if (size % 16 != 0 || align % 16 != 0) {
    is_std140_compatible = false;
  }
}

struct SymbolParser {
  SymbolTable &table;
  SymbolScope &global;
  ErrorHandler &err_handler;

  void parse_scope(SymbolScope &scope,
                   Node node,
                   const std::string &prefix,
                   TemplateInst template_inst = {})
  {
    Node parent = node.parent();

    if (parent.parent().type() == NodeType::TemplateDecl) {
      parse_template_arguments(scope, parent.parent(), template_inst);
    }

    if (parent.type() == NodeType::FuncDecl) {
      parse_function_arguments(scope, parent);
    }
    if (parent.type() == NodeType::ForLoop) {
      parse_loop_arguments(scope, parent);
    }

    int offset = 0;

    if (node.type() == NodeType::FuncDecl || node.type() == NodeType::ClassDecl ||
        node.type() == NodeType::LocalScope)
    {
      for (Node child = node.child_first(); child.is_valid(); child = child.next()) {
        switch (child.type()) {
          case NodeType::Namespace:
            parse_namespace(scope, child, prefix);
            break;
          case NodeType::UsingStmt:
            parse_using_stmt(scope, child);
            break;
          case NodeType::LocalScope:
            parse_local_scope(scope, child, prefix);
            break;
          case NodeType::ClassDecl:
            parse_class_decl(scope, child, parent, offset, prefix);
            break;
          case NodeType::EnumValue:
            parse_enum_value(scope, child, parent);
            break;
          case NodeType::FuncDecl:
            parse_func_decl(scope, child, prefix);
            break;
          case NodeType::VarDecl:
            parse_var_decl(scope, child, offset, prefix);
            break;
          case NodeType::TemplateDecl:
            parse_template_decl(scope, child);
            break;
          case NodeType::TemplateInst:
            parse_template_inst(scope, child, prefix);
            break;
          case NodeType::TemplateSpec:
            parse_template_spec(scope, child, prefix);
            break;
          case NodeType::SwitchStmt:
            /* Special case for switch statements which are 3 level deep. */
            child.foreach<SwitchCase>([&](SwitchCase stmt) {
              stmt.foreach<LocalScope>(
                  [&](LocalScope local) { parse_local_scope(scope, local, prefix); });
            });
            break;
          case NodeType::ForLoop:
          case NodeType::DoWhileLoop:
          case NodeType::WhileLoop:
          case NodeType::IfStmt:
          case NodeType::ElseIfStmt:
          case NodeType::ElseStmt:
          case NodeType::ReturnStmt:
            /* Convert local scopes that are nested one level deeper (eg.  if, etc...). */
            child.foreach<LocalScope>(
                [&](LocalScope local) { parse_local_scope(scope, local, prefix); });
            break;
          default:
            break;
        }
      }
    }
  }

  void parse_local_scope(SymbolScope &scope, LocalScope local_node, const std::string &prefix)
  {
    SymbolScope *sym = table.scp_arena.alloc(&scope, local_node);
    scope.scopes.emplace(sym->identifier, sym);

    parse_scope(*sym, local_node, prefix + sym->identifier + ns_sep);
  }

  /* Create declaration for each template argument so name lookup will work with these. */
  void parse_template_arguments(SymbolScope &scope, TemplateDecl decl, TemplateInst inst)
  {
    TemplateParamList params = inst.parameters();
    Node param = params.child_first();

    decl.arguments().foreach_child([&](TemplateArg arg) {
      assert(arg.is_valid());
      IdQualified type = arg.type();
      string id(arg.id().str());
      if (type.str() == "typename") {
        SymbolClass *cls = table.cls_arena.alloc(&scope, arg.id().front(), id);
        SymbolClass *resolved = scope.lookup_class(param);
        cls->resolved = resolved;
        if (resolved->is_error) {
          error(param, "Unknown type in template instantiation");
        }
        scope.classes.emplace(id, cls);
      }
      else {
        SymbolClass *cls = scope.lookup_class(type);
        SymbolVariable *var = table.var_arena.alloc(&scope, cls, arg.id().front(), id);
        scope.variables.emplace(id, var);
        /* TODO(fclem): Fold constexpr. */
        error(param, "Unsupported TODO");
      }
      param = param.next();
    });
  }

  /* Create declaration for each function argument so name lookup will work with these.
   * Also create declaration for `this_`. */
  void parse_function_arguments(SymbolScope &scope, FuncDecl decl)
  {
    decl.arguments().foreach<FuncArg>([&](FuncArg arg) {
      SymbolClass *type = scope.lookup_class(arg.type().id());
      SymbolVariable var(&scope, type, arg.declarator());
      var.type = scope.lookup_class(arg.type().id());
      scope.variables.emplace(var.identifier, table.var_arena.alloc(var));
    });

    if (scope.parent != nullptr) {
      /* Add `this_` to the var declarations. */
      auto &fns = scope.parent->functions;
      if (auto it = fns.find(scope.identifier); it != fns.end()) {
        SymbolFunction *fn = it->second;
        if (fn->fn_type == SymbolFunction::MEMBER) {
          SymbolVariable *var = table.var_arena.alloc(
              &scope, fn->parent_class(), decl.front(), "this_");
          scope.variables.emplace(var->identifier, var);
        }
      }
    }
  }

  /* Parse declarators inside for loop conditions. */
  void parse_loop_arguments(SymbolScope &scope, ForLoop loop)
  {
    loop.condition().foreach_recursive<VarDecl>([&](VarDecl var) {
      SymbolClass *type = scope.lookup_class(var.type().id());
      var.foreach<Declarator>([&](Declarator decl) {
        SymbolVariable sym(&scope, type, decl);
        sym.type = type;
        scope.variables.emplace(sym.identifier, table.var_arena.alloc(sym));
      });
    });
  }

  void parse_namespace(SymbolScope &scope, Namespace decl, string ns_prefix)
  {
    IdQualified ns = decl.identifier();
    /* Walk nested identifier */
    SymbolScope *ns_scope = &scope;
    ns.foreach<Id>([&](Id id) {
      SymbolScope *sym = table.scp_arena.alloc(ns_scope, id, SymbolScope::NAMESPACE);
      scope.scopes.emplace(sym->identifier, sym);
      ns_scope = sym;
      ns_prefix = ns_prefix + sym->identifier + ns_sep;
    });
    parse_scope(*ns_scope, decl.body(), ns_prefix);
  }

  void parse_using_stmt(SymbolScope &scope, UsingStmt stmt)
  {
    if (stmt.is_namespace()) {
      error(stmt, "Using namespace statement are not supported");
    }
    else {
      IdQualified aliased(stmt.aliased());
      IdQualified name(stmt.id());
      SymbolClass *type = scope.lookup_class(aliased);
      if (type->is_error) {
        error(aliased, "Unknown Type");
        return;
      }
      string identifier(name.name().str());
      if (type) {
        scope.classes.emplace(identifier, type);
        return;
      }
    }
  }

  void parse_enum_value(SymbolScope &scope, EnumValue val, ClassDecl cls)
  {
    SymbolClass *type = scope.lookup_class(cls.parent_class());
    SymbolVariable sym(&scope, type, val);
    SymbolVariable *ptr = table.var_arena.alloc(sym);
    scope.variables.emplace(sym.identifier, ptr);

    if (!cls.is_enum_class()) {
      /* Alias to the parent namespace for non-anonymous, non-class enum. */
      scope.parent->variables.emplace(sym.identifier, ptr);
    }
  }

  SymbolClass *parse_class_decl(SymbolScope &scope,
                                ClassDecl decl,
                                ClassDecl parent_class,
                                int &offset,
                                const std::string &prefix,
                                const std::string &suffix = "",
                                TemplateInst temp = {})
  {
    SymbolClass *cls = table.cls_arena.alloc(&scope, decl, suffix);
    scope.classes.emplace(cls->identifier, cls);
    scope.scopes.emplace(cls->identifier, cls);

    parse_scope(*cls, decl.body(), prefix + cls->identifier + ns_sep, temp);

    cls->ensure_size_and_align();

    if (cls->is_anonymous) {
      /* Instantiate into parent class as member. */
      SymbolVariable *var = table.var_arena.alloc(
          &scope, cls, decl.front(), cls->identifier + ns_sep);
      scope.variables.emplace(cls->identifier, var);

      var->type = cls;
      var->set_offset(parent_class.is_union(), offset);
    }
    {
      /* Instantiate in global resolved namespace. */
      SymbolClass *flat_cls = table.cls_arena.alloc(*cls);
      cls->resolved = flat_cls;
      flat_cls->resolved = cls;
      flat_cls->identifier = prefix + cls->identifier;
      global.classes.try_emplace(flat_cls->identifier, flat_cls);
    }
    return cls;
  }

  SymbolFunction *parse_func_decl(SymbolScope &scope,
                                  FuncDecl func,
                                  const std::string &prefix,
                                  const std::string &suffix = "",
                                  TemplateInst temp = {})
  {
    SymbolFunction *fn = table.fun_arena.alloc(&scope, func, suffix);
    scope.functions.emplace(fn->identifier, fn);
    scope.scopes.emplace(scope.unique_id(), fn);
    parse_scope(*fn, func.body(), prefix + fn->identifier + ns_sep, temp);

    {
      /* Instantiate in global resolved namespace. */
      string identifier_mangled;
      switch (fn->fn_type) {
        case SymbolFunction::GLOBAL:
        case SymbolFunction::STATIC:
          identifier_mangled = prefix + fn->identifier;
          break;
        case SymbolFunction::MEMBER:
          /* Member function use overload resolution. */
          identifier_mangled = "_" + fn->identifier;
          break;
      }
      SymbolFunction *flat_func = table.fun_arena.alloc(*fn);
      fn->resolved = flat_func;
      flat_func->resolved = fn;
      flat_func->identifier = identifier_mangled;
      global.functions.try_emplace(identifier_mangled, flat_func);
    }

    return fn;
  }

  void parse_template_decl(SymbolScope &scope, TemplateDecl decl)
  {
    /* Container for template data. */
    SymbolTemplate *temp = table.tmp_arena.alloc(decl);
    /* Do not parse. Only keep the symbol definition. The instantiation will do the parsing. */
    if (decl.is_function()) {
      SymbolFunction *fn = table.fun_arena.alloc(&scope, decl.decl());
      fn->template_data = temp;
      scope.functions.emplace(fn->identifier, fn);
    }
    else {
      SymbolClass *cls = table.cls_arena.alloc(&scope, decl.decl());
      cls->template_data = temp;
      scope.classes.emplace(cls->identifier, cls);
    }
  }

  template<typename DeclAst, typename SymbolT>
  TemplateDecl check_template_instance(TemplateParamList temp_params,
                                       DeclAst &decl,
                                       const SymbolT &sym)
  {
    if (sym.is_error) {
      error(decl.identifier(), "Missing template definition");
      return {};
    }
    if (sym.template_data == nullptr) {
      error(decl.identifier(), "Function is not a template");
      return {};
    }
    TemplateDecl temp_decl = sym.template_data->decl;
    if (temp_decl.arguments().child_count() != temp_params.child_count()) {
      error(temp_params, "Template parameter count must match declaration argument count");
      return {};
    }
    return temp_decl;
  }

  void parse_template_inst(SymbolScope &scope, TemplateInst temp, const std::string &prefix)
  {
    TemplateParamList temp_params = temp.parameters();

    if (temp.is_function()) {
      const FuncForwardDecl decl(temp.decl());
      SymbolFunction *fn = scope.lookup_function(decl.identifier());

      if (TemplateDecl temp_decl = check_template_instance(temp_params, decl, *fn);
          temp_decl.is_valid())
      {
        string arg_mangled = SymbolTemplate::mangle_identifier(temp_params, scope);

        SymbolFunction *fn_inst = parse_func_decl(scope, temp_decl.decl(), "", arg_mangled, temp);

        fn->template_data->instances_fn.emplace(arg_mangled, fn_inst);
      }
    }
    else {
      const ClassDecl decl(temp.decl());
      SymbolClass *cls = scope.lookup_class(decl.identifier());

      if (TemplateDecl temp_decl = check_template_instance(temp_params, decl, *cls);
          temp_decl.is_valid())
      {
        string arg_mangled = SymbolTemplate::mangle_identifier(temp_params, scope);

        int unused_offset = 0;
        SymbolClass *cls_inst = parse_class_decl(
            scope, temp_decl.decl(), Node{}, unused_offset, prefix, arg_mangled, temp);

        cls->template_data->instances_cls.emplace(arg_mangled, cls_inst);
      }
    }
  }

  void parse_template_spec(SymbolScope &scope, TemplateSpec temp, const std::string &prefix)
  {
    TemplateParamList temp_params = temp.parameters();
    /* Instantiate the whole symbol into the current namespace. */
    if (temp.is_function()) {
      const FuncDecl decl(temp.decl());
      SymbolFunction *fn = scope.lookup_function(decl.identifier());

      if (TemplateDecl temp_decl = check_template_instance(temp_params, decl, *fn);
          temp_decl.is_valid())
      {
        string arg_mangled = SymbolTemplate::mangle_identifier(temp_params, scope);

        SymbolFunction *spec = parse_func_decl(scope, decl, prefix, arg_mangled);

        fn->template_data->instances_fn.emplace(arg_mangled, spec);
      }
    }
    else {
      const ClassDecl decl(temp.decl());
      SymbolClass *cls = scope.lookup_class(decl.identifier());

      if (TemplateDecl temp_decl = check_template_instance(temp_params, decl, *cls);
          temp_decl.is_valid())
      {
        string arg_mangled = SymbolTemplate::mangle_identifier(temp_params, scope);

        int unused_offset = 0;
        SymbolClass *spec = parse_class_decl(
            scope, decl, Node{}, unused_offset, prefix, arg_mangled);

        cls->template_data->instances_cls.emplace(arg_mangled, spec);
      }
    }
  }

  void parse_var_decl(SymbolScope &scope, VarDecl var, int &offset, const std::string &prefix)
  {
    SymbolClass *type = scope.lookup_class(var.type().id());
    if (type->template_data) {
      TemplateParamList param = var.type().id().template_params();

      SymbolClass *base_type = type;
      type = base_type->template_data->lookup_inst_cls(param, scope);
      if (type->is_error) {
        string args = SymbolTemplate::mangle_identifier(param, scope, ", ");
        error(param,
              "Missing explicit instantiation of template " + base_type->identifier + "<" +
                  args.substr(2) + ">");
      }
    }

    if (type->is_error) {
      error(var, "Unknown type");
      return;
    }

    var.foreach<Declarator>([&](Declarator decl) {
      SymbolVariable *sym = table.var_arena.alloc(&scope, type, decl);
      scope.variables.emplace(sym->identifier, sym);

      SymbolClass *cls = scope.as_class();

      if (cls) {
        sym->set_offset(cls->is_union, offset);

        /* Alias to the parent class for anonymous structs. */
        if (cls->is_anonymous) {
          auto [anon_prefix, named_parent] = anonymous_scope_prefix(cls);
          /* Create a resolved symbol. */
          SymbolVariable *resolved = table.var_arena.alloc(&scope, type, decl);
          resolved->identifier = anon_prefix + resolved->identifier;
          sym->resolved = resolved;
          /* Alias to the first, non-anonymous parent. */
          named_parent->variables.emplace(sym->identifier, sym);
        }
      }

      if (sym->is_static) {
        /* Instantiate in global resolved namespace. */
        SymbolVariable *flat_var = table.var_arena.alloc(*sym);
        sym->resolved = flat_var;
        flat_var->resolved = sym;
        flat_var->identifier = prefix + sym->identifier;
        global.variables.try_emplace(flat_var->identifier, flat_var);
      }

      if (sym->is_constexpr) {
        if (sym->is_static) {
          sym->value = initialize_constexpr(scope, sym->type, decl);
          sym->resolved->value = sym->value;
        }
        else {
          error(var.type(), "Constexpr declaration must also be static");
        }
      }
    });
  }

  int64_t eval_scalar_initializer_list(const SymbolScope &scope, InitializerList list)
  {
    if (list.is_empty()) {
      return 0;
    }
    if (list.child_count() != 1) {
      error(list, "Excess elements in scalar initializer");
      return 0;
    }
    if (Initializer init = list.child_first(); init.is_valid()) {
      Node node = init.child_first();
      if (node.type() == NodeType::InitializerList) {
        return eval_scalar_initializer_list(scope, node);
      }
      return evaluate_constexpr(scope, node);
    }
    return 0;
  }

  int64_t initialize_constexpr(const SymbolScope &scope, SymbolClass *cls, Declarator decl)
  {
    if (cls->identifier != "int" && cls->identifier != "uint") {
      error(decl, "Constexpr variable must be of type int or uint");
      return 0;
    }
    if (AssignStmt assign = decl.initial_value(); assign.is_valid()) {
      if (InitializerList list = assign.initializer_list(); list.is_valid()) {
        return eval_scalar_initializer_list(scope, list);
      }
      return evaluate_constexpr(scope, assign.expr());
    }
    if (InitializerList list = decl.initializer_list(); list.is_valid()) {
      return eval_scalar_initializer_list(scope, list);
    }
    error(decl,
          "Constexpr variable '" + string(decl.identifier().str()) +
              "' must be initialized by a constant expression");
    return 0;
  }

  string expr_to_string(const SymbolScope &scope, Expr expr, int &node_count)
  {
    string expr_str;
    expr.foreach_child([&](Node child) {
      switch (child.type()) {
        /* TODO member access. */
        case NodeType::LocalVar: {
          SymbolVariable *var = scope.lookup_variable(LocalVar(child).identifier());
          if (var->is_constexpr) {
            expr_str += to_string(var->value);
          }
          else {
            error(child, "Read of non-const variable is not allowed in a constant expression");
            expr_str += "0";
          }
          node_count++;
          break;
        }
        case NodeType::FuncCall:
          error(child, "Constexpr cannot contain function calls");
          expr_str += "0";
          node_count++;
          break;
        case NodeType::ExprSub:
          expr_str += '(';
          expr_str += expr_to_string(scope, ExprSub(child).expr(), node_count);
          expr_str += ')';
          node_count++;
          break;
        case NodeType::Op:
        case NodeType::NumConst:
          expr_str += child.str();
          node_count++;
          break;
        default:
          error(child, "Unsupported symbol inside constexpr definition");
          expr_str += "0";
          node_count++;
          break;
      }
    });
    return expr_str;
  }

  int64_t evaluate_constexpr(const SymbolScope &scope, Expr expr)
  {
    /* First build a string containing only numerical constants by substituting all the symbols. */
    int count = 0;
    string expr_str = expr_to_string(scope, expr, count);

    /* Fast path. */
    if (count == 1) {
      return stoll(expr_str);
    }

    /* Then use the expression parser to evaluate it. */
    try {
      ExpressionParser expression_parser;
      expression_parser.lexical_analysis(expr_str);
      return expression_parser.eval();
    }
    catch (const std::exception &e) {
      error(expr, "Failed to evaluate expanded expression '" + expr_str + "'");
      return 0;
    }
    return 0;
  }

  struct AnonScopePrefix {
    string prefix;
    SymbolScope *non_anonymous_parent;
  };

  static AnonScopePrefix anonymous_scope_prefix(SymbolClass *cls)
  {
    string prefix;
    /* Support multiple nesting level. */
    do {
      /* Union members need to go through the getter functions. */
      string access = cls->is_union ? "()" : "";
      prefix = cls->identifier + "_" + access + "." + prefix;
    } while ((cls = cls->parent->as_class(), cls->is_anonymous));

    return {prefix, cls};
  }

 private:
  void error(ast::Node node, const string &msg)
  {
    err_handler.report(node.front(), msg);
  }
};

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

SymbolFunction *SymbolScope::lookup_function(IdQualified id) const
{
  return lookup_generic<SymbolFunction>(id, id.front());
}
SymbolClass *SymbolScope::lookup_class(IdQualified id) const
{
  return lookup_generic<SymbolClass>(id, id.front());
}
SymbolVariable *SymbolScope::lookup_variable(IdQualified id) const
{
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

template<typename T>
T *SymbolScope::lookup_generic(IdQualified id, const SourceLocation &loc) const
{
  assert(id.is_valid());
  /* Bubble up immediately if it's a global lookup. */
  if (id.is_global() && parent) {
    return parent->lookup_generic<T>(id, loc);
  }
  /* Try to match within this scope. */
  if (auto var = lookup_generic_nested<T>(id.namespace_start(), loc)) {
    return var;
  }
  /* Delegate to parent scope if not found locally. */
  if (parent) {
    return parent->lookup_generic<T>(id, loc);
  }
  /* Lookup failure at root level, try to return error symbol. */
  if constexpr (is_same_v<T, SymbolFunction>) {
    if (auto it = functions.find(err_symbol); it != functions.end()) {
      return it->second;
    }
  }
  else if constexpr (is_same_v<T, SymbolClass>) {
    if (auto it = classes.find(err_symbol); it != classes.end()) {
      return it->second;
    }
  }
  else if constexpr (is_same_v<T, SymbolVariable>) {
    if (auto it = variables.find(err_symbol); it != variables.end()) {
      return it->second;
    }
  }
  else {
    static_assert(false);
  }
  /* Couldn't find the error symbol. Catastrophic failure. */
  assert(0);
  return nullptr;
}

template<typename T> T *SymbolScope::lookup_generic_nested(Id id, const SourceLocation &loc) const
{
  /* Look up anonymous namespace first. */
  if (auto it = scopes.find(""); it != scopes.end()) {
    Id next_search_id = id.is_namespace() ? id.next_id() : id;
    if (auto ret = it->second->lookup_generic_nested<T>(next_search_id, loc)) {
      return ret;
    }
  }
  /* Resolve target based on whether it's a namespace or the final symbol. */
  if (id.is_namespace()) {
    if (auto it = scopes.find(string(id.str())); it != scopes.end()) {
      return it->second->lookup_generic_nested<T>(id.next_id(), loc);
    }
  }
  else {
    if constexpr (is_same_v<T, SymbolFunction>) {
      if (auto it = functions.find(string(id.str())); it != functions.end()) {
        if (it->second->loc <= loc) {
          return it->second;
        }
      }
    }
    else if constexpr (is_same_v<T, SymbolClass>) {
      if (auto it = classes.find(string(id.str())); it != classes.end()) {
        if (it->second->loc <= loc) {
          return it->second;
        }
      }
    }
    else if constexpr (is_same_v<T, SymbolVariable>) {
      if (auto it = variables.find(string(id.str())); it != variables.end()) {
        if (it->second->loc <= loc) {
          return it->second;
        }
      }
    }
    else {
      static_assert(false);
    }
  }
  return nullptr;
}

SymbolClass *SymbolScope::lookup_class(string id) const
{
  return classes.find(id)->second;
}

SymbolFunction *SymbolScope::lookup_function(string id) const
{
  return functions.find(id)->second;
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
    struct Element {
      string *file;
      int row;
      int col;
      string type;
      string identifier;
      variant<const SymbolScope *,
              const SymbolClass *,
              const SymbolFunction *,
              const SymbolVariable *>
          ptr;
    };

    struct Child {
      SymbolType type;
      const Symbol *sym;
      const Symbol *resolved;
      const SymbolClass *cls;
    };

    vector<Child> children;
    /* Gather children based on the current node type. */
    for (const auto &[name, var] : node->variables) {
      children.emplace_back(SymbolType::Variable, var, var->resolved, var->type);
    }
    for (const auto &[name, func] : node->functions) {
      children.emplace_back(SymbolType::Function, func, func->resolved, nullptr);
    }
    for (const auto &[name, cls] : node->classes) {
      children.emplace_back(SymbolType::Class, cls, cls->resolved, nullptr);
    }
    for (const auto &[name, scope_ptr] : node->scopes) {
      children.emplace_back(SymbolType::Scope, scope_ptr, nullptr, nullptr);
    }
    /* Sort elements to guarantee deterministic output (Row -> Col -> Alphabetical). */
    sort(children.begin(), children.end(), [](const Child &a, const Child &b) {
      return a.sym->loc < b.sym->loc;
    });

    /* Iterate and print the sorted children. */
    for (size_t i = 0; i < children.size(); ++i) {
      bool child_is_last = (i == children.size() - 1);

      vector<bool> next_is_last = is_last;
      next_is_last.push_back(child_is_last);

      print_line(children[i].sym->loc,
                 children[i].type,
                 children[i].sym->identifier,
                 children[i].cls,
                 children[i].resolved ? children[i].resolved->identifier : "",
                 depth + 1,
                 next_is_last);

      if (children[i].type == SymbolType::Scope) {
        const auto *next_node = static_cast<const SymbolScope *>(children[i].sym);
        /* Recurse deeper if the child acts as a container for other symbols. */
        self(self, next_node, depth + 1, next_is_last);
      }
    }
  };

  print_node(print_node, this, 0, {});
}

SymbolTable::SymbolTable(ParserBase &parser)
{
  LocalScope node = parser.root();
  root = scp_arena.alloc(node);
  flatten_root = scp_arena.alloc(node);
  register_builtins(node);
}

void SymbolTable::parse(LocalScope node, ErrorHandler &err_handler)
{
  SymbolParser symbol_parser{*this, *flatten_root, err_handler};
  symbol_parser.parse_scope(*root, node, "");

  if (err_handler.err.has_value()) {
    throw ParserException();
  }
}

void SymbolTable::register_builtins(LocalScope node)
{
  Token tok = node.back();

  struct BuiltinType {
    string id;
    size_t size;
    size_t align;
  };

  const vector<BuiltinType> types = {
      {err_symbol, 1, 1},

      {"void", 1, 1}, /* Only for function return type. */

      {"char", 1, 1},
      {"short", 2, 2},
      {"int", 4, 4},
      {"uchar", 1, 1},
      {"ushort", 2, 2},
      {"uint", 4, 4},
      {"half", 2, 2},
      {"float", 4, 4},
      {"bool", 1, 1},
      {"int32_t", 4, 4},
      {"uint32_t", 4, 4},
      {"bool32_t", 4, 4},

      {"samplerBuffer", 0, 1},
      {"sampler1D", 0, 1},
      {"sampler2D", 0, 1},
      {"sampler3D", 0, 1},
      {"isamplerBuffer", 0, 1},
      {"isampler1D", 0, 1},
      {"isampler2D", 0, 1},
      {"isampler3D", 0, 1},
      {"usamplerBuffer", 0, 1},
      {"usampler1D", 0, 1},
      {"usampler2D", 0, 1},
      {"usampler3D", 0, 1},
      {"sampler1DArray", 0, 1},
      {"sampler2DArray", 0, 1},
      {"isampler1DArray", 0, 1},
      {"isampler2DArray", 0, 1},
      {"usampler1DArray", 0, 1},
      {"usampler2DArray", 0, 1},
      {"samplerCube", 0, 1},
      {"isamplerCube", 0, 1},
      {"usamplerCube", 0, 1},
      {"samplerCubeArray", 0, 1},
      {"isamplerCubeArray", 0, 1},
      {"usamplerCubeArray", 0, 1},
      {"usampler1DAtomic", 0, 1},
      {"usampler2DAtomic", 0, 1},
      {"usampler2DArrayAtomic", 0, 1},
      {"usampler3DAtomic", 0, 1},
      {"isampler1DAtomic", 0, 1},
      {"isampler2DAtomic", 0, 1},
      {"isampler2DArrayAtomic", 0, 1},
      {"isampler3DAtomic", 0, 1},
      {"sampler2DDepth", 0, 1},
      {"sampler2DArrayDepth", 0, 1},
      {"samplerCubeDepth", 0, 1},
      {"samplerCubeArrayDepth", 0, 1},
      {"image1D", 0, 1},
      {"image2D", 0, 1},
      {"image3D", 0, 1},
      {"iimage1D", 0, 1},
      {"iimage2D", 0, 1},
      {"iimage3D", 0, 1},
      {"uimage1D", 0, 1},
      {"uimage2D", 0, 1},
      {"uimage3D", 0, 1},
      {"image1DArray", 0, 1},
      {"image2DArray", 0, 1},
      {"iimage1DArray", 0, 1},
      {"iimage2DArray", 0, 1},
      {"uimage1DArray", 0, 1},
      {"uimage2DArray", 0, 1},
      {"iimage2DAtomic", 0, 1},
      {"iimage3DAtomic", 0, 1},
      {"uimage2DAtomic", 0, 1},
      {"uimage3DAtomic", 0, 1},
      {"iimage2DArrayAtomic", 0, 1},
      {"uimage2DArrayAtomic", 0, 1},
  };

  for (const auto &t : types) {
    SymbolClass cls(root, tok, t.id, t.size, t.align);
    cls.is_std140_compatible = t.align == 4;
    cls.is_std430_compatible = t.align == 4;
    cls.is_error = t.id == err_symbol;

    SymbolClass *sym = cls_arena.alloc(cls);
    SymbolClass *sym_resolved = cls_arena.alloc(cls);
    sym->resolved = sym_resolved;
    root->classes.emplace(t.id, sym);
    flatten_root->classes.emplace(t.id, sym_resolved);
  }

  struct BuiltinVector {
    string id;
    string base;
    int comp;
    int align;
  };

  const std::vector<BuiltinVector> vectors = {
      {"float2", "float", 2, 8},     {"float3", "float", 3, 16},    {"float4", "float", 4, 16},
      {"float2x2", "float2", 2, 16}, {"float2x3", "float3", 2, 16}, {"float2x4", "float4", 2, 16},
      {"float3x2", "float2", 3, 16}, {"float3x3", "float3", 3, 16}, {"float3x4", "float4", 3, 16},
      {"float4x2", "float2", 4, 16}, {"float4x3", "float3", 4, 16}, {"float4x4", "float4", 4, 16},
      {"int2", "int", 2, 8},         {"int3", "int", 3, 16},        {"int4", "int", 4, 16},
      {"uint2", "uint", 2, 8},       {"uint3", "uint", 3, 16},      {"uint4", "uint", 4, 16},
      {"bool2", "bool", 2, 2},       {"bool3", "bool", 3, 3},       {"bool4", "bool", 4, 4},
  };

  const char *comp_name[4] = {"x", "y", "z", "w"};

  for (const auto &t : vectors) {
    SymbolClass *cls = cls_arena.alloc(
        root, tok, t.id, t.comp * root->classes[t.base]->size, t.align);
    root->classes.emplace(t.id, cls);
    root->scopes.emplace(t.id, cls);

    cls->is_std140_compatible = t.align >= 8;
    cls->is_std430_compatible = t.align >= 8;

    SymbolFunction *sub_op = fun_arena.alloc(
        cls, tok, subscript_operator_id, SymbolFunction::MEMBER);
    sub_op->return_type = root->classes[t.base];

    cls->operator_subscript = sub_op;
    cls->functions.emplace(subscript_operator_id, sub_op);

    SymbolClass *sym_resolved = cls_arena.alloc(*cls);
    cls->resolved = sym_resolved;
    flatten_root->classes.emplace(t.id, sym_resolved);

    for (int i = 0; i < t.comp; i++) {
      string id(comp_name[i]);
      SymbolVariable *var = var_arena.alloc(cls, root->classes[t.base], tok, id);
      cls->variables.emplace(id, var);
    }
  }

  /* Swizzle Generation */
  /* We do this in a separate loop because a swizzle on a float2 (like .xxxx)
   * might require float4 to already be registered in root->classes. */
  for (const auto &t : vectors) {
    /* Matrices (e.g., float2x2 where t.base is "float2") do not have standard swizzles.  We only
     * generate swizzles for vectors whose base components are scalars. */
    if (t.base != "float" && t.base != "int" && t.base != "uint" && t.base != "bool") {
      continue;
    }
    SymbolScope *scope = root->scopes[t.id];

    auto generate_swizzles = [&](auto &self, string current, int target_length) -> void {
      if (current.length() == target_length) {
        /* e.g., "float" + "3" = "float3" */
        string return_type_id = t.base + to_string(target_length);

        SymbolVariable var(scope, root->classes[return_type_id], tok, current);

        scope->variables.emplace(current, var_arena.alloc(var));
        return;
      }
      /* Only allow swizzling components that actually exist on this vector type. */
      for (int i = 0; i < t.comp; i++) {
        self(self, current + comp_name[i], target_length);
      }
    };
    /* Generate combinations for vec2, vec3, and vec4 variants. */
    for (int len = 2; len <= 4; len++) {
      generate_swizzles(generate_swizzles, "", len);
    }
  }

  /* Error function symbol. */
  {
    SymbolFunction *sym = fun_arena.alloc(root, tok, string(err_symbol), SymbolFunction::GLOBAL);
    sym->is_error = true;
    SymbolFunction *sym_resolved = fun_arena.alloc(*sym);
    sym->resolved = sym_resolved;
    root->functions.emplace(sym->identifier, sym);
    flatten_root->functions.emplace(sym->identifier, sym_resolved);
  }

  /* Error variable symbol. */
  {
    SymbolVariable *sym = var_arena.alloc(
        root, root->lookup_class(err_symbol), tok, string(err_symbol));
    sym->is_error = true;
    SymbolVariable *sym_resolved = var_arena.alloc(*sym);
    sym->resolved = sym_resolved;
    root->variables.emplace(sym->identifier, sym);
    flatten_root->variables.emplace(sym->identifier, sym_resolved);
  }
}

}  // namespace blender::gpu::shader::parser
