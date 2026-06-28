/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "ast.hh"
#include "token.hh"

namespace blender::gpu::shader::parser {

using namespace ast;
using namespace std;

struct SourceLocation {
  Token tok;

  SourceLocation(Token tok) : tok(tok) {}

  operator string() const
  {
    return tok.filename() + ':' + to_string(tok.line_number()) + ':' +
           to_string(tok.char_number() + 1);
  }

  friend bool operator<=(const SourceLocation &a, const SourceLocation &b)
  {
    return (a.include_id() != b.include_id()) ? (a.include_id() <= b.include_id()) :
                                                (a.tok.index_ <= b.tok.index_);
  }

  friend bool operator<(const SourceLocation &a, const SourceLocation &b)
  {
    return (a.include_id() != b.include_id()) ? (a.include_id() < b.include_id()) :
                                                (a.tok.index_ < b.tok.index_);
  }

 private:
  int include_id() const
  {
    return static_cast<const ParserBase *>(tok.buf_)->include_id;
  }
};

struct Symbol {
  SymbolScope *parent = nullptr;
  SourceLocation loc;
  string identifier;
};

struct SymbolTemplate {
  /* Definition. */
  TemplateDecl decl;

  /* TODO(fclem): This is a bit stupid. */
  unordered_map<string, SymbolClass *> instances_cls;
  unordered_map<string, SymbolFunction *> instances_fn;

  SymbolTemplate(TemplateDecl temp) : decl(temp) {}

  SymbolClass *lookup_inst_cls(TemplateParamList list, const SymbolScope &scope) const;

  SymbolFunction *lookup_inst_fn(TemplateParamList list, const SymbolScope &scope) const;

  static string mangle_identifier(TemplateParamList list,
                                  const SymbolScope &scope,
                                  const string &sep = "T");
};

struct SymbolParser;

struct SymbolScope : Symbol {
  friend SymbolParser;

  /* Namespaces or nested blocks. */
  unordered_map<string, SymbolScope *> scopes;

  unordered_map<string, SymbolVariable *> variables;
  unordered_map<string, SymbolFunction *> functions;
  unordered_map<string, SymbolClass *> classes;

  /* Function scope act as anonymous scopes but still have identifier. */
  enum Type { FUNCTION, CLASS, NAMESPACE, LOCAL, ROOT } type = ROOT;

  SymbolScope(LocalScope decl) : Symbol(nullptr, decl.back(), "" /* Root */) {}

  SymbolScope(SymbolScope *parent, Token tok, const string &id, Type type)
      : Symbol(parent, tok, id), type(type)
  {
  }

  SymbolScope(SymbolScope *parent, LocalScope decl)
      : SymbolScope(parent, decl.front(), parent->unique_id(), LOCAL)
  {
  }

  SymbolScope(SymbolScope *parent, Id id, Type type)
      : SymbolScope(parent, id.front(), string(id.str()), type)
  {
  }

  SymbolVariable *lookup_variable(IdQualified id) const;
  SymbolFunction *lookup_function(IdQualified id) const;
  SymbolClass *lookup_class(IdQualified id) const;

  SymbolFunction *lookup_function(string id) const;
  SymbolClass *lookup_class(string id) const;

  void print() const;

  const SymbolScope *root_scope() const;

  /* Can be null if this is not a class. */
  SymbolClass *as_class();
  /* Can be null if this is not a function. */
  SymbolFunction *as_function();

  /* TODO */
  /* Built the symbol prefix for a symbol declared in this scope. */
  string make_prefix() const;

 private:
  SymbolVariable *lookup_variable_nested(Id id) const;
  SymbolFunction *lookup_function_nested(Id id) const;
  SymbolClass *lookup_class_nested(Id id) const;

  template<typename T> T *lookup_generic(IdQualified id, const SourceLocation &loc) const;
  template<typename T> T *lookup_generic_nested(Id id, const SourceLocation &loc) const;

  template<typename Callback> void visit_variables(Callback &&callback);

  int id = 0;
  /* Create a unique, non-reachable key.
   * Identifiers cannot start with digits.
   * This makes a scope impenetrable (can only leave the scope) during traversal. */
  string unique_id()
  {
    return to_string(id++);
  }
};

struct SymbolClass : SymbolScope {
  SymbolClass *resolved = nullptr;

  SymbolFunction *operator_subscript = nullptr;
  SymbolTemplate *template_data = nullptr;

  bool is_builtin = false;
  bool is_anonymous = false;
  bool is_union = false;
  bool is_std140_compatible = false;
  bool is_std430_compatible = false;
  bool is_error = false;

  int size = -1;  /* -1 is for not computed yet. */
  int align = -1; /* -1 is for not computed yet. */

  SymbolClass(SymbolScope *parent, ClassDecl decl, const string &suffix = "");

  SymbolClass(SymbolScope *parent,
              Token tok,
              const string &builtin_id,
              int size_in_bytes = 1,
              int align_in_bytes = 1)
      : SymbolScope(parent, tok, builtin_id, CLASS),
        is_builtin(true),
        size(size_in_bytes),
        align(align_in_bytes)
  {
  }

  void ensure_size_and_align();
};

struct SymbolFunction : SymbolScope {
  SymbolFunction *resolved = nullptr;

  SymbolTemplate *template_data = nullptr;
  SymbolClass *return_type = nullptr;

  bool is_error = false;

  enum Type { STATIC, MEMBER, GLOBAL } fn_type;

  SymbolFunction(SymbolScope *parent, FuncDecl decl, const string &suffix = "")
      : SymbolScope(
            parent, decl.front(), string(decl.identifier().name().str()) + suffix, FUNCTION),
        fn_type(decl.is_method() ? (decl.is_static() ? STATIC : MEMBER) : GLOBAL)
  {
  }

  SymbolFunction(SymbolScope *parent, Token tok, const string &id, Type fn_type)
      : SymbolScope(parent, tok, id, FUNCTION), fn_type(fn_type)
  {
  }

  /* Can be null if in global space. */
  SymbolClass *parent_class()
  {
    return this->parent->as_class();
  }
};

struct SymbolVariable : Symbol {
  SymbolVariable *resolved = nullptr;

  SymbolClass *type = nullptr; /* TODO rename to type */

  /* Value if constexpr. */
  int64_t value = 0;

  int array_dimensions = 0;
  /* In bytes. From scope start, taking alignment into account. */
  int offset = 0;
  bool is_static = false;
  bool is_error = false;
  bool is_constexpr = false;

  SymbolVariable(SymbolScope *parent, SymbolClass *type, Declarator decl)
      : Symbol(parent, decl.front(), string(decl.identifier().str())),
        type(type),
        array_dimensions(decl.array().dimensions()),
        is_static(decl.type().is_static()),
        is_constexpr(decl.type().constant().is_valid() ?
                         decl.type().constant().front() == Constexpr :
                         false)
  {
  }

  SymbolVariable(SymbolScope *parent, SymbolClass *type, EnumValue decl)
      : Symbol(parent, decl.front(), string(decl.identifier().str())), type(type), is_static(true)
  {
  }

  SymbolVariable(SymbolScope *parent, SymbolClass *type, Token id, const string &str)
      : Symbol(parent, id, str), type(type)
  {
  }

  void set_offset(bool is_union, int &offset);

  /* Can be null if in global space. */
  SymbolClass *parent_class()
  {
    return this->parent->as_class();
  }
};

inline SymbolClass *SymbolScope::as_class()
{
  return type == CLASS ? static_cast<SymbolClass *>(this) : nullptr;
}

inline SymbolFunction *SymbolScope::as_function()
{
  return type == FUNCTION ? static_cast<SymbolFunction *>(this) : nullptr;
}

struct SymbolTable {
  /* TODO(fclem): Real memory arena. */
  template<typename T> struct Allocator {
   private:
    vector<unique_ptr<T>> arena;

   public:
    template<typename... Args> T *alloc(Args &&...args)
    {
      arena.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
      return arena.back().get();
    }
  };

  Allocator<SymbolVariable> var_arena;
  Allocator<SymbolFunction> fun_arena;
  Allocator<SymbolTemplate> tmp_arena;
  Allocator<SymbolClass> cls_arena;
  Allocator<SymbolScope> scp_arena;

  template<typename SymbolT> SymbolT *alloc(SymbolT &&sym);

  SymbolScope *root = nullptr;
  SymbolScope *flatten_root = nullptr;

  SymbolTable(ParserBase &builtin_parser);

  void parse(LocalScope node, ErrorHandler &err_handler);

 private:
  void register_builtins(LocalScope node);
};

}  // namespace blender::gpu::shader::parser
