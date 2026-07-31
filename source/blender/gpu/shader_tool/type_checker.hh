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

  /* Array dimension of the current symbol. */
  int array_dim = 0;

  bool is_subscript_expr = false;

 public:
  ExpressionTypeParser(const SymbolTable *table, const SymbolScope *scope, Node expr_first_child)
      : table(table), scope(scope), start(expr_first_child), node(expr_first_child)
  {
  }

  Result<SymbolClass *> eval()
  {
    SymbolClass *cls = expr(0);

    if (array_dim > 0) {
      error(start, Diag::ArrayTypeTemporary);
    }

    if (peek().is_valid() && !err) {
      error(peek(), Diag::ParserTrailingInput);
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
        return table->str_cls;
      case NodeType::LocalVar:
        return local_var(node);
      case NodeType::NumConst:
        return num_const(node);
        return expr(unary_binding_power);
      case NodeType::Op:
        return op(node, expr(unary_binding_power));
      case NodeType::Expr: {
        ExpressionTypeParser ctx(table, scope, Expr(node).child_first());
        ctx.is_subscript_expr = this->is_subscript_expr;
        SymbolClass *cls = ctx.eval().unwrap(err);
        merge_errors(ctx, false);
        return cls;
      }
      case NodeType::ExprSub: {
        ExpressionTypeParser ctx(table, scope, ExprSub(node).expr().child_first());
        ctx.is_subscript_expr = this->is_subscript_expr;
        SymbolClass *cls = ctx.eval().unwrap(err);
        merge_errors(ctx, false);
        return cls;
      }
      default:
        error(node.is_valid() ? node : start, Diag::InvalidExprTypeChecker);
        this->node = Node(start.p, -1);
        return table->err_cls;
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
      error(node, Diag::InvalidSubscript);
      return left;
    }

    if (array_dim > 0) {
      error(node, Diag::ArrayTypeOperand);
    }

    if (node.type() != NodeType::Op) {
      error(node, Diag::TypeInvalidOperator, left->identifier);
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
      error(node, Diag::TypeInvalidTernary, tval->original, fval->original);
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
    error(peek(), Diag::TokenInvalidOperator, to_str(type));
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
          Diag::InvalidBinaryOperands,
          left_type->identifier,
          to_str(tok_type),
          right_type->identifier);
    return table->err_cls;
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
    error(operator_type, Diag::InvalidUnaryArgumentType, to_str(tok_type), right_type->identifier);
    return table->err_cls;
  }

  SymbolClass *member_access(SymbolClass *left, ast::Node member)
  {
    switch (member.type()) {
      case NodeType::FuncCall: {
        FuncCall call = member;
        if (!first_function_call.is_valid()) {
          first_function_call = call;
        }
        SymbolFunction *sym = left->lookup_function(
                                      call.identifier(), call.parameters(), *table, *scope)
                                  .unwrap(err);
        return sym->return_type;
      }
      case NodeType::LocalVar: {
        LocalVar var = member;
        SymbolVariable *sym = left->lookup_variable(var.identifier());
        if (!sym) {
          error(node, Diag::UnknownMember, string(var.identifier().str()));
          return table->err_cls;
        }
        if (sym->array_dimensions) {
          array_dim = sym->array_dimensions;
        }
        return sym->type;
      }
      default:
        error(node.is_valid() ? node : start, Diag::TypeCheckerInvalidExpression);
        this->node = Node{};
        return table->err_cls;
    }
  }

  SymbolClass *constructor(Constructor ctor)
  {
    if (!first_temporary.is_valid()) {
      this->first_temporary = ctor;
    }
    return scope->lookup_class(ctor.id()).unwrap(err);
  }

  SymbolClass *fun_call(FuncCall call)
  {
    if (call.identifier().str() == "resource_table_get") {
      /* Special getter that creates a resource table link to the function.
       * This is to help the porting process and should be phased out at some point. */
      FuncParamList params = call.parameters();
      if (params.child_count() != 1) {
        error(params, Diag::ExpectedOneArgument);
        return table->err_cls;
      }
      Expr expr(params.child_first());
      if (params.child_count() != 1) {
        error(params, Diag::ExpectedOneTypename);
        return table->err_cls;
      }
      LocalVar var(expr.child_first());
      if (params.child_count() != 1) {
        error(params, Diag::ExpectedOneTypename);
        return table->err_cls;
      }
      SymbolClass *type = scope->lookup_class(var.identifier()).unwrap(err);
      if (!type->is_srt()) {
        error(params, Diag::ExpectedResourceTableTypename);
        return table->err_cls;
      }
      return type;
    }

    if (!first_function_call.is_valid()) {
      first_function_call = call;
    }
    SymbolFunction *fn =
        scope->lookup_function(call.identifier(), call.parameters(), *table, *scope).unwrap(err);
    return fn->return_type;
  }

  SymbolClass *local_var(LocalVar var)
  {
    SymbolVariable *sym = scope->lookup_variable(var.identifier());
    if (!sym) {
      error(var, Diag::UnknownIdentifier, string(var.identifier().str()));
      return table->err_cls;
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
    return table->get_literal_type(num.str());
  }

 private:
  void error(ast::Node node,
             Diag diag,
             const string &msg = "",
             const string &msg2 = "",
             const string &msg3 = "")
  {
    /* Only log the first error. */
    if (!err) {
      err = {node, diag, msg, msg2, msg3};
    }
  }

  /* Pipe error if existing. */
  void error(std::optional<AstNodeException> &err)
  {
    if (err) {
      error(err->node, err->diag, err->param1, err->param2, err->param3);
    }
  }

  void check_subscript(Node node)
  {
    ExpressionTypeParser ctx(table, scope, Subscript(node).expr().child_first());
    ctx.is_subscript_expr = true;
    SymbolClass *type = ctx.eval().unwrap(err);
    if (type != table->int_cls && type != table->uint_cls) {
      error(node, Diag::SubscriptNotInt);
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
