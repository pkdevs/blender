/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "ast.hh"
#include "token.hh"

#include <format>
#include <sstream>
#include <unordered_map>

namespace blender::gpu::shader::parser {

/* TODO using fmt directly.
 * Using std::format is not possible on macos because of our minimum target. */
namespace mini_fmt {

namespace detail {
// Convert any streamable argument into a std::string
template<typename T> std::string to_string_arg(T &&arg)
{
  std::ostringstream ss;
  ss << std::forward<T>(arg);
  return ss.str();
}

// Expand variadic arguments into a vector of strings
template<typename... Args> std::vector<std::string> args_to_strings(Args &&...args)
{
  return {to_string_arg(std::forward<Args>(args))...};
}
}  // namespace detail

template<typename... Args> std::string format(const std::string &fmt, Args &&...args)
{
  std::vector<std::string> str_args = detail::args_to_strings(std::forward<Args>(args)...);
  std::string result;
  result.reserve(fmt.size() + 16 * sizeof...(args));

  size_t auto_idx = 0;
  size_t i = 0;
  size_t len = fmt.size();

  while (i < len) {
    if (fmt[i] == '{' && i + 1 < len) {
      // Case 1: Sequential placeholder "{}"
      if (fmt[i + 1] == '}') {
        if (auto_idx < str_args.size()) {
          result += str_args[auto_idx++];
        }
        i += 2;
        continue;
      }

      // Case 2: Positional placeholder "{N}"
      size_t end_pos = fmt.find('}', i + 1);
      if (end_pos != std::string::npos) {
        std::string num_str = fmt.substr(i + 1, end_pos - (i + 1));

        // Parse integer index manually to avoid std::stoi dependency
        bool valid = !num_str.empty();
        size_t idx = 0;
        for (char c : num_str) {
          if (c < '0' || c > '9') {
            valid = false;
            break;
          }
          idx = idx * 10 + (c - '0');
        }

        if (valid && idx < str_args.size()) {
          result += str_args[idx];
          i = end_pos + 1;
          continue;
        }
      }
    }

    result += fmt[i++];
  }

  return result;
}

}  // namespace mini_fmt

using namespace ast;
using namespace std;

struct AstNodeException {
  Node node;
  string msg;

  AstNodeException(Node node, const string &msg) : node(node), msg(msg) {}
};

struct TokenException {
  Token tok;
  string msg;

  TokenException(Token tok, const string &msg) : tok(tok), msg(msg) {}
};

struct NodeErrorHandler {
  ErrorHandler &err_handler;

  enum class Diag {
    /* Entry Points & Shaders */
    ExpectedVertexShaderEntryPoint,
    ExpectedFragmentShaderEntryPoint,
    ExpectedComputeShaderEntryPoint,
    /* Resource Tables */
    ExpectedResourceTableInitializer,
    ResourceTableConstructorNotAllowed,
    /* Functions & Member Access */
    ReturnOutsideFunction,
    MissingParameterForCall,
    CompilerErrorMemberFuncNoClass,
    MemberAccessUnknownClass,
    EntryPointVoidReturn,
    /* Loops */
    UnrolledLoopMissingInit,
    UnrolledLoopMultipleVars,
    UnrolledLoopNonIntVar,
    UnrolledLoopMissingAssignment,
    UnrolledLoopCommaOperator,
    UnrolledLoopTooManyIterations,
    UnrolledLoopMustAssignToVar,
    UnrolledLoopInvalidExpression,
    /* Variables & Types */
    UnknownVariable,
    UnknownTypeName,
    TypeNotConvertibleToBool,
    MissingExplicitTemplateArguments,
    TemplateInvalidArg,
    /* Arrays */
    UnionArrayUnsupported,
    UnionArrayUnsupportedAlt,
    ArraySubscriptMismatch,
    ArrayNeedsSizeOrInitializer,
    ArraySizeMustBeGreaterThanZero,
    MultidimensionalArrayImplicitSize,
    MultidimensionalArraysNotSupported,
    ArraySizeNotConstantExpression,
    /* Initializers & Constexpr */
    ConstexprDivisionByZero,
    InitializerListRequiresExplicitType,
    ExpectedArrayMemberInitializer,
    FieldDesignatorOutOfOrder,
    FieldDesignatorNotFound,
    /* Attributes & Branching */
    MultipleConditionAttributes,
    ExpectedNParametersForAttribute,
    ExpectedRangeParametersForAttribute,
    StaticBranchMissingPrevious,
    StaticBranchRequiredByPrevious,
    StaticBranchConstexprIfNotAllowed,
    ExpectedImageFormat,
    /* Notes */
    NoteDeclarationUnionRequested,
    NoteInstantiationClassTemplateRequested,
    NoteInstantiationFuncTemplateRequested
  };

  const std::unordered_map<Diag, std::string> diagnostic_messages = {
      /* Entry Points & Shaders */
      {Diag::ExpectedVertexShaderEntryPoint, /* 0 arg */
       "Expected vertex shader entry point function"},
      {Diag::ExpectedFragmentShaderEntryPoint, /* 0 arg */
       "Expected fragment shader entry point function"},
      {Diag::ExpectedComputeShaderEntryPoint, /* 0 arg */
       "Expected compute shader entry point function"},
      /* Resource Tables */
      {Diag::ExpectedResourceTableInitializer, /* 0 arg */
       "Expected resource table initializer"},
      {Diag::ResourceTableConstructorNotAllowed, /* 1 arg */
       "Calling resource table constructor '{0}' is not allowed; use 'resource_table_get({0})' "
       "instead"},
      /* Functions & Member Access */
      {Diag::ReturnOutsideFunction, /* 0 arg */
       "Return statement outside of a function"},
      {Diag::MissingParameterForCall, /* 1 arg */
       "Missing parameter for call to '{}'"},
      {Diag::CompilerErrorMemberFuncNoClass, /* 0 arg */
       "Compiler error: member function has no class"},
      {Diag::MemberAccessUnknownClass, /* 0 arg */
       "Cannot access member of unknown class"},
      {Diag::EntryPointVoidReturn, /* 0 arg */
       "Entry point functions must return 'void'"},
      /* Loops */
      {Diag::UnrolledLoopMissingInit,
       "Init statement needs to define the loop variable for unrolled loops"},
      {Diag::UnrolledLoopMultipleVars, /* 0 arg */
       "Multiple variable declared in unrolled loop"},
      {Diag::UnrolledLoopNonIntVar, /* 0 arg */
       "Loop variable needs to be an integer type for unrolled loops"},
      {Diag::UnrolledLoopMissingAssignment, /* 0 arg */
       "Loop variable needs to be assigned a value (using assignment) for unrolled loops"},
      {Diag::UnrolledLoopCommaOperator, /* 0 arg */
       "Comma operator is not allowed in unrolled loop statement"},
      {Diag::UnrolledLoopTooManyIterations, /* 0 arg */
       "Loop unrolling generates too many iterations (over 64)"},
      {Diag::UnrolledLoopMustAssignToVar, /* 1 arg */
       "Unrolled loop expression must assign to '{}'"},
      {Diag::UnrolledLoopInvalidExpression, /* 1 arg */
       "Expected '++{0}', '--{0}', '{0}++', '{0}--' or '{0} = expr', '{0} += expr', '{0} "
       "-= expr', '{0} /= expr', '{0} *= expr' for unrolled loop expression"},
      /* Variables & Types */
      {Diag::UnknownVariable, /* 1 arg */
       "Unknown variable '{}'"},
      {Diag::UnknownTypeName, /* 0 arg */
       "Unknown type name"},
      {Diag::TypeNotConvertibleToBool, /* 1 arg */
       "Value of type '{}' is not contextually convertible to 'bool'"},
      {Diag::MissingExplicitTemplateArguments, /* 0 arg */
       "Missing explicit template arguments"},
      {Diag::TemplateInvalidArg, /* 1 arg */
       "Invalid argument for template parameter '{}'"},
      /* Arrays */
      {Diag::UnionArrayUnsupported, /* 0 arg */
       "Unsupported array at union base scope; wrap it inside a named struct"},
      {Diag::UnionArrayUnsupportedAlt, /* 0 arg */
       "Unsupported array at union base scope; wrap the array inside a named struct"},
      {Diag::ArraySubscriptMismatch, /* 2 arg */
       "Not enough subscript ({}) to match array dimensions ({})"},
      {Diag::ArrayNeedsSizeOrInitializer, /* 0 arg */
       "Definition of variable with array type needs an explicit size or an initializer"},
      {Diag::ArraySizeMustBeGreaterThanZero, /* 0 arg */
       "Array size must be greater than zero"},
      {Diag::MultidimensionalArrayImplicitSize, /* 0 arg */
       "Multi-dimensional arrays cannot have implicit size"},
      {Diag::MultidimensionalArraysNotSupported, /* 0 arg */
       "Multi-dimensional arrays are not supported"},
      {Diag::ArraySizeNotConstantExpression, /* 0 arg */
       "Array size is not a constant expression"},
      /* Initializers & Constexpr */
      {Diag::ConstexprDivisionByZero, /* 0 arg */
       "Division by zero during constexpr evaluation"},
      {Diag::InitializerListRequiresExplicitType, /* 0 arg */
       "Initializer list requires explicit type"},
      {Diag::ExpectedArrayMemberInitializer, /* 0 arg */
       "Expected array member initializer but got designated initializer"},
      {Diag::FieldDesignatorOutOfOrder, /* 1 arg */
       "Field designator '{}' needs to be specified in declaration order"},
      {Diag::FieldDesignatorNotFound, /* 2 arg */
       "Field designator '{}' does not refer to any field in type '{}'"},
      /* Attributes & Branching */
      {Diag::MultipleConditionAttributes, /* 0 arg */
       "Only one condition attribute is allowed."},
      {Diag::ExpectedNParametersForAttribute, /* 2 arg */
       "Expected {} parameter(s) for attribute '{}'"},
      {Diag::ExpectedRangeParametersForAttribute, /* 2 arg */
       "Expected between 1 and {} parameter(s) for attribute '{}'"},
      {Diag::StaticBranchMissingPrevious, /* 0 arg */
       "statement cannot be tagged as 'static_branch' because previous statement also isn't"},
      {Diag::StaticBranchRequiredByPrevious, /* 0 arg */
       "statement must be tagged as 'static_branch' because previous statement is"},
      {Diag::StaticBranchConstexprIfNotAllowed, /* 0 arg */
       "'if' statement cannot be 'constexpr' inside a static branch"},
      {Diag::ExpectedImageFormat, /* 0 arg */
       "Expected image format"},
      /* Notes */
      {Diag::NoteDeclarationUnionRequested, /* 1 arg */
       "In declaration of union '{}' requested here"},
      {Diag::NoteInstantiationClassTemplateRequested, /* 1 arg */
       "In instantiation of class template specialization '{}' requested here"},
      {Diag::NoteInstantiationFuncTemplateRequested, /* 1 arg */
       "In instantiation of function template specialization '{}' requested here"},
  };

  NodeErrorHandler(ErrorHandler &handler) : err_handler(handler) {}

  void error(Token tok, const string &msg)
  {
    if (err_handler.err) {
      return;
    }
    vector<pair<Token, string>> errors;
    errors.emplace_back(tok, msg);

    for (auto it = note_stack.rbegin(); it != note_stack.rend(); ++it) {
      auto [note_node, note_msg] = *it;
      errors.emplace_back(note_node, note_msg);
    }
    err_handler.report(errors);
  }

  // void error(ast::Node node, const string &msg)
  // {
  //   error(node.front(), msg);
  // }

  /* Pipe error if existing. */
  void error(std::optional<AstNodeException> &err)
  {
    if (err) {
      error(err->node, err->msg);
    }
  }
  void error(std::optional<TokenException> &err)
  {
    if (err) {
      error(err->tok, err->msg);
    }
  }

  template<typename... Args> void error(ast::Node node, Diag code, Args &&...args)
  {
    std::string_view format_str = diagnostic_messages.at(code);
    std::string message = mini_fmt::format(std::string(format_str), std::forward<Args>(args)...);
    error(node, message);
  }

  /* Stack of notes to add to the error. */
  vector<pair<Token, string>> note_stack = {};

  struct ScopedNote {
    vector<pair<Token, string>> &note_stack;

    ScopedNote(Token tok, const string &msg, vector<pair<Token, string>> &note_stack)
        : note_stack(note_stack)
    {
      note_stack.emplace_back(tok, msg);
    }

    ~ScopedNote()
    {
      note_stack.pop_back();
    }
  };

  [[nodiscard]] ScopedNote note(Token tok, const string &msg)
  {
    return ScopedNote(tok, msg, note_stack);
  }
  [[nodiscard]] ScopedNote note(Node node, const string &msg)
  {
    return ScopedNote(node.front(), msg, note_stack);
  }

#define NOTE(node, msg) auto n = note(node, msg)
};

template<typename T, typename ExceptionT = AstNodeException> struct Result {
  T value;
  std::optional<ExceptionT> err;

  T unwrap(NodeErrorHandler *handler)
  {
    handler->error(err);
    return value;
  }
};

template<typename T, typename U, typename ExceptionT = AstNodeException> struct ResultPair {
  T first;
  U second;
  std::optional<ExceptionT> err;

  pair<T, U> unwrap(NodeErrorHandler *handler)
  {
    handler->error(err);
    return {first, second};
  }
};

}  // namespace blender::gpu::shader::parser
