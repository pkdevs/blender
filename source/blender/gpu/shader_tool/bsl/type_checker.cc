/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "pratt_parser.hh"
#include "symbol_table.hh"
#include "type_checker.hh"

namespace bsl {

/**
 * Type resolution parser.
 * Will evaluate each operand type and return the type of the expression result.
 */
struct ExpressionTypeParser
    : public PrattParser<ExpressionTypeParser, ExpressionResult, ast::Node> {
  using Base = PrattParser<ExpressionTypeParser, ExpressionResult, ast::Node>;
  using ErrorType = Base::ErrorType;
  using BindingPower = Base::BindingPower;

 private:
  std::optional<AstNodeException> err;
  const SymbolTable *table;
  const SymbolScope *scope;
  ast::Node start;
  ast::Node node;

  /* The expression is supposed to be bound to a reference variable. */
  bool is_reference;

 public:
  ExpressionTypeParser(const SymbolTable *table,
                       const SymbolScope *scope,
                       ast::Node expr_first_child,
                       bool is_reference = false)
      : table(table),
        scope(scope),
        start(expr_first_child),
        node(expr_first_child),
        is_reference(is_reference)
  {
  }

  Result<ExpressionResult> eval()
  {
    ExpressionResult result = expr(0);

    if (peek() != Invalid && !err) {
      error(consume(), Diag::ParserTrailingInput);
    }
    return {result, err};
  }

  /**
   * CRTP Implementation
   */

  ExpressionResult identifier(const ast::Node &t)
  {
    switch (t.type()) {
      case ast::NodeType::Constructor: {
        ast::Constructor ctor = t;
        return scope->lookup_class(*table, ctor.id()).unwrap(err);
      }
      case ast::NodeType::FuncCall: {
        ast::FuncCall call = t;
        if (is_reference) {
          error(t, Diag::FunctionCallNotAllowedInReferenceDefinition);
        }
        auto sym = scope->lookup_function(*table, call.identifier(), call.parameters(), *scope);
        return sym.unwrap(err)->return_type;
      }
      case ast::NodeType::LocalVar: {
        ast::LocalVar var = t;
        SymbolVariable *sym = scope->lookup_variable(*table, var.identifier());
        if (sym) {
          return sym;
        }
        error(t, Diag::UnknownMember, std::string(var.identifier().str()));
        return table->err_cls;
      }
      default:
        error(t, Diag::TypeCheckerInvalidExpression);
        return table->err_cls;
    }
  }

  ExpressionResult number(const ast::Node &t)
  {
    SymbolClass *type = table->get_literal_type(t.str());

    ConstexprValue value;
    try {
      std::string str_val(t.str());

      if (type == table->float_cls) {
        value = std::stof(str_val);
      }
      else if (type == table->int_cls) {
        value = std::stoi(str_val, nullptr, 0);
      }
      else if (type == table->uint_cls) {
        value = uint(std::stoull(str_val, nullptr, 0));
      }
      else if (type == table->bool_cls) {
        value = str_val == "true";
      }
      else {
        throw std::runtime_error("Invalid Type");
      }
    }
    catch (...) {
      error(t, Diag::InvalidNumberLiteral);
      return table->err_cls;
    }
    return {type, value, /* is_temporary */ false};
  }

  ExpressionResult string(const ast::Node & /*t*/)
  {
    return table->str_cls;
  }

  ExpressionResult parenthesis(const ast::Node &t, BindingPower p)
  {
    /* Enter sub-expression */
    this->node = ast::ExprSub(t).expr().child_first();
    /* Parse the whole parenthesis expression. */
    ExpressionResult v = expr(p);
    /* Exit sub-expression. */
    this->node = t.next();
    return v;
  }

  ExpressionResult member(ExpressionResult left, const ast::Node &t)
  {
    SymbolClass *l = left.type;
    switch (t.type()) {
      case ast::NodeType::FuncCall: {
        ast::FuncCall call = t;
        if (is_reference) {
          error(t, Diag::FunctionCallNotAllowedInReferenceDefinition);
        }
        auto sym = l->lookup_function(*table, call.identifier(), call.parameters(), *scope);
        return sym.unwrap(err)->return_type;
      }
      case ast::NodeType::LocalVar: {
        ast::LocalVar var = t;
        SymbolVariable *sym = l->lookup_variable(*table, var.identifier());
        if (sym) {
          return sym;
        }
        error(t, Diag::UnknownMember, std::string(var.identifier().str()));
        return table->err_cls;
      }
      default:
        error(t, Diag::TypeCheckerInvalidExpression);
        this->node = ast::Node{};
        return table->err_cls;
    }
  }

  ExpressionResult function_call(ExpressionResult left, const ast::Node &t)
  {
    if (is_reference) {
      error(t, Diag::FunctionCallNotAllowedInReferenceDefinition);
    }
    error(t, Diag::CalledIsNotFunction, left.type->identifier);
    return table->err_cls;
  }

  ExpressionResult subscript(ExpressionResult left, const ast::Node &t)
  {
    /* Subscript operator. */
    if (ast::Subscript sub = t; sub.is_valid()) {
      if (left.array_dim > 0) {
        ExpressionResult arg = subscript_parameter(sub);

        if (table->get_conversion_rank(arg.type, table->int_cls) <= MatchRank::Promotion) {
          error(t, Diag::SubscriptNotInt);
        }
        if (is_reference && !(arg.flags & ExprFlag::IsConstant)) {
          error(t, Diag::ReferenceCannotBindNonConstSubscript);
        }

        left.array_dim -= 1;
        return left;
      }
      /* Call subscript operator */
      if (left.type->operator_subscript) {
        ExpressionResult arg = subscript_parameter(sub);
        if (table->get_conversion_rank(arg.type, table->int_cls) <= MatchRank::Promotion) {
          error(t, Diag::SubscriptNotInt);
        }
        return left.type->operator_subscript->return_type;
      }
    }
    error(t, Diag::InvalidSubscript);
    return table->err_cls;
  }

  ExpressionResult subscript_parameter(ast::Subscript &sub)
  {
    /* Enter sub-expression */
    this->node = sub.expr().child_first();
    /* Parse the whole parenthesis expression. */
    ExpressionResult v = expr(0);
    /* Exit sub-expression. */
    this->node = sub.next();
    return v;
  }

  ExpressionResult prefix(const ast::Node &t, BindingPower p)
  {
    ExpressionResult right = expr(p);

    if (right.array_dim > 0) {
      error(node, Diag::ArrayTypeOperand);
      return table->err_cls;
    }

    TokenType op_type = t.front().type();

    auto it = table->operators.find({nullptr, op_type, right.type});
    if (it == table->operators.end()) {
      error(t, Diag::InvalidUnaryArgumentType, to_str(op_type), right.type->identifier);
      return table->err_cls;
    }

    SymbolClass *type = it->second->return_type;

    if (is_reference && (op_type == Increment || op_type == Decrement)) {
      error(t, Diag::ReferenceCannotBindSideEffect);
      return table->err_cls;
    }

    ExprFlag flags = ExprFlag(right.flags);
    if (flags & ExprFlag::IsConstexpr) {
      switch (op_type) {
        /* Note that side effects are not propagated. */
        case Increment:
          return {type, apply_unary_arithmetic(right, [](auto r) { return ++r; })};
        case Decrement:
          return {type, apply_unary_arithmetic(right, [](auto r) { return --r; })};
        case Plus:
          return {type, apply_arithmetic(right, [](auto r) { return +r; })};
        case Minus:
          return {type, apply_arithmetic(right, [](auto r) { return -r; })};
        case Not:
          /* Note that '!' token is of MultiTok class and can contain many unary '!'. */
          return {type,
                  (t.str().size() & 1) ? apply_arithmetic(right, [](auto r) { return !r; }) :
                                         apply_arithmetic(right, [](auto r) { return !!r; })};
        case BitwiseNot:
          return {type, apply_integral(right, [](auto r) { return ~r; })};
        default:
          assert(0); /* Unreachable. */
          return table->err_cls;
      }
    }
    return {type, flags};
  }

  ExpressionResult suffix(ExpressionResult left, const ast::Node &t)
  {
    if (left.array_dim > 0) {
      error(node, Diag::ArrayTypeOperand);
      return table->err_cls;
    }

    TokenType op_type = t.front().type();

    auto it = table->operators.find({left.type, op_type, nullptr});
    if (it == table->operators.end()) {
      error(t, Diag::InvalidUnaryArgumentType, to_str(op_type), left.type->identifier);
      return table->err_cls;
    }

    SymbolClass *type = it->second->return_type;

    if (is_reference && (op_type == Increment || op_type == Decrement)) {
      error(t, Diag::ReferenceCannotBindSideEffect);
      return table->err_cls;
    }

    ExprFlag flags = ExprFlag(left.flags);
    if (flags & ExprFlag::IsConstexpr) {
      switch (op_type) {
        /* Note that side effects are not propagated. */
        case Increment:
          return {type, apply_unary_arithmetic(left, [](auto r) { return r++; })};
        case Decrement:
          return {type, apply_unary_arithmetic(left, [](auto r) { return r--; })};
        default:
          assert(0); /* Unreachable. */
          return table->err_cls;
      }
    }
    return {type, flags};
  }

  ExpressionResult binary(ExpressionResult lhs, const ast::Node &t, BindingPower p)
  {
    ExpressionResult rhs = expr(p);

    if (lhs.array_dim > 0 || rhs.array_dim > 0) {
      error(node, Diag::ArrayTypeOperand);
      return table->err_cls;
    }

    TokenType op_type = t.front().type();

    auto it = table->operators.find({lhs.type, op_type, rhs.type});
    if (it == table->operators.end()) {
      error(t,
            Diag::InvalidBinaryOperands,
            lhs.type->identifier,
            to_str(op_type),
            rhs.type->identifier);
      return table->err_cls;
    }

    SymbolClass *type = it->second->return_type;
    ExprFlag flags = ExprFlag(lhs.flags & rhs.flags);

    if (flags & ExprFlag::IsConstexpr) {
      switch (op_type) {
        case Multiply:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l * r; })};
        case Divide:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l / r; })};
        case Modulo:
          return {type, apply_integral(lhs, rhs, [](auto l, auto r) { return l % r; })};
        case Plus:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l + r; })};
        case Minus:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l - r; })};
        case LShift:
          return {type, apply_integral(lhs, rhs, [](auto l, auto r) { return l << r; })};
        case RShift:
          return {type, apply_integral(lhs, rhs, [](auto l, auto r) { return l >> r; })};
        case LThan:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l < r; })};
        case LEqual:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l <= r; })};
        case GThan:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l > r; })};
        case GEqual:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l >= r; })};
        case Equal:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l == r; })};
        case NotEqual:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l != r; })};
        case And:
          return {type, apply_integral(lhs, rhs, [](auto l, auto r) { return l & r; })};
        case Xor:
          return {type, apply_integral(lhs, rhs, [](auto l, auto r) { return l ^ r; })};
        case Or:
          return {type, apply_integral(lhs, rhs, [](auto l, auto r) { return l | r; })};
        case LogicalAnd:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l && r; })};
        case LogicalOr:
          return {type, apply_arithmetic(lhs, rhs, [](auto l, auto r) { return l || r; })};
        default:
          assert(0); /* Unreachable. */
          return table->err_cls;
      }
    }
    return {type, flags};
  }

  ExpressionResult ternary(ExpressionResult left,
                           const ast::Node & /*t*/,
                           BindingPower p_first,
                           BindingPower p_second)
  {
    ExpressionResult tval = expr(p_first);
    if (consume().front().type() != Colon) {
      error(ErrorType::ExpectedColon, Colon);
    }
    ExpressionResult fval = expr(p_second);

    if (left.type != table->bool_cls) {
      error(node, Diag::CondInvalidTernary, left.type->original);
      return table->err_cls;
    }
    if (tval.type != fval.type) {
      error(node, Diag::TypeInvalidTernary, tval.type->original, fval.type->original);
      return table->err_cls;
    }

    ExprFlag flags = ExprFlag(left.flags & tval.flags & fval.flags);
    if (flags & ExprFlag::IsConstexpr) {
      return {tval.type, std::get<bool>(left.value) ? tval.value : fval.value};
    }
    /* Operand types match. We can return either. */
    return {tval.type, flags};
  }

  ExpressionResult error(ErrorType err, lexit::TokenType type)
  {
    switch (err) {
      case ErrorType::ExpectedParenthesis:
        error(node, Diag::ExpectedParenthesis);
        break;
      case ErrorType::ExpectedColon:
        error(node, Diag::ExpectedColon);
        break;
      case ErrorType::InvalidOperator:
        error(node, Diag::TokenInvalidOperator, to_str(type));
        break;
      case ErrorType::InvalidExpression:
        error(node, Diag::InvalidExprTypeChecker);
        break;
    }
    return table->err_cls;
  }

  TokenType peek() const
  {
    return node.front().type();
  }

  ast::Node consume()
  {
    ast::Node n = node;
    node = node.next();
    return n;
  }

  TokenType to_type(const ast::Node &t) const
  {
    return t.front().type();
  }

 private:
  /*
   * Helper to evaluate operations valid for all types (Arithmetic).
   * C++ decltype(a op b) natively dictates the correct promoted return type
   * which perfectly forwards into std::variant.
   */
  template<typename Func>
  ConstexprValue apply_arithmetic(const ExpressionResult &lhs,
                                  const ExpressionResult &rhs,
                                  Func func) const
  {
    return std::visit(
        [func](auto &&a, auto &&b) -> ConstexprValue { return func(a, b); }, lhs.value, rhs.value);
  }
  template<typename Func>
  ConstexprValue apply_arithmetic(const ExpressionResult &rhs, Func func) const
  {
    return std::visit([func](auto &&b) -> ConstexprValue { return func(b); }, rhs.value);
  }

  /* Helper to evaluate bitwise and modulo operations (requires integral types). */
  template<typename Func>
  ConstexprValue apply_integral(const ExpressionResult &lhs,
                                const ExpressionResult &rhs,
                                Func func) const
  {
    return std::visit(
        [func](auto &&a, auto &&b) -> ConstexprValue {
          using T = std::decay_t<decltype(a)>;
          using U = std::decay_t<decltype(b)>;

          if constexpr (std::is_integral_v<T> && std::is_integral_v<U>) {
            return func(a, b);
          }
          else {
            /* Invalid operands: operation require integral types. */
            assert(0);
            return 0;
          }
        },
        lhs.value,
        rhs.value);
  }
  template<typename Func>
  ConstexprValue apply_integral(const ExpressionResult &rhs, Func func) const
  {
    return std::visit(
        [func](auto &&b) -> ConstexprValue {
          using T = std::decay_t<decltype(b)>;

          if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
            return func(b);
          }
          else {
            /* Invalid operands: operation require integral types. */
            assert(0);
            return 0;
          }
        },
        rhs.value);
  }

  template<typename Func>
  ConstexprValue apply_unary_arithmetic(const ExpressionResult &rhs, Func func) const
  {
    return std::visit(
        [func](auto &&b) -> ConstexprValue {
          using T = std::decay_t<decltype(b)>;

          if constexpr (!std::is_same_v<T, bool>) {
            return func(b);
          }
          else {
            /* Invalid operands: operation require float of integral types. */
            assert(0);
            return 0;
          }
        },
        rhs.value);
  }

  void error(ast::Node node,
             Diag diag,
             const std::string &msg = "",
             const std::string &msg2 = "",
             const std::string &msg3 = "")
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
};

Result<ExpressionResult> SymbolTable::expr_type_analysis(const SymbolScope &scope,
                                                         ast::Node start_node,
                                                         bool is_reference) const
{
  assert(start_node.is_valid());
  /* Caller must not pass the Expr node but it's first child. */
  assert(start_node.type() != ast::NodeType::Expr);
  return ExpressionTypeParser(this, &scope, start_node, is_reference).eval();
}

}  // namespace bsl
