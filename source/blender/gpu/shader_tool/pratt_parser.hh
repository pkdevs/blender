/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "lexit/types.hh"

/**
 * CRTP Base Class for Pratt Parsers.
 *
 * The Derived class must implement the following interface:
 * - ResultT identifier(const NodeT &t)
 * - ResultT number(const NodeT &t)
 * - ResultT string(const NodeT &t)
 * - ResultT prefix(const NodeT &t, BindingPower p)
 * - ResultT parenthesis(BindingPower p)                             // Must parse closing token
 * - ResultT member(ResultT left, const NodeT &t, BindingPower p)
 * - ResultT subscript(ResultT left, const NodeT &t, BindingPower p) // Must parse closing token
 * - ResultT postfix(ResultT left, const NodeT &t, BindingPower p)
 * - ResultT binary(ResultT left, const NodeT &t, BindingPower p)
 * - ResultT ternary(ResultT left, const NodeT &t, BindingPower p)
 * - ResultT error(ErrorType err, lexit::TokenType type)
 * - NodeT peek() const
 * - NodeT consume()
 * - bool is_valid_token(const NodeT &t) const
 */
template<typename Derived, typename ResultT, typename NodeT> class PrattParser {
 public:
  enum class ErrorType {
    ExpectedParenthesis,
    ExpectedColon,
    InvalidOperator,
    InvalidExpression,
  };

  struct BindingPower {
    /* Unary operators must have the highest precedence. */
    static constexpr int unary = 1000;
    /* Let parenthesis evaluate everything until a closing parenthesis. */
    static constexpr int parenthesis = 0;

    int binding_power;

    BindingPower(int power) : binding_power(power) {}
  };

  ResultT expr(BindingPower right)
  {
    Derived &self = static_cast<Derived &>(*this);

    /* Parse unary operator, evaluate parenthesis, evaluate constant. */
    ResultT left = nud(self.consume());

    /* While left binding power is greater than the right, continue consuming binary operations. */
    while (self.is_valid_token(self.peek()) &&
           left_binding_power(self.peek().type()) > right.binding_power)
    {
      left = led(left, self.consume());
    }

    return left;
  }

  /* How a token evaluates without left context (e.g. unary operator).
   * Also known as Null-Denotation or NUD. */
  ResultT nud(const NodeT &t)
  {
    using namespace lexit;

    Derived &self = static_cast<Derived &>(*this);
    TokenType type = t.type();

    switch (type) {
      case Word:
        return self.identifier(t);
      case Number:
        return self.number(t);
      case String:
        return self.string(t);
      case Plus:
      case Minus:
      case Not:
      case BitwiseNot:
        /* Note that '!' token is of MultiTok class and can contain many unary '!'.
         * Implementation must take care of it. */
        return self.prefix(t, BindingPower::unary);
      case ParOpen:
        /* Parse the whole parenthesis expression. */
        return self.parenthesis(BindingPower::parenthesis);
      default:
        return self.error(ErrorType::InvalidExpression, type);
    }
  }

  /* How a token evaluates from left-to-right, on two operands.
   * Also known as Left-Denotation or LED. */
  ResultT led(ResultT left, const NodeT &t)
  {
    using namespace lexit;

    Derived &self = static_cast<Derived &>(*this);
    TokenType type = t.type();

    switch (type) {
      case Dot:
        return self.member(left, t);
      case SquareOpen:
        return self.subscript(left, t);
      case Increment:
      case Decrement:
        return self.postfix(left, t, left_binding_power(type));
      case Multiply:
      case Divide:
      case Modulo:
      case Plus:
      case Minus:
      case LShift:
      case RShift:
      case LThan:
      case LEqual:
      case GThan:
      case GEqual:
      case Equal:
      case NotEqual:
      case And:
      case Xor:
      case Or:
      case LogicalAnd:
      case LogicalOr:
        return self.binary(left, t, left_binding_power(type));
      case Question:
        /* The middle expression can be almost anything so use 0 so it only stops at the ':' (since
         * Colon has a precedence of 0).
         * Second expression uses (Question - 1) to handle right-associativity. */
        return self.ternary(left, t, 0, left_binding_power(Question) - 1);
      default:
        return self.error(ErrorType::InvalidOperator, type);
    }
  }

  /* C/C++ binding power. */
  int left_binding_power(lexit::TokenType type)
  {
    using namespace lexit;

    Derived &self = static_cast<Derived &>(*this);

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
    self.error(ErrorType::InvalidOperator, type);
    return -1;
  }
};
