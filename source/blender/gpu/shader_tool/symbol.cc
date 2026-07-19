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

static const char *subscript_operator_id = "arr_op_";
static const char *ns_sep = SourceProcessor::namespace_separator;

/**
 * Type resolution parser.
 * Will evaluate each operand type and return the type of the expression result.
 */
class ExpressionTypeParser {
 public:
  Result<SymbolClass *> eval(const SymbolTable &table, const SymbolScope &scope, Expr expr) const
  {
    EvalContext ctx(&table, &scope, expr.child_first());

    Result<SymbolClass *> result;
    result.value = ctx.expr(0);
    result.err = ctx.error;

    if (ctx.peek().is_valid() && !result.err.has_value()) {
      result.err = AstNodeException(ctx.peek(), "Trailing input");
    }
    return result;
  }

 private:
  struct EvalContext {
    std::optional<AstNodeException> error;

   private:
    const SymbolTable *table;
    const SymbolScope *scope;
    Node node;
    SymbolClass *float_cls;
    SymbolClass *int_cls;
    SymbolClass *uint_cls;
    SymbolClass *str_cls;
    SymbolClass *err_cls;

   public:
    EvalContext(const SymbolTable *table, const SymbolScope *scope, Node node)
        : table(table), scope(scope), node(node)
    {
      const SymbolScope &root = *scope->root_scope();
      float_cls = root.lookup_class("float");
      int_cls = root.lookup_class("int");
      uint_cls = root.lookup_class("uint");
      // str_cls = root.lookup_class("string");
      err_cls = root.lookup_class(SymbolTable::err_symbol);
    }

    SymbolClass *expr(int right_binding_power)
    {
      /* Parse unary operator, evaluate parenthesis, evaluate constant. */
      SymbolClass *left = nud(consume());
      /* While left binding power is greater than the right, continue consuming binary operations.
       */
      while (left_binding_power(peek()) > right_binding_power) {
        left = led(left, consume());
      }
      return left;
    }

    /* How a token evaluates without left context (e.g. unary operator).
     * Also known as Null-Denotation or NUD. */
    SymbolClass *nud(ast::Node node)
    {
      /* Unary operators must have the highest precedence. */
      static constexpr int unary_binding_power = 1000;

      switch (node.type()) {
        // case NodeType::TemplateExplicit: /* Should have already been removed. */
        case NodeType::FuncCall:
          return fun_call(node);
          // case NodeType::Constructor: /* TODO */
        case NodeType::StringConst:
          return str_cls;
        case NodeType::LocalVar:
          return local_var(node);
        case NodeType::NumConst:
          return num_const(node);
        case NodeType::Op:
          return op(node, expr(unary_binding_power));
        case NodeType::ExprSub: {
          EvalContext ctx(table, scope, ExprSub(node).expr().child_first());
          return ctx.expr(0);
        }
        default:
          error = AstNodeException(node, "Invalid expression");
          return err_cls;
      }
    }

    /* How a token evaluates from left-to-right, on two operands.
     * Also known as Left-Denotation or LED. */
    SymbolClass *led(SymbolClass *left, Node node)
    {
      if (node.type() != NodeType::Op) {
        error = AstNodeException(node, "Invalid operator");
      }

      /* Binary operator. */
      if (node.front() != '?') {
        return op(node, left, expr(left_binding_power(node)));
      }

      /* Ternary operator. */

      /* The middle expression can be almost anything.
       * We use 0 so it only stops at the ':' (since Colon has a precedence of 0). */
      SymbolClass *tval = expr(0);

      consume(); /* ':' */

      /* Use (Precedence - 1) to handle right-associativity. */
      SymbolClass *fval = expr(left_binding_power(Question) - 1);

      if (tval != fval) {
        error = AstNodeException(node, "Incompatible operand types");
      }
      /* Operand types match. We can return either. */
      return fval;
    }

    int left_binding_power(Node node)
    {
      return left_binding_power(node.front().type());
    }

    int left_binding_power(TokenType type)
    {
      switch (type) {
        case Multiply:
        case Divide:
        case Modulo:
          return 110;
        case Plus:
        case Minus:
          return 100;
        case LShift:
        case RShift:
          return 90;
        case LThan:
        case LEqual:
        case GThan:
        case GEqual:
          return 80;
        case Equal:
        case NotEqual:
          return 70;
        case And:
          return 60;
        case Xor:
          return 50;
        case Or:
          return 40;
        case LogicalAnd:
          return 30;
        case LogicalOr:
          return 20;
        case Question:
          return 10;
        case Colon:
        case ParOpen:
        case ParClose:
          return 0;
        case Not:
        case BitwiseNot:
          /* Prefix operators don't bind to the left! */
          return 0;
        case Invalid: /* EndOfFile */
          return -1;
        default:
          break;
      }
      error = AstNodeException(Node{}, "Invalid operator token");
      return -1;
    }

    Node peek() const
    {
      return node;
    }

    Node consume()
    {
      Node n = node;
      node = node.next();
      return n;
    }

    SymbolClass *op(Node operator_type, SymbolClass *left_type, SymbolClass *right_type)
    {
      OperatorKey key{left_type, operator_type.front().type(), right_type};
      if (auto it = table->operators.find(key); it != table->operators.end()) {
        return it->second->return_type;
      }
      error = AstNodeException(operator_type,
                               "Invalid operands to binary expression ('" + left_type->identifier +
                                   "' and '" + right_type->identifier + "')");
      return err_cls;
    }

    SymbolClass *op(Node operator_type, SymbolClass *right_type)
    {
      OperatorKey key{nullptr, operator_type.front().type(), right_type};
      if (auto it = table->operators.find(key); it != table->operators.end()) {
        return it->second->return_type;
      }
      error = AstNodeException(operator_type,
                               "Invalid argument type '" + right_type->identifier +
                                   "' to unary expression");
      return err_cls;
    }

    SymbolClass *fun_call(FuncCall call)
    {
      SymbolFunction *fn = scope->lookup_function(call.identifier());
      auto [arg_types, err] = SymbolFunction::to_arg_types(*table, *scope, call.parameters());
      if (err) {
        error = err;
        return err_cls;
      }

      fn->lookup_overload(arg_types);
      SymbolClass *type = fn->return_type;

      Node next = call.next();
      if (next.type() == NodeType::Op && next.front() == Dot) {
        /* Member access */
        error = AstNodeException(Node{}, "Not implemented yet");
      }
      if (next.type() == NodeType::Subscript) {
        /* Subscript. */
        error = AstNodeException(Node{}, "Not implemented yet");
      }
      return type;
    }

    SymbolClass *local_var(LocalVar var)
    {
      SymbolVariable *sym = scope->lookup_variable(var.identifier());
      SymbolClass *type = sym->type;
      /* TODO member access */
      return type;
    }

    // SymbolClass *member_access(SymbolClass *left, LocalVar var) {/* TODO */}

    // SymbolClass *subscript(SymbolClass *left, LocalVar var) {/* TODO */}

    SymbolClass *num_const(NumConst num)
    {
      string_view lit = num.str();
      /* Convert to lowercase helper for easier suffix matching */
      string str;
      str.reserve(lit.size());
      for (char c : lit) {
        str.push_back(tolower(static_cast<unsigned char>(c)));
      }

      bool has_decimal = (str.find('.') != string_view::npos);
      bool has_exponent = !str.starts_with("0x") && (str.find('e') != string_view::npos);

      if (has_decimal || has_exponent || str.back() == 'f') {
        return float_cls;
      }
      if (str.back() == 'u') {
        return uint_cls;
      }
      return int_cls;
    }
  };
};

Result<SymbolClass *> SymbolTable::expr_type_analysis(const SymbolScope &scope, Expr expr) const
{
  ExpressionTypeParser parser;
  return parser.eval(*this, scope, expr);
}

Result<SymbolClass *> SymbolTable::resolve_auto_type(SymbolScope &scope, Declarator decl) const
{
  std::optional<AstNodeException> err;
  Node node = decl.child_last();
  if (node.type() == NodeType::InitializerList) {
    Initializer init(InitializerList(node).child_first());
    if (init.child_first().type() == NodeType::InitializerList) {
      err = {init.child_first(),
             "Cannot deduce type for variable '" + string(decl.identifier().str()) +
                 "' with type 'auto' from nested initializer list"};
    }
    else if (init.next().is_valid()) {
      err = {init.next(),
             "Initializer for variable '" + string(decl.identifier().str()) +
                 "' with type 'auto' contains multiple expressions"};
    }
    else {
      return expr_type_analysis(scope, Expr(init.child_first()));
    }
  }
  else {
    return expr_type_analysis(scope, AssignStmt(node).expr());
  }
  return {scope.root_scope()->lookup_class(SymbolTable::err_symbol), err};
}

/* Return size padded to the given alignment. */
static int pad(int size, int align)
{
  return ((size + align - 1) / align) * align;
}

Result<string> SymbolTable::mangle_identifier(TemplateArgList args,
                                              TemplateParamList list,
                                              const SymbolScope &scope,
                                              const string &sep)
{
  Expr param = list.child_first();

  std::optional<AstNodeException> err;

  string str;
  args.foreach_child([&](TemplateArg arg) {
    assert(arg.is_valid());
    if (!param.is_valid()) {
      err = AstNodeException(
          list, "Missing argument for template parameter '" + string(arg.type().str()) + "'");
    }
    else if (arg.front() == Typename) {
      if (param.child_count() != 1 || param.child_first().type() != NodeType::LocalVar) {
        err = AstNodeException(
            param, "Invalid argument for template parameter '" + string(arg.type().str()) + "'");
      }
      else {
        IdQualified type_id(LocalVar(param.child_first()).identifier());
        SymbolClass *type = scope.lookup_class(type_id);
        if (type->template_data) {
          TemplateParamList list_nested = type_id.template_params();
          if (!list_nested.is_valid()) {
            err = AstNodeException(type_id,
                                   "Use of undeclared identifier '" + string(type_id.str()) + "'");
          }
          else {
            /* Recursive. */
            auto [type_, err_] = type->template_data->lookup_inst(list_nested, scope);
            if (err_) {
              err = err_;
            }
            type = type_;
          }
        }
        if (type->is_error) {
          err = AstNodeException(param,
                                 "Use of undeclared identifier '" + string(type_id.str()) + "'");
        }
        else {
          str += sep + type->resolved->identifier;
        }
      }
    }
    else {
      auto [val, err_] = SymbolTable::evaluate_constexpr(scope, param.child_first());
      if (err_) {
        err = err_;
      }
      /* Replace minus sign by underscore. */
      str += sep + (val < 0 ? "_" : "") + to_string(abs(val));
    }
    param = param.next();
  });
  return {str, err};
}

template<typename T>
Result<T *> SymbolTemplate<T>::lookup_inst(TemplateParamList list, const SymbolScope &scope) const
{
  auto [id, err] = SymbolTable::mangle_identifier(decl.arguments(), list, scope);
  auto it = instances.find(id);
  if (it == instances.end() && !err) {
    if constexpr (is_same_v<T, SymbolFunction>) {
      err = {list, "No matching function for the given template parameters"};
    }
    else {
      err = {list, "No matching class for the given template parameters"};
    }
  }

  if (err) {
    if constexpr (is_same_v<T, SymbolFunction>) {
      return {scope.root_scope()->lookup_function(SymbolTable::err_symbol), err};
    }
    else {
      return {scope.root_scope()->lookup_class(SymbolTable::err_symbol), err};
    }
  }
  return {it->second, err};
}

template<typename T> void SymbolTemplate<T>::init_adl()
{
  TemplateDecl temp = this->decl;
  if (temp.is_class()) {
    return;
  }
  TemplateArgList tmp_args(temp.arguments());

  int tmp_arg_count = tmp_args.child_count();
  if (tmp_arg_count == 0) {
    return;
  }
  temp_arg_index_in_fn_arg.reserve(tmp_arg_count);

  FuncDecl decl(temp.decl());
  decl.arguments().foreach<FuncArg>([&](FuncArg arg) {
    int arg_id = -1;
    int id = 0;
    tmp_args.foreach<TemplateArg>([&](TemplateArg tmp_arg) {
      if (tmp_arg.id().str() == arg.type().id().str()) {
        arg_id = id;
      }
      ++id;
    });
    if (arg_id != -1) {
      temp_arg_index_in_fn_arg.emplace_back(arg_id);
    }
  });

  if (temp_arg_index_in_fn_arg.size() != tmp_arg_count) {
    temp_arg_index_in_fn_arg.clear();
  }
}

Result<string> SymbolTable::mangle_identifier(const SymbolFunctionTemplate &tmp,
                                              FuncParamList list,
                                              const SymbolScope &scope,
                                              const string &sep) const
{
  vector<SymbolClass *> arg_cls;
  list.foreach<Expr>([&](Expr expr) {
    auto [type, _] = expr_type_analysis(scope, expr);
    arg_cls.emplace_back(type);
  });

  string str;
  for (int i : tmp.temp_arg_index_in_fn_arg) {
    if (i < arg_cls.size()) {
      str += sep + arg_cls[i]->resolved->identifier;
    }
    else {
      str += sep + err_symbol;
    }
  }
  return {str, {}};
}

template<typename T>
Result<T *> SymbolTemplate<T>::lookup_adl(const SymbolTable &symbols,
                                          FuncParamList list,
                                          const SymbolScope &scope) const
{
  if constexpr (is_same_v<T, SymbolClass>) {
    return {scope.root_scope()->lookup_class(SymbolTable::err_symbol),
            AstNodeException(list, "Cannot use ADL on types")};
  }
  else {
    auto [id, err] = symbols.mangle_identifier(*this, list, scope);
    auto it = instances.find(id);
    if (it == instances.end() && !err) {
      err = {list, "No matching function for the given template parameters"};
    }

    if (err) {
      return {scope.root_scope()->lookup_function(SymbolTable::err_symbol), err};
    }
    return {it->second, err};
  }
}

template struct SymbolTemplate<SymbolClass>;
template struct SymbolTemplate<SymbolFunction>;

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
            parse_enum_value(scope, child, parent, prefix);
            break;
          case NodeType::FuncDecl:
            parse_func_decl(scope, child, prefix);
            break;
          case NodeType::VarDecl:
            parse_var_decl(scope, child, offset, prefix);
            break;
          case NodeType::StructuredBinding:
            parse_structured_binding(scope, child);
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
    Expr param = params.child_first();

    decl.arguments().foreach_child([&](TemplateArg arg) {
      assert(arg.is_valid());
      if (arg.front() == Typename) {
        IdQualified arg_id = arg.type();
        string id(arg_id.str());
        if (param.child_count() != 1 || param.child_first().type() != NodeType::LocalVar) {
          error(param, "Invalid argument for template parameter '" + id + "'");
          return;
        }
        IdQualified type_id(LocalVar(param.child_first()).identifier());
        SymbolClass *cls = table.cls_arena.alloc(&scope, arg.id().front(), id);
        SymbolClass *resolved = scope.lookup_class(type_id);
        cls->resolved = resolved;
        if (resolved->is_error) {
          error(param, "Unknown type in template instantiation");
        }
        scope.classes.emplace(id, cls);
      }
      else {
        IdQualified type = arg.type();
        string id(arg.id().str());
        SymbolClass *cls = scope.lookup_class(type);
        SymbolVariable *var = table.var_arena.alloc(&scope, cls, arg.id().front(), id);
        var->is_constexpr = true;
        auto [val, err] = SymbolTable::evaluate_constexpr(scope, param.child_first());
        if (err) {
          error(err->node, err->msg);
        }
        var->value = val;
        scope.variables.emplace(id, var);
      }
      param = param.next();
    });
  }

  /* Create declaration for each function argument so name lookup will work with these.
   * Also create declaration for `this_`. */
  void parse_function_arguments(SymbolScope &scope, FuncDecl decl)
  {
    SymbolFunction *fn_sym = static_cast<SymbolFunction *>(&scope);
    decl.arguments().foreach<FuncArg>([&](FuncArg arg) {
      SymbolClass *type = scope.lookup_class(arg.type().id());
      SymbolVariable var(&scope, type, arg.declarator());
      var.type = scope.lookup_class(arg.type().id());
      scope.variables.emplace(var.identifier, table.var_arena.alloc(var));
      /* Register argument type for argument resolution. */
      fn_sym->arg_types.emplace_back(type);
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

  void parse_enum_value(SymbolScope &scope, EnumValue val, ClassDecl cls, const string &prefix)
  {
    SymbolClass *enum_cls = scope.lookup_class(cls.identifier());
    SymbolClass *type = scope.lookup_class(cls.parent_class());
    SymbolVariable *sym = table.var_arena.alloc(&scope, type, val);
    sym->is_static = true;
    sym->is_constexpr = true;
    /* Resolve its value. */
    if (AssignStmt assign = val.value(); assign.is_valid()) {
      if (InitializerList list = assign.initializer_list(); list.is_valid()) {
        sym->value = eval_scalar_initializer_list(scope, list);
      }
      auto [val, err] = SymbolTable::evaluate_constexpr(scope, assign.expr().child_first());
      if (err) {
        error(err->node, err->msg);
      }
      sym->value = val;
    }
    else {
      sym->value = enum_cls->enum_last_val + 1;
    }
    scope.variables.emplace(sym->identifier, sym);
    /* Save value for the next member. */
    enum_cls->enum_last_val = sym->value;

    /* Instantiate in global resolved namespace. */
    {
      SymbolVariable *flat_sym = table.var_arena.alloc(*sym);
      sym->resolved = flat_sym;
      flat_sym->resolved = sym;
      flat_sym->identifier = prefix + sym->identifier;
      global.variables.try_emplace(flat_sym->identifier, flat_sym);
    }

    if (!cls.is_enum_class()) {
      /* Alias to the parent namespace for non-anonymous, non-class enum. */
      scope.parent->variables.emplace(sym->identifier, sym);
    }
  }

  SymbolClass *parse_class_decl(SymbolScope &scope,
                                ClassDecl decl,
                                ClassDecl parent_class,
                                int &offset,
                                const std::string &prefix,
                                const std::string &suffix = "",
                                TemplateInst temp = {},
                                SymbolClassTemplate *cls_template = nullptr)
  {
    SymbolClass *cls = table.cls_arena.alloc(&scope, decl, suffix);
    scope.classes.emplace(cls->identifier, cls);
    scope.scopes.emplace(cls->identifier, cls);

    if (cls->is_anonymous && anonymous_scope_prefix(cls).non_anonymous_parent == nullptr) {
      error(decl, "Anonymous unions at namespace or global scope are not supported");
      return scope.root_scope()->lookup_class(SymbolTable::err_symbol);
    }

    /* Instantiate in global resolved namespace. */
    SymbolClass *flat_cls = table.cls_arena.alloc(*cls);
    cls->resolved = flat_cls;
    flat_cls->resolved = cls;
    flat_cls->identifier = prefix + cls->identifier;
    global.classes.try_emplace(flat_cls->identifier, flat_cls);

    if (cls_template) {
      /* Add the template definition early so it can be queried during instantiation. */
      cls_template->instances.emplace(suffix, cls);
      /* Create an alias to the unspecified class name (no template arg): e.g. `A` > `A<T>`. */
      SymbolClass *alias = table.cls_arena.alloc(&scope, decl);
      alias->resolved = flat_cls;
      cls->classes.emplace(alias->identifier, alias);
    }

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
    return cls;
  }

  SymbolFunction *parse_func_decl(SymbolScope &scope,
                                  FuncDecl func,
                                  const std::string &prefix,
                                  const std::string &suffix = "",
                                  TemplateInst temp = {})
  {
    SymbolFunction *fn = table.fun_arena.alloc(&scope, func, suffix);
    scope.function_emplace(fn);
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
    /* Do not parse. Only keep the symbol definition. The instantiation will do the parsing. */
    if (decl.is_function()) {
      SymbolFunctionTemplate *temp = table.tmp_fun_arena.alloc(decl);
      SymbolFunction *fn = table.fun_arena.alloc(&scope, decl.decl());
      fn->template_data = temp;
      scope.functions.emplace(fn->identifier, fn);
    }
    else {
      SymbolClassTemplate *temp = table.tmp_cls_arena.alloc(decl);
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
        auto [arg_mangled,
              err] = SymbolTable::mangle_identifier(temp_decl.arguments(), temp_params, scope);
        if (err) {
          error(err->node, err->msg);
        }

        SymbolScope *parent = &scope;
        if (fn->fn_type == SymbolFunction::MEMBER) {
          parent = nullptr;
          /* Set the instantiated method parent to the container class. Note that it can also
           * be templated. So we need to resolve it too. Lookup partial identifier. */
          IdQualified id = decl.identifier();
          Id start = id.namespace_start();
          Id last = id.child_last(NodeType::Id).prev(NodeType::Id);
          if (SymbolClass *parent_cls = scope.lookup_class(start, last); parent_cls) {
            if (parent_cls->template_data) {
              if (TemplateParamList param = last.template_params(); param.is_valid()) {
                SymbolClass *base_type = parent_cls;
                auto [type_, _] = base_type->template_data->lookup_inst(param, scope);
                parent = type_;

                if (type_->is_error) {
                  auto [args, err] = SymbolTable::mangle_identifier(
                      base_type->template_data->decl.arguments(), param, scope, ", ");
                  if (err) {
                    error(err->node, err->msg);
                  }
                  else {
                    error(param,
                          "Missing explicit instantiation of template '" + base_type->identifier +
                              "<" + args.substr(2) + ">'");
                  }
                }
              }
              else {
                error(last, "Missing template parameters");
              }
            }
            else {
              parent = parent_cls;
            }
          }

          if (parent == nullptr) {
            error(id,
                  "Missing instantiation of parent class for instantiation of '" +
                      string(id.str()) + "'");
            return;
          }
        }
        SymbolFunction *fn_inst = parse_func_decl(
            *parent, temp_decl.decl(), prefix, arg_mangled, temp);
        fn->template_data->instances.emplace(arg_mangled, fn_inst);
      }
    }
    else {
      const ClassDecl decl(temp.decl());
      SymbolClass *cls = scope.lookup_class(decl.identifier());

      if (TemplateDecl temp_decl = check_template_instance(temp_params, decl, *cls);
          temp_decl.is_valid())
      {
        auto [arg_mangled,
              err] = SymbolTable::mangle_identifier(temp_decl.arguments(), temp_params, scope);
        if (err) {
          error(err->node, err->msg);
        }
        int unused_offset = 0;
        parse_class_decl(scope,
                         temp_decl.decl(),
                         Node{},
                         unused_offset,
                         prefix,
                         arg_mangled,
                         temp,
                         cls->template_data);
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
        auto [arg_mangled,
              err] = SymbolTable::mangle_identifier(temp_decl.arguments(), temp_params, scope);
        if (err) {
          error(err->node, err->msg);
        }
        SymbolFunction *spec = parse_func_decl(scope, decl, prefix, arg_mangled);
        fn->template_data->instances.emplace(arg_mangled, spec);
      }
    }
    else {
      const ClassDecl decl(temp.decl());
      SymbolClass *cls = scope.lookup_class(decl.identifier());

      if (TemplateDecl temp_decl = check_template_instance(temp_params, decl, *cls);
          temp_decl.is_valid())
      {
        auto [arg_mangled,
              err] = SymbolTable::mangle_identifier(temp_decl.arguments(), temp_params, scope);
        if (err) {
          error(err->node, err->msg);
        }
        int unused_offset = 0;
        SymbolClass *spec = parse_class_decl(
            scope, decl, Node{}, unused_offset, prefix, arg_mangled);
        cls->template_data->instances.emplace(arg_mangled, spec);
      }
    }
  }

  void parse_structured_binding(SymbolScope &scope, StructuredBinding decl)
  {
    AssignStmt assign = decl.assign();
    Expr expr = assign.expr();
    if (!expr.is_valid()) {
      error(decl, "Cannot deduce actual type for variable '' with type 'auto'");
      return;
    }

    auto [cls, err] = table.expr_type_analysis(scope, expr);
    if (err) {
      error(err->node, err->msg);
      return;
    }
    if (cls->is_error) {
      error(decl, "Cannot deduce actual type for variable '' with type 'auto'");
      return;
    }

    Id id = decl.child_first();

    /* Create temp variable to write to. */
    SymbolVariable *tmp = table.var_arena.alloc(&scope, cls, decl.front(), decl.tmp_id());
    scope.variables.emplace(tmp->identifier, tmp);

    cls->visit_variables([&](SymbolVariable &var) {
      if (!id.is_valid()) {
        error(decl, "Not enough names in structure binding");
        return;
      }
      /* Make copy of the variable in local scope. */
      SymbolVariable *sym = table.var_arena.alloc(var);
      sym->identifier = id.str();
      sym->parent = &scope;
      scope.variables.emplace(sym->identifier, sym);
      /* Resolve to the tmp variable member. */
      SymbolVariable *res = table.var_arena.alloc(var);
      res->identifier = tmp->identifier + '.' + var.identifier;
      res->parent = &scope;
      sym->resolved = res;

      id = id.next();
    });
    if (id.is_valid()) {
      error(decl, "Too many names in structure binding");
      return;
    }
  }

  void parse_var_decl(SymbolScope &scope, VarDecl var, int &offset, const std::string &prefix)
  {
    IdQualified type_id = var.type().id();
    string_view type_id_str = type_id.str();

    SymbolClass *type = nullptr;
    if (type_id_str == "auto") {
      /* Assumes C++ compilation already checked that all declarator uses the same type.
       * Deduce type using only the first declarator. */
      Declarator decl = var.child_first(NodeType::Declarator);
      auto [resolved_type, err] = table.resolve_auto_type(scope, decl);
      if (err) {
        error(err->node, err->msg);
      }
      type = resolved_type;
    }
    else {
      type = scope.lookup_class(type_id);
      if (type->template_data) {
        TemplateParamList param = type_id.template_params();

        SymbolClass *base_type = type;
        auto [type_, _] = base_type->template_data->lookup_inst(param, scope);
        type = type_;
        if (type->is_error) {
          auto [args, err] = SymbolTable::mangle_identifier(
              base_type->template_data->decl.arguments(), param, scope, ", ");
          if (err) {
            error(err->node, err->msg);
          }
          else {
            error(param,
                  "Missing explicit instantiation of template '" + base_type->identifier + "<" +
                      args.substr(2) + ">'");
          }
        }
      }
    }

    if (type->is_error) {
      error(var, "Unknown type name '" + string(type_id_str) + "'");
      return;
    }

    SymbolClass *cls = scope.as_class();

    string anon_prefix;
    SymbolScope *named_parent = nullptr;
    if (cls && cls->is_anonymous) {
      auto prefix = anonymous_scope_prefix(cls);
      anon_prefix = prefix.prefix, named_parent = prefix.non_anonymous_parent;
    }

    var.foreach<Declarator>([&](Declarator decl) {
      SymbolVariable *sym = table.var_arena.alloc(&scope, type, decl);
      scope.variables.emplace(sym->identifier, sym);

      if (cls) {
        sym->set_offset(cls->is_union, offset);

        /* Alias to the parent class for anonymous structs. */
        if (cls->is_anonymous) {
          /* Create a resolved symbol. */
          SymbolVariable *resolved = table.var_arena.alloc(&scope, type, decl);
          resolved->identifier = anon_prefix + resolved->identifier + "()";
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
      auto [val, err] = SymbolTable::evaluate_constexpr(scope, node.child_first());
      if (err) {
        error(err->node, err->msg);
      }
      return val;
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
      auto [val, err] = SymbolTable::evaluate_constexpr(scope, assign.expr().child_first());
      if (err) {
        error(err->node, err->msg);
      }
      return val;
    }
    if (InitializerList list = decl.initializer_list(); list.is_valid()) {
      return eval_scalar_initializer_list(scope, list);
    }
    error(decl,
          "Constexpr variable '" + string(decl.identifier().str()) +
              "' must be initialized by a constant expression");
    return 0;
  }

  struct AnonScopePrefix {
    string prefix;
    SymbolScope *non_anonymous_parent;
  };

  static AnonScopePrefix anonymous_scope_prefix(SymbolClass *cls)
  {
    string prefix;
    SymbolClass *parent = nullptr;
    /* Support multiple nesting level. */
    do {
      /* Union members need to go through the getter functions. */
      parent = cls->parent->as_class();
      string access = parent && parent->is_union && parent->is_anonymous ? "()" : "";
      prefix = cls->identifier + "_" + access + "." + prefix;
    } while ((cls = parent, cls && cls->is_anonymous));

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

Result<string> SymbolTable::expr_to_string(const SymbolScope &scope, Node start, int &node_count)
{
  std::optional<AstNodeException> error;
  string expr_str;
  for (Node child = start; child.is_valid(); child = child.next()) {
    switch (child.type()) {
      /* TODO member access. */
      case NodeType::LocalVar: {
        SymbolVariable *var = scope.lookup_variable(LocalVar(child).identifier());
        if (!var->is_constexpr) {
          return {
              "0",
              AstNodeException(
                  child, "Read of non-const variable is not allowed in a constant expression")};
        }
        expr_str += to_string(var->value);
        node_count++;
        break;
      }
      case NodeType::FuncCall:
        return {"0", AstNodeException(child, "Constexpr cannot contain function calls")};
      case NodeType::ExprSub: {
        auto [str, err] = expr_to_string(scope, ExprSub(child).expr().child_first(), node_count);
        expr_str += '(';
        expr_str += str;
        expr_str += ')';
        node_count++;
        if (err) {
          return {"0", err};
        }
        break;
      }
      case NodeType::Op:
      case NodeType::NumConst:
        expr_str += child.str();
        node_count++;
        break;
      default:
        return {"0", AstNodeException(child, "Unsupported symbol inside constexpr definition")};
    }
  }
  return {expr_str, error};
}

Result<int64_t> SymbolTable::evaluate_constexpr(const SymbolScope &scope, Node start)
{
  /* First build a string containing only numerical constants by substituting all the symbols. */
  int count = 0;
  auto [expr_str, error] = expr_to_string(scope, start, count);
  if (error) {
    return {0, error};
  }

  /* Fast path. */
  if (count == 1) {
    try {
      return {stoll(expr_str), {}};
    }
    catch (...) {
      return {0, AstNodeException(start, "Error parsing integer literal")};
    }
  }

  /* Then use the expression parser to evaluate it. */
  try {
    ExpressionParser expression_parser;
    expression_parser.lexical_analysis(expr_str);
    return {expression_parser.eval(), {}};
  }
  catch (const std::exception &e) {
    return {0,
            AstNodeException(start, "Failed to evaluate expanded expression '" + expr_str + "'")};
  }
}

Result<vector<SymbolClass *>> SymbolFunction::to_arg_types(const SymbolTable &table,
                                                           const SymbolScope &scope,
                                                           FuncParamList list)
{
  std::optional<AstNodeException> error;
  vector<SymbolClass *> arg_types;
  list.foreach<Expr>([&](Expr expr) {
    auto [cls, err] = table.expr_type_analysis(scope, expr);
    arg_types.emplace_back(cls);
    if (err && !error) {
      error = err;
    }
  });
  return {arg_types, error};
}

Result<vector<SymbolClass *>> SymbolFunction::to_arg_types(const SymbolTable & /*table*/,
                                                           const SymbolScope &scope,
                                                           FuncArgList list)
{
  vector<SymbolClass *> arg_types;
  list.foreach<FuncArg>([&](FuncArg arg) {
    SymbolClass *cls = scope.lookup_class(arg.type().id());
    arg_types.emplace_back(cls);
  });
  return {arg_types, std::nullopt};
}

SymbolFunction *SymbolFunction::lookup_overload(const vector<SymbolClass *> &arg_types)
{
  for (SymbolFunction *fn = this; fn; fn = fn->overload_next) {
    if (fn->argument_matches(arg_types)) {
      return fn;
    }
  }
  return root_scope()->lookup_function(SymbolTable::err_symbol);
}

bool SymbolFunction::argument_matches(const vector<SymbolClass *> &arg_types) const
{
  /* Use GLSL strict matching for now, no implicit conversion. */
  return this->arg_types == arg_types;
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

SymbolClass *SymbolScope::lookup_class(Id id, Id last) const
{
  return lookup_generic_nested<SymbolClass>(id, last, id.front());
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
  if (auto v = lookup_generic_nested<T>(id.namespace_start(), id.child_last(NodeType::Id), loc)) {
    return v;
  }
  /* Delegate to parent scope if not found locally. */
  if (parent) {
    return parent->lookup_generic<T>(id, loc);
  }
  /* Lookup failure at root level, try to return error symbol. */
  if constexpr (is_same_v<T, SymbolFunction>) {
    if (auto it = functions.find(SymbolTable::err_symbol); it != functions.end()) {
      return it->second;
    }
  }
  else if constexpr (is_same_v<T, SymbolClass>) {
    if (auto it = classes.find(SymbolTable::err_symbol); it != classes.end()) {
      return it->second;
    }
  }
  else if constexpr (is_same_v<T, SymbolVariable>) {
    if (auto it = variables.find(SymbolTable::err_symbol); it != variables.end()) {
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

template<typename T>
T *SymbolScope::lookup_generic_nested(Id id, Id last, const SourceLocation &loc) const
{
  /* Look up anonymous namespace first. */
  if (auto it = scopes.find(""); it != scopes.end()) {
    Id next_search_id = id.id != last.id ? id.next_id() : id;
    if (auto ret = it->second->lookup_generic_nested<T>(next_search_id, last, loc)) {
      return ret;
    }
  }
  /* Resolve target based on whether it's a namespace or the final symbol. */
  if (id.id != last.id) {
    /* Only classes can have template specifiers. */
    if (TemplateParamList list = id.template_params(); list.is_valid()) {
      if (auto it = classes.find(string(id.str())); it != classes.end()) {
        if (SymbolClass *cls = it->second->as_class(); cls && cls->template_data) {
          if (auto [tmp, err] = cls->template_data->lookup_inst(list, *this); !tmp->is_error) {
            return tmp->lookup_generic_nested<T>(id.next_id(), last, loc);
          }
        }
      }
      return nullptr;
    }
    if (auto it = scopes.find(string(id.str())); it != scopes.end()) {
      return it->second->lookup_generic_nested<T>(id.next_id(), last, loc);
    }
    return nullptr;
  }

  if constexpr (is_same_v<T, SymbolFunction>) {
    if (auto it = functions.find(string(id.str())); it != functions.end()) {
      if (it->second->loc <= loc) {
        // if (TemplateParamList list = id.template_params(); list.is_valid()) {
        //   return (it->second->template_data) ? it->second : nullptr;
        // }
        return it->second;
      }
    }
  }
  else if constexpr (is_same_v<T, SymbolClass>) {
    if (auto it = classes.find(string(id.str())); it != classes.end()) {
      if (it->second->loc <= loc) {
        // if (TemplateParamList list = id.template_params(); list.is_valid()) {
        //   return (it->second->template_data) ? it->second : nullptr;
        // }
        return it->second;
      }
    }
  }
  else if constexpr (is_same_v<T, SymbolVariable>) {
    if (auto it = variables.find(string(id.str())); it != variables.end()) {
      if (it->second->loc <= loc) {
        if (TemplateParamList list = id.template_params(); list.is_valid()) {
          return nullptr; /* Variable cannot be templated (only their types can). */
        }
        return it->second;
      }
    }
  }
  else {
    static_assert(false);
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

SymbolVariable *SymbolScope::lookup_variable(string id) const
{
  return variables.find(id)->second;
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

  struct BuiltinBasicType {
    string id;
    size_t size;
    size_t align;
  };

  const vector<BuiltinBasicType> types = {
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
      {"float2", "float", 2, 8},
      {"float3", "float", 3, 16},
      {"float4", "float", 4, 16},
      {"float2x2", "float2", 2, 16},
      {"float2x3", "float3", 2, 16},
      {"float2x4", "float4", 2, 16},
      {"float3x2", "float2", 3, 16},
      {"float3x3", "float3", 3, 16},
      {"float3x4", "float4", 3, 16},
      {"float4x2", "float2", 4, 16},
      {"float4x3", "float3", 4, 16},
      {"float4x4", "float4", 4, 16},
      {"int2", "int", 2, 8},
      {"int3", "int", 3, 16},
      {"int4", "int", 4, 16},
      {"uint2", "uint", 2, 8},
      {"uint3", "uint", 3, 16},
      {"uint4", "uint", 4, 16},
      {"bool2", "bool", 2, 2},
      {"bool3", "bool", 3, 3},
      {"bool4", "bool", 4, 4},
      {"packed_float2", "float", 2, 8},
      {"packed_float3", "float", 3, 12},
      {"packed_float4", "float", 4, 16},
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

  const std::vector<BuiltinFunc> functions = {
      /* Error variable symbol. */
      {"void", err_symbol, {}},

      {"float3", "reflect", {"float3", "float3"}},
      {"float3", "refract", {"float3", "float3", "float"}},

      {"bool", "greaterThan", {"float3", "float3"}},
      {"bool", "lessThan", {"float3", "float3"}},
      {"bool", "lessThanEqual", {"float3", "float3"}},
      {"bool", "greaterThanEqual", {"float3", "float3"}},
      {"bool", "equal", {"float3", "float3"}},
      {"bool", "notEqual", {"float3", "float3"}},

      {"bool3", "not", {"bool3"}},
      {"bool3", "any", {"bool3"}},
      {"bool", "all", {"bool3"}},

      {"int", "bitCount", {"int"}},
      {"uint", "bitCount", {"uint"}},
      {"int", "bitfieldExtract", {"int"}},
      {"uint", "bitfieldExtract", {"uint"}},
      {"int", "bitfieldInsert", {"int"}},
      {"uint", "bitfieldInsert", {"uint"}},
      {"int", "bitfieldReverse", {"int"}},
      {"uint", "bitfieldReverse", {"uint"}},

      {"int", "atomicAdd", {"int", "int"}},
      {"int", "atomicAnd", {"int", "int"}},
      {"int", "atomicOr", {"int", "int"}},
      {"int", "atomicXor", {"int", "int"}},
      {"int", "atomicMin", {"int", "int"}},
      {"int", "atomicMax", {"int", "int"}},
      {"int", "atomicExchange", {"int", "int"}},
      {"int", "atomicCompSwap", {"int", "int", "int"}},
      {"uint", "atomicAdd", {"uint", "uint"}},
      {"uint", "atomicAnd", {"uint", "uint"}},
      {"uint", "atomicOr", {"uint", "uint"}},
      {"uint", "atomicXor", {"uint", "uint"}},
      {"uint", "atomicMin", {"uint", "uint"}},
      {"uint", "atomicMax", {"uint", "uint"}},
      {"uint", "atomicExchange", {"uint", "uint"}},
      {"uint", "atomicCompSwap", {"uint", "uint", "uint"}},
      {"uint", "packHalf2x16", {"float2"}},
      {"uint", "packUnorm2x16", {"float2"}},
      {"uint", "packSnorm2x16", {"float2"}},
      {"uint", "packUnorm4x8", {"float4"}},
      {"uint", "packSnorm4x8", {"float4"}},
      {"float2", "unpackHalf2x16", {"uint"}},
      {"float2", "unpackUnorm2x16", {"uint"}},
      {"float2", "unpackSnorm2x16", {"uint"}},
      {"float4", "unpackUnorm4x8", {"uint"}},
      {"float4", "unpackSnorm4x8", {"uint"}},
  };
  /* Builtin functions. */
  for (auto f : functions) {
    SymbolFunction *sym = fun_arena.alloc(root, tok, string(f.id), SymbolFunction::GLOBAL);
    sym->is_error = f.id == err_symbol;
    sym->return_type = root->lookup_class(f.return_type);
    SymbolFunction *sym_resolved = fun_arena.alloc(*sym);
    sym->resolved = sym_resolved;
    root->functions.emplace(sym->identifier, sym);
    flatten_root->functions.emplace(sym->identifier, sym_resolved);
  }

  /* Builtin operators. */

  const unordered_map<TokenType, const char *> unary_type_to_id = {
      {Plus, "pos_op_"},
      {Minus, "neg_op_"},
      {BitwiseNot, "bitnot_op_"},
      {Not, "not_op_"},
  };

  const unordered_map<TokenType, const char *> binary_type_to_id = {
      {Plus, "add_op_"},
      {Minus, "sub_op_"},
      {Multiply, "mul_op_"},
      {Divide, "div_op_"},
      {Modulo, "mod_op_"},
      {And, "bitand_op_"},
      {Or, "bitor_op_"},
      {Xor, "bitxor_op_"},
      {LShift, "lshift_op_"},
      {RShift, "rshift_op_"},
      {Equal, "eq_op_"},
      {NotEqual, "neq_op_"},
      {LThan, "lte_op_"},
      {GThan, "gte_op_"},
      {LEqual, "leq_op_"},
      {GEqual, "geq_op_"},
      {LogicalAnd, "and_op_"},
      {LogicalOr, "or_op_"},
  };

  const vector<BuiltinType> builtin_types = generate_builtin_types();

  const vector<BuiltinOp> operators_desc = generate_all_operators(builtin_types);
  for (const auto &op : operators_desc) {
    string id((op.left.empty() ? unary_type_to_id : binary_type_to_id).find(op.op)->second);
    SymbolFunction *sym = fun_arena.alloc(root, tok, id, SymbolFunction::GLOBAL);
    sym->return_type = root->lookup_class(op.result);
    SymbolClass *left = op.left.empty() ? nullptr : root->lookup_class(op.left);
    SymbolClass *right = root->lookup_class(op.right);

    OperatorKey key(left, op.op, right);
    operators.emplace(key, sym);
  }

  const vector<BuiltinFunc> builtin_constructors = generate_all_constructors(builtin_types);
  for (const auto &fn : builtin_constructors) {
    SymbolFunction *sym = fun_arena.alloc(root, tok, fn.id, SymbolFunction::GLOBAL);
    sym->return_type = root->lookup_class(fn.return_type);
    for (auto arg : fn.arg_types) {
      sym->arg_types.emplace_back(root->lookup_class(arg));
    }
    sym->resolved = sym;
    root->function_emplace(sym);
  }

  struct BuiltinConst {
    string id;
    string type;
    bool is_constexpr;
    int value;
  };

  const std::vector<BuiltinConst> consts = {
      /* Error variable symbol. */
      {err_symbol, err_symbol, false, 1},

      {"true", "bool", true, 1},
      {"false", "bool", true, 0},
  };
  /* Boolean constants. */
  for (auto c : consts) {
    SymbolVariable *sym = var_arena.alloc(root, root->lookup_class(c.type), tok, string(c.id));
    SymbolVariable *sym_resolved = var_arena.alloc(*sym);
    sym->is_error = c.id == err_symbol;
    sym->resolved = sym_resolved;
    sym->is_static = c.is_constexpr;
    sym->is_constexpr = c.is_constexpr;
    sym->value = c.value;
    root->variables.emplace(sym->identifier, sym);
    flatten_root->variables.emplace(sym->identifier, sym_resolved);
  }
}

}  // namespace blender::gpu::shader::parser
