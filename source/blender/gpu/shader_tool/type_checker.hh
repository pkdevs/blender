/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "token.hh"
#include "token_stream.hh"

#include "symbol.hh"

namespace blender::gpu::shader::parser {

/**
 * Type resolution parser.
 * Will evaluate each operand type and return the type of the expression result.
 */
struct ExpressionTypeParser {
  /* First temporary inducing operator. */
  Node first_temporary = {};
  /* First variable not declared const. */
  Node first_non_const_var_in_subscript = {};
  /* First operator creating side effect. */
  Node first_side_effect_op = {};
  /* First function call. */
  Node first_function_call = {};

 private:
  std::optional<AstNodeException> err;
  const SymbolTable *table;
  const SymbolScope *scope;
  Node start;
  Node node;
  SymbolClass *float_cls;
  SymbolClass *int_cls;
  SymbolClass *uint_cls;
  SymbolClass *str_cls;
  SymbolClass *err_cls;

  /* Array dimension of the current symbol. */
  int array_dim = 0;

  bool is_subscript_expr = false;

 public:
  ExpressionTypeParser(const SymbolTable *table, const SymbolScope *scope, Node expr_first_child)
      : table(table), scope(scope), start(expr_first_child), node(expr_first_child)
  {
    const SymbolScope &root = *scope->root_scope();
    float_cls = root.lookup_class("float");
    int_cls = root.lookup_class("int");
    uint_cls = root.lookup_class("uint");
    // str_cls = root.lookup_class("string");
    err_cls = root.lookup_class(SymbolTable::err_symbol);
  }

  Result<SymbolClass *> eval()
  {
    SymbolClass *cls = expr(0);

    if (array_dim > 0) {
      error(start, "Expression result is of array type");
    }

    if (peek().is_valid() && !err) {
      error(peek(), "Trailing input");
    }
    return {cls, err};
  }

  SymbolClass *expr(int right_binding_power)
  {
    /* Parse unary operator, evaluate parenthesis, evaluate constant. */
    SymbolClass *left = nud(consume());

    /* While left binding power is greater than the right, continue consuming binary operations.
     */
    while (node.is_valid() && left_binding_power(peek()) > right_binding_power) {
      left = led(left, consume());
    }
    return left;
  }

  /* How a token evaluates without left context (e.g. unary operator).
   * Also known as Null-Denotation or NUD. */
  SymbolClass *nud(ast::Node node)
  {
    /* Unary operators must have the highest precedence after postfix and member access. */
    static constexpr int unary_binding_power = 120;

    switch (node.type()) {
      // case NodeType::TemplateExplicit: /* Should have already been removed. */
      case NodeType::FuncCall:
        return fun_call(node);
      case NodeType::Constructor:
        return constructor(node);
      case NodeType::StringConst:
        return str_cls;
      case NodeType::LocalVar:
        return local_var(node);
      case NodeType::NumConst:
        return num_const(node);
      case NodeType::Op:
        return op(node, expr(unary_binding_power));
      case NodeType::ExprSub: {
        ExpressionTypeParser ctx(table, scope, ExprSub(node).expr().child_first());
        ctx.is_subscript_expr = this->is_subscript_expr;
        auto [cls, err] = ctx.eval();
        error(err);
        merge_errors(ctx, false);
        return cls;
      }
      default:
        error(node.is_valid() ? node : start, "Invalid expression");
        this->node = Node(start.p, -1);
        return err_cls;
    }
  }

  /* How a token evaluates from left-to-right, on two operands.
   * Also known as Left-Denotation or LED. */
  SymbolClass *led(SymbolClass *left, Node node)
  {
    /* Subscript operator. */
    if (node.type() == NodeType::Subscript) {
      if (array_dim > 0) {
        array_dim -= 1;
        check_subscript(node);
        return left;
      }
      /* Call subscript operator */
      if (left->operator_subscript) {
        check_subscript(node);
        return left->operator_subscript->return_type;
      }
      error(node, "Subscripted value is not an array or vector");
      return left;
    }

    if (array_dim > 0) {
      error(node, "Invalid operand of array type");
    }

    if (node.type() != NodeType::Op) {
      error(node, "Invalid operator for type '" + left->resolved->identifier + "'");
    }

    /* Member access operator. */
    if (node.front() == Dot) {
      return member_access(left, consume());
    }

    /* Post-fix operator. */
    if (node.front() == Increment || node.front() == Decrement) {
      if (!first_temporary.is_valid()) {
        first_temporary = node;
      }
      /* TODO operator. */
      return left;
    }

    /* Binary operator. */
    if (node.front() != '?') {
      if (!first_temporary.is_valid()) {
        first_temporary = node;
      }
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
      error(node, "Incompatible operand types");
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
      case Increment:
      case Decrement:
      case SquareOpen:
      case Dot:
        return 130;
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
    error(peek(), "Invalid operator token '" + to_str(type) + "'");
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
    TokenType tok_type = operator_type.front().type();
    OperatorKey key{left_type, tok_type, right_type};
    if (auto it = table->operators.find(key); it != table->operators.end()) {
      return it->second->return_type;
    }
    error(operator_type,
          "Invalid operands to binary expression ('" + left_type->identifier + "' " +
              to_str(tok_type) + " '" + right_type->identifier + "')");
    return err_cls;
  }

  SymbolClass *op(Node operator_type, SymbolClass *right_type)
  {
    TokenType tok_type = operator_type.front().type();
    if (tok_type == Increment || tok_type == Decrement) {
      if (!first_side_effect_op.is_valid()) {
        first_side_effect_op = operator_type;
      }
    }
    OperatorKey key{nullptr, tok_type, right_type};
    if (auto it = table->operators.find(key); it != table->operators.end()) {
      return it->second->return_type;
    }
    error(operator_type,
          "Invalid argument type '" + right_type->identifier + "' to unary expression (" +
              to_str(tok_type) + "'" + right_type->identifier + "')");
    return err_cls;
  }

  SymbolClass *member_access(SymbolClass *left, ast::Node member)
  {
    switch (member.type()) {
      case NodeType::FuncCall: {
        FuncCall fun = member;
        if (!first_function_call.is_valid()) {
          first_function_call = fun;
        }
        auto [sym, err] = left->lookup_function(fun.identifier());
        error(err);
        return sym->return_type;
      }
      case NodeType::LocalVar: {
        LocalVar var = member;
        SymbolVariable *sym = left->lookup_variable(var.identifier());
        if (!sym) {
          error(node, "Unknown member '" + string(var.identifier().str()) + "'");
          return err_cls;
        }
        if (sym->array_dimensions) {
          array_dim = sym->array_dimensions;
        }
        return sym->type;
      }
      default:
        error(node.is_valid() ? node : start, "Invalid expression");
        this->node = Node{};
        return err_cls;
    }
  }

  SymbolClass *constructor(Constructor ctor)
  {
    if (!first_temporary.is_valid()) {
      this->first_temporary = ctor;
    }
    auto [cls, err] = scope->lookup_class(ctor.id());
    error(err);
    return cls;
  }

  SymbolClass *fun_call(FuncCall call)
  {
    if (call.identifier().str() == "resource_table_get") {
      /* Special getter that creates a resource table link to the function.
       * This is to help the porting process and should be phased out at some point. */
      FuncParamList params = call.parameters();
      if (params.child_count() != 1) {
        error(params, "Expecting one argument");
        return err_cls;
      }
      Expr expr(params.child_first());
      if (params.child_count() != 1) {
        error(params, "Expecting one typename");
        return err_cls;
      }
      LocalVar var(expr.child_first());
      if (params.child_count() != 1) {
        error(params, "Expecting one typename");
        return err_cls;
      }
      auto [type, err] = scope->lookup_class(var.identifier());
      error(err);
      if (!type->is_srt()) {
        error(params, "Expecting a resource table typename");
        return err_cls;
      }
      return type;
    }

    if (!first_function_call.is_valid()) {
      first_function_call = call;
    }
    auto [fn, err_fn] = scope->lookup_function(call.identifier(), call.parameters(), *table);
    error(err_fn);
    return fn->return_type;
  }

  SymbolClass *local_var(LocalVar var)
  {
    SymbolVariable *sym = scope->lookup_variable(var.identifier());
    if (!sym) {
      error(var, "Unknown identifier '" + string(var.identifier().str()) + "'");
      return err_cls;
    }
    if (!sym->is_const && is_subscript_expr && !first_non_const_var_in_subscript.is_valid()) {
      first_non_const_var_in_subscript = var;
    }
    if (sym->array_dimensions) {
      array_dim = sym->array_dimensions;
    }
    return sym->type;
  }

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

 private:
  void error(ast::Node node, const string &msg)
  {
    /* Only log the first error. */
    if (!err) {
      err = {node, msg};
    }
  }

  /* Pipe error if existing. */
  void error(std::optional<AstNodeException> &err)
  {
    if (err) {
      error(err->node, err->msg);
    }
  }

  void check_subscript(Node node)
  {
    ExpressionTypeParser ctx(table, scope, Subscript(node).expr().child_first());
    ctx.is_subscript_expr = true;
    auto [type, err] = ctx.eval();
    error(err);
    if (type->resolved != int_cls->resolved && type->resolved != uint_cls->resolved) {
      error(node, "Array subscript is not an integer");
    }
    /* Discard checking for temporary. It is allowed to have temporary inside subscript
     * expressions as long as we can be sure they are constant. */
    merge_errors(ctx, true);
  }

  void merge_errors(const ExpressionTypeParser &ctx, bool allow_temporary)
  {
    if (!first_temporary.is_valid() && !allow_temporary) {
      this->first_temporary = ctx.first_temporary;
    }
    if (!first_non_const_var_in_subscript.is_valid()) {
      this->first_non_const_var_in_subscript = ctx.first_non_const_var_in_subscript;
    }
    if (!first_side_effect_op.is_valid()) {
      this->first_side_effect_op = ctx.first_side_effect_op;
    }
    if (!first_function_call.is_valid()) {
      this->first_function_call = ctx.first_function_call;
    }
  }
};

}  // namespace blender::gpu::shader::parser
