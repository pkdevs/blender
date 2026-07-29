/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "ast.hh"
#include "metadata.hh"
#include "token.hh"
#include <unordered_map>

namespace blender::gpu::shader::parser {

using namespace ast;
using namespace std;

struct SymbolParser;
struct SymbolTable;

enum class ResourceType {
  NONE = 0,

  LEGACY_INFO,
  COMPILATION_CONST,
  SPECIALIZATION_CONST,
  PUSH_CONST,
  SAMPLER,
  IMAGE,
  UNIFORM_BUF,
  STORAGE_BUF,
  ACCELERATION_STRUCTURE,
  RESOURCE_TABLE,
  SHARED,

  VERT_ATTR_IN,
  VERT_ATTR_OUT,
  FRAG_IN,
  FRAG_OUT,

  BASE_INSTANCE,
  CLIP_CONTROL,
  CLIP_DISTANCES,
  FRONT_FACING,
  GLOBAL_INVOCATION_ID,
  IN,
  INSTANCE_ID,
  INSTANCE_INDEX,
  LAYER,
  LOCAL_INVOCATION_ID,
  LOCAL_INVOCATION_INDEX,
  OUT,
  POINT_COORD,
  POINT_SIZE,
  POSITION,
  SUBPASS_IN,
  VERTEX_ID,
  VIEWPORT_INDEX,
  WORK_GROUP_ID,
  NUM_WORK_GROUP,
  FRAG_DEPTH,
  FRAG_COORD,
  FRAG_STENCIL_REF,

  CONDITION,
  FREQUENCY,
  DUAL_SOURCE_INDEX,
  RASTER_ORDER_GROUP,
};

static inline string to_str(ResourceType type)
{
#define SERIALIZE(a) \
  case ResourceType::a: \
    return #a

  switch (type) {
    SERIALIZE(NONE);
    SERIALIZE(LEGACY_INFO);
    SERIALIZE(COMPILATION_CONST);
    SERIALIZE(SPECIALIZATION_CONST);
    SERIALIZE(PUSH_CONST);
    SERIALIZE(SAMPLER);
    SERIALIZE(IMAGE);
    SERIALIZE(UNIFORM_BUF);
    SERIALIZE(STORAGE_BUF);
    SERIALIZE(ACCELERATION_STRUCTURE);
    SERIALIZE(VERT_ATTR_IN);
    SERIALIZE(VERT_ATTR_OUT);
    SERIALIZE(FRAG_IN);
    SERIALIZE(FRAG_OUT);
    SERIALIZE(BASE_INSTANCE);
    SERIALIZE(CLIP_CONTROL);
    SERIALIZE(CLIP_DISTANCES);
    SERIALIZE(FRONT_FACING);
    SERIALIZE(GLOBAL_INVOCATION_ID);
    SERIALIZE(IN);
    SERIALIZE(INSTANCE_ID);
    SERIALIZE(INSTANCE_INDEX);
    SERIALIZE(LAYER);
    SERIALIZE(LOCAL_INVOCATION_ID);
    SERIALIZE(LOCAL_INVOCATION_INDEX);
    SERIALIZE(OUT);
    SERIALIZE(POINT_COORD);
    SERIALIZE(POINT_SIZE);
    SERIALIZE(POSITION);
    SERIALIZE(SUBPASS_IN);
    SERIALIZE(VERTEX_ID);
    SERIALIZE(VIEWPORT_INDEX);
    SERIALIZE(WORK_GROUP_ID);
    SERIALIZE(FRAG_DEPTH);
    SERIALIZE(FRAG_COORD);
    SERIALIZE(RESOURCE_TABLE);
    SERIALIZE(SHARED);
    SERIALIZE(NUM_WORK_GROUP);
    SERIALIZE(FRAG_STENCIL_REF);
    SERIALIZE(CONDITION);
    SERIALIZE(FREQUENCY);
    SERIALIZE(DUAL_SOURCE_INDEX);
    SERIALIZE(RASTER_ORDER_GROUP);
  }
  return "Unknown";
#undef SERIALIZE
}

enum class ResourceTableType {
  NONE = 0,

  VERTEX_IN,
  VERTEX_OUT,
  FRAGMENT_OUT,
  FRAGMENT_IN,
  RESOURCE_TABLE,

  ENTRY_POINT,
};

static inline string to_str(ResourceTableType type)
{
#define SERIALIZE(a) \
  case ResourceTableType::a: \
    return #a

  switch (type) {
    SERIALIZE(NONE);
    SERIALIZE(VERTEX_IN);
    SERIALIZE(VERTEX_OUT);
    SERIALIZE(FRAGMENT_IN);
    SERIALIZE(FRAGMENT_OUT);
    SERIALIZE(RESOURCE_TABLE);
    SERIALIZE(ENTRY_POINT);
  }
  return "Unknown";
#undef SERIALIZE
}

struct StringPair {
  string str;
  string str_debug;
};

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

  void error(ast::Node node, const string &msg)
  {
    error(node.front(), msg);
  }

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

struct ParsedAttribute {
  ResourceType res_type = ResourceType::NONE;
  ResourceTableType srt_type = ResourceTableType::NONE;
  Expr param1, param2, param3;
  Attr attr;
  Expr condition;
  Expr frequency;
  Expr dual_source_index;
  Expr raster_order_group;

  string parse_condition() const;
};

Result<ParsedAttribute> resource_type_from_attributes(AttrList list);

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
  /* Unresolved identifier */
  string original;
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

  ResultPair<T *, string> lookup_inst(TemplateParamList list, const SymbolScope &scope) const;
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

  SymbolScope(LocalScope decl) : Symbol("" /* Root */, "", decl.back(), nullptr) {}

  SymbolScope(SymbolScope *parent, Token tok, const string &id, Type type)
      : Symbol(id, id, tok, parent), type(type)
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
  template<typename ArgOrParamList>
  Result<SymbolFunction *> lookup_function(IdQualified id,
                                           ArgOrParamList params,
                                           const SymbolTable &table) const;
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
  SymbolScope *root_scope();

  /* Can be null if this is not a class. */
  SymbolClass *as_class();
  /* Can be null if this is not a function. */
  SymbolFunction *as_function();

  /* Return the function containing this scope or the scope itself if it is a function.
   * Return nullptr if this scope is not inside a function. */
  SymbolFunction *parent_function();

  /* Returns true if the function is an overload or redefines the symbol. */
  bool function_emplace(SymbolFunction *fn);

  vector<SymbolVariable *> non_static_variables_in_declaration_order() const;

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
  ResourceTableType srt_type = ResourceTableType::NONE;

  SymbolFunction *operator_subscript = nullptr;
  SymbolClassTemplate *template_data = nullptr;

  bool is_builtin = false;
  bool is_anonymous = false;
  bool is_union = false;
  bool is_enum = false;
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

  void ensure_size_and_align(NodeErrorHandler *err_handler = nullptr, bool check_st430 = false);

  bool is_srt() const
  {
    return srt_type != ResourceTableType::NONE;
  }
};

struct SymbolFunction : SymbolScope {
  /* Single linked list of overloads. */
  SymbolFunction *overload_next = nullptr;

  SymbolFunctionTemplate *template_data = nullptr;
  SymbolClass *return_type = nullptr;

  vector<SymbolClass *> arg_types;
  vector<Expr> arg_defaults;

  bool is_error = false;

  enum Type { STATIC, MEMBER, GLOBAL } fn_type;
  enum class EntryPointType { FRAG, VERT, COMP, NONE } entry_point_type;

  SymbolFunction(SymbolScope *parent, FuncDecl decl, const string &suffix = "")
      : SymbolScope(
            parent, decl.front(), string(decl.identifier().name().str()) + suffix, FUNCTION),
        fn_type(decl.is_method() ? (decl.is_static() ? STATIC : MEMBER) : GLOBAL),
        entry_point_type(EntryPointType::NONE)
  {
    if (decl.is_method()) {
      loc = decl.parent(NodeType::ClassDecl).front();
    }
    for (Attr attr : decl.attributes().children_of_type<Attr>()) {
      string_view id = attr.identifier().str();
      if (id == "vertex") {
        entry_point_type = EntryPointType::VERT;
      }
      else if (id == "fragment") {
        entry_point_type = EntryPointType::FRAG;
      }
      else if (id == "compute") {
        entry_point_type = EntryPointType::COMP;
      }
    }
  }

  SymbolFunction(SymbolScope *parent, Token tok, const string &id, Type fn_type)
      : SymbolScope(parent, tok, id, FUNCTION),
        fn_type(fn_type),
        entry_point_type(EntryPointType::NONE)
  {
  }

  /* Can be null if in global space. */
  SymbolClass *parent_class()
  {
    return this->parent->as_class();
  }

  bool is_entry_point() const
  {
    return entry_point_type != SymbolFunction::EntryPointType::NONE;
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
  SymbolClass *type = nullptr; /* TODO rename to type */

  ResourceType res_type = ResourceType::NONE;

  /* Value if constexpr. */
  int64_t value = 0;

  int array_dimensions = 0;
  /* Number of array elements in the declaration.
   * 0 if not array.
   * -1 if not known at compile time. */
  int array_elements = 0;
  /* In bytes. From scope start, taking alignment into account. */
  int offset = 0;
  bool is_static = false;
  bool is_error = false;
  bool is_constexpr = false;
  bool is_const = false;
  bool is_compilation_const = false;
  /* Valid if the variable is a local reference.
   * The variable will be substituted to this statement. */
  Expr reference_value = {};

  SymbolVariable(SymbolScope *parent, SymbolClass *type, Declarator decl);

  SymbolVariable(SymbolScope *parent, SymbolClass *type, EnumValue decl)
      : Symbol(string(decl.identifier().str()),
               string(decl.identifier().str()),
               decl.front(),
               parent),
        type(type),
        is_static(true)
  {
  }

  SymbolVariable(SymbolScope *parent, SymbolClass *type, Token id, const string &str)
      : Symbol(str, str, id, parent), type(type)
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

inline SymbolFunction *SymbolScope::parent_function()
{
  SymbolScope *fn = this;
  while (fn && fn->type != FUNCTION) {
    fn = fn->parent;
  }
  return fn ? fn->as_function() : nullptr;
}

inline bool SymbolScope::function_emplace(SymbolFunction *fn)
{
  bool result = false;
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
    result = true;
  }
  scopes.emplace(unique_id(), fn);
  return result;
}

inline vector<SymbolVariable *> SymbolScope::non_static_variables_in_declaration_order() const
{
  assert(this->type == CLASS);
  vector<SymbolVariable *> members;
  for (const auto &[k, v] : variables) {
    members.emplace_back(v);
  }

  /* Sort elements to guarantee deterministic order. */
  sort(members.begin(), members.end(), [](const SymbolVariable *a, const SymbolVariable *b) {
    return a->loc < b->loc;
  });

  return members;
}

/* Structure to hold the flattened member data */
struct MemberNode {
  SymbolClass *type;
  string path;
  int absolute_offset;
};

namespace detail {

/* Recursive helper to accumulate path and offsets */
static inline optional<TokenException> get_flat_members_recursive(const SymbolClass *cls,
                                                                  const string &current_path,
                                                                  int current_offset,
                                                                  vector<MemberNode> &out_buffer)
{
  /* Iterate through variables in their declaration order */
  for (SymbolVariable *var : cls->non_static_variables_in_declaration_order()) {
    /* Construct the full access path. */
    /* If the identifier is empty (e.g., an anonymous union/struct), we skip adding the dot. */
    std::string next_path = current_path;
    if (!var->identifier.empty()) {
      if (!next_path.empty()) {
        next_path += ".";
      }
      next_path += var->original;
    }
    /* Accumulate the total offset from the root struct. */
    int offset = current_offset + var->offset;

    if (var->array_dimensions > 1) {
      /* TODO */
      return TokenException(var->loc.tok, "Multi-dimensional arrays are not supported");
    }
    int elements = (var->array_dimensions == 0) ? 1 : var->array_elements;
    if (elements == -1) {
      return TokenException(var->loc.tok, "Array size is not a constant expression");
    }

    for (int i = 0; i < elements; ++i) {
      string array_accessor = var->array_dimensions > 0 ? "[" + to_string(i) + "]" : "";
      if (var->type->is_builtin) {
        /* Base case: It's a primitive/builtin type. Add to the flat buffer. */
        out_buffer.emplace_back(var->type, next_path + array_accessor, offset);
        offset += var->type->size;
      }
      else {
        /* Recursive step: It's a nested user-defined structure. */
        auto err = get_flat_members_recursive(
            var->type, next_path + array_accessor, offset, out_buffer);
        offset += var->type->size;
        if (err) {
          return err;
        }
      }
    }
  }
  return {};
}

}  // namespace detail

static inline Result<vector<MemberNode>, TokenException> get_all_members_flat(
    const SymbolClass *root_class)
{
  vector<MemberNode> flat_buffer;
  auto err = detail::get_flat_members_recursive(root_class, "", 0, flat_buffer);
  return {flat_buffer, err};
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

  static Result<string> expr_to_string(const SymbolScope &scope,
                                       Node start,
                                       int &node_count,
                                       bool allow_compilation_constant = false);

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
  static vector<BuiltinFunc> generate_all_builtin_functions();
};

}  // namespace blender::gpu::shader::parser
