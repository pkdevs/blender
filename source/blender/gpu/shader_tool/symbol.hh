/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "ast.hh"
#include "token.hh"
#include <unordered_map>

namespace blender::gpu::shader::parser {

using namespace ast;
using namespace std;

struct SymbolParser;
struct SymbolTable;

struct StringPair {
  string str;
  string str_debug;
};

struct AstNodeException {
  Node node;
  string msg;

  AstNodeException(Node node, const string &msg) : node(node), msg(msg) {}
};

template<typename T> struct Result {
  T value;
  /* TODO better error class */
  std::optional<AstNodeException> err;
};

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
  string identifier;
  SourceLocation loc;
  SymbolScope *parent = nullptr;
};

template<typename T> struct SymbolTemplate {
  /* Definition. */
  TemplateDecl decl;

  /* TODO(fclem): This is a bit stupid. */
  unordered_map<string, T *> instances;

  /* For argument dependent lookup. Maps a template argument to a function argument. */
  vector<int> temp_arg_index_in_fn_arg;

  SymbolTemplate(TemplateDecl temp) : decl(temp)
  {
    init_adl();
  }

  bool is_adl_possible() const
  {
    return !temp_arg_index_in_fn_arg.empty();
  }

  Result<T *> lookup_inst(TemplateParamList list, const SymbolScope &scope) const;
  Result<T *> lookup_adl(const SymbolTable &symbols,
                         FuncParamList list,
                         const SymbolScope &scope) const;

  void init_adl();

 private:
  IdQualified id() const
  {
    if (decl.is_class()) {
      return ClassDecl(decl.decl()).identifier();
    }
    return FuncDecl(decl.decl()).identifier();
  }
};

using SymbolClassTemplate = SymbolTemplate<SymbolClass>;
using SymbolFunctionTemplate = SymbolTemplate<SymbolFunction>;

struct SymbolScope : Symbol {
  friend SymbolParser;

  /* Namespaces or nested blocks. */
  unordered_map<string, SymbolScope *> scopes;

  unordered_map<string, SymbolVariable *> variables;
  unordered_map<string, SymbolFunction *> functions;
  unordered_map<string, SymbolClass *> classes;

  /* Function scope act as anonymous scopes but still have identifier. */
  enum Type { FUNCTION, CLASS, NAMESPACE, LOCAL, ROOT } type = ROOT;

  SymbolScope(LocalScope decl) : Symbol("" /* Root */, decl.back(), nullptr) {}

  SymbolScope(SymbolScope *parent, Token tok, const string &id, Type type)
      : Symbol(id, tok, parent), type(type)
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
  Result<SymbolFunction *> lookup_function(IdQualified id) const;
  Result<SymbolClass *> lookup_class(IdQualified id) const;

  Result<SymbolClass *> lookup_class(Id id, Id last) const;

  SymbolVariable *lookup_variable(string id) const;
  SymbolFunction *lookup_function(string id) const;
  SymbolClass *lookup_class(string id) const;

  /* Doesn't match last template in identifier. */
  SymbolFunction *lookup_function_base(IdQualified id) const;
  SymbolClass *lookup_class_base(IdQualified id) const;

  void print() const;

  const SymbolScope *root_scope() const;

  /* Can be null if this is not a class. */
  SymbolClass *as_class();
  /* Can be null if this is not a function. */
  SymbolFunction *as_function();

  void function_emplace(SymbolFunction *fn);

  template<typename Callback> void visit_variables(Callback &&callback);

 private:
  template<typename T> T *lookup_generic(IdQualified id, const SourceLocation &loc) const;
  template<typename T> T *lookup_generic_nested(Id id, Id last, const SourceLocation &loc) const;

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
  SymbolClassTemplate *template_data = nullptr;

  bool is_builtin = false;
  bool is_anonymous = false;
  bool is_union = false;
  bool is_std140_compatible = false;
  bool is_std430_compatible = false;
  bool is_error = false;

  /* For enums, the value of the last declaration. */
  int enum_last_val = -1;

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
  /* Single linked list of overloads. */
  SymbolFunction *overload_next = nullptr;

  SymbolFunctionTemplate *template_data = nullptr;
  SymbolClass *return_type = nullptr;

  vector<SymbolClass *> arg_types;

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

  /* To be called on the first registered overload. */
  SymbolFunction *lookup_overload(const vector<SymbolClass *> &arg_types);

  /* Return true if the given argument types are compatible */
  bool argument_matches(const vector<SymbolClass *> &arg_types) const;

  static Result<vector<SymbolClass *>> to_arg_types(const SymbolTable &table,
                                                    const SymbolScope &scope,
                                                    FuncParamList list);
  static Result<vector<SymbolClass *>> to_arg_types(const SymbolTable &table,
                                                    const SymbolScope &scope,
                                                    FuncArgList list);
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
      : Symbol(string(decl.identifier().str()), decl.front(), parent),
        type(type),
        array_dimensions(decl.array().dimensions()),
        is_static(decl.type().is_static()),
        is_constexpr(decl.type().constant().is_valid() ?
                         decl.type().constant().front() == Constexpr :
                         false)
  {
  }

  SymbolVariable(SymbolScope *parent, SymbolClass *type, EnumValue decl)
      : Symbol(string(decl.identifier().str()), decl.front(), parent), type(type), is_static(true)
  {
  }

  SymbolVariable(SymbolScope *parent, SymbolClass *type, Token id, const string &str)
      : Symbol(str, id, parent), type(type)
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

inline void SymbolScope::function_emplace(SymbolFunction *fn)
{
  if (auto it = functions.try_emplace(fn->identifier, fn); !it.second) {
    /* If function already exists, insert overload in the linked list. */
    fn->overload_next = it.first->second->overload_next;
    it.first->second->overload_next = fn;
    /* Add suffix to the function identifier to reduce chance of hitting an overload at runtime.
     * This way, the dead code eliminator can discard more functions.
     * This suffix needs to be the same whatever the file include order is, as it can differ from
     * shader to shader. We use the line index for that. It is short enough to not clutter the
     * resulting source file. */
    fn->identifier += to_string(fn->loc.tok.line_number());
  }
  scopes.emplace(unique_id(), fn);
}

template<typename Callback> inline void SymbolScope::visit_variables(Callback &&callback)
{
  assert(this->type == CLASS);
  vector<SymbolVariable *> members;
  for (auto &[k, v] : variables) {
    members.emplace_back(v);
  }

  /* Sort elements to guarantee deterministic order. */
  sort(members.begin(), members.end(), [](const SymbolVariable *a, const SymbolVariable *b) {
    return a->loc < b->loc;
  });

  for (auto *m : members) {
    callback(*m);
  }
}

/* Left is nullptr for unary operators. */
using OperatorKey = tuple<SymbolClass * /* Left */, TokenType, SymbolClass * /* Right */>;

struct OperatorKeyHasher {
  template<class T> static void hash_combine(std::size_t &seed, const T &v)
  {
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  }

  std::size_t operator()(const OperatorKey &t) const
  {
    std::size_t seed = 0;
    hash_combine(seed, std::get<0>(t));
    hash_combine(seed, std::get<1>(t));
    hash_combine(seed, std::get<2>(t));
    return seed;
  }
};

using OperatorMap = unordered_map<OperatorKey, SymbolFunction *, OperatorKeyHasher>;

struct SymbolTable {
  static constexpr string err_symbol = "ERROR_SYMBOL";

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
  Allocator<SymbolClass> cls_arena;
  Allocator<SymbolScope> scp_arena;
  Allocator<SymbolClassTemplate> tmp_cls_arena;
  Allocator<SymbolFunctionTemplate> tmp_fun_arena;

  OperatorMap operators;

  template<typename SymbolT> SymbolT *alloc(SymbolT &&sym);

  SymbolScope *root = nullptr;
  SymbolScope *flatten_root = nullptr;

  SymbolTable(ParserBase &builtin_parser);

  void parse(LocalScope node, ErrorHandler &err_handler);

  static Result<StringPair> mangle_identifier(TemplateArgList args,
                                              TemplateParamList list,
                                              const SymbolScope &scope);
  Result<StringPair> mangle_identifier(const SymbolFunctionTemplate &tmp,
                                       FuncParamList list,
                                       const SymbolScope &scope) const;

  Result<SymbolClass *> expr_type_analysis(const SymbolScope &scope, Expr expr) const;

  Result<SymbolClass *> resolve_auto_type(SymbolScope &scope, Declarator decl) const;

  static Result<int64_t> evaluate_constexpr(const SymbolScope &scope, Node start);

  static Result<string> expr_to_string(const SymbolScope &scope, Node start, int &node_count);

 private:
  struct BuiltinType {
    /* "int", "int2", ... */
    string name;
    /* "int", "uint", "float", "bool", ... */
    string base;
    /* 1 (scalar), 2, 3, 4 */
    int size;
  };

  struct BuiltinOp {
    string left;
    TokenType op;
    string right;
    string result;
  };

  struct BuiltinFunc {
    string return_type;
    string id;
    vector<string> arg_types;
  };

  void register_builtins(LocalScope node);
  static vector<BuiltinType> generate_builtin_types();
  static vector<BuiltinOp> generate_all_operators(const vector<BuiltinType> &types);
  static vector<BuiltinFunc> generate_all_constructors(const vector<BuiltinType> &types);
};

}  // namespace blender::gpu::shader::parser
