/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "lexit/lexit.hh"

namespace blender::gpu::shader::parser {

using namespace lexit;

struct ParserBase;
struct Token;
struct SymbolVariable;
struct SymbolFunction;
struct SymbolClass;
struct SymbolScope;

std::string to_str(TokenType type);

namespace ast {

using Token = parser::Token;

enum class NodeType : char {
  Invalid = 0,
  TranslationUnit,
  Preprocessor,
  Namespace,
  NamespaceSeparator,
  ClassDecl,
  AccessSpecifier,
  EnumValue,
  Id,
  IdQualified,
  IdType,
  Const,
  Reference,
  AssignStmt,
  PipelineDecl,
  VarDecl,
  Declarator,
  StaticStmt,
  FuncDecl,
  FuncForwardDecl,
  FuncArgList,
  FuncArg,
  FuncParamList,
  FuncCall,
  TemplateDecl,
  TemplateArgList,
  TemplateArg,
  TemplateParamList,
  TemplateSpec,
  TemplateInst,
  TemplateExplicit,
  LocalScope,
  Subscript,
  AttrList,
  Attr,
  Expr,
  ContinueStmt,
  BreakStmt,
  ReturnStmt,
  UsingStmt,
  LocalStmt,
  LocalVar,
  SwitchStmt,
  SwitchCase,
  ForLoop,
  WhileLoop,
  DoWhileLoop,
  Condition,
  IfStmt,
  ElseIfStmt,
  ElseStmt,
  InitializerList,
  DesignatedInitializer,
  Initializer,
  StringConst,
  ExprSub,
  NumConst,
  Op,
  OpDeref,
  Constructor,
};

using TokenID = int;
using NodeID = int;

struct NodeData;
using Nodes = std::vector<NodeData>;

struct NodeData {
  NodeType type = NodeType::Invalid;
  TokenID front = -1;
  TokenID back = -1;
  NodeID parent = -1;
  NodeID prev = -1;
  NodeID next = -1;
  NodeID child_first = -1;
  NodeID child_last = -1;

  NodeData() = default;

  NodeData(Nodes &nodes, NodeID id, NodeID parent_id, lexit::Token tok, NodeType type)
      : type(type), front(tok.index_), back(tok.index_)
  {
    if (parent_id != -1) {
      NodeData &parent = nodes[parent_id];
      if (parent.child_last != -1) {
        nodes[parent.child_last].next = id;
        this->prev = parent.child_last;
      }
      if (parent.child_first == -1) {
        parent.child_first = id;
      }
      parent.child_last = id;
    }
    this->parent = parent_id;
  }

  std::string location(const ParserBase &parser) const;
};

struct Node {
  const ParserBase *p = nullptr;
  NodeID id = -1;
#ifdef LEXIT_DEBUG
  std::string_view debug_str;
#endif

  Node() = default;
  Node(const ParserBase *p, NodeID id) : p(p), id(id)
  {
#ifdef LEXIT_DEBUG
    debug_str = str();
#endif
  }

  NodeType type() const;

  Token front() const;
  Token back() const;

  Node prev() const;
  Node next() const;
  Node parent() const;
  Node prev(NodeType type) const;
  Node next(NodeType type) const;
  Node parent(NodeType type) const;
  Node children() const;
  Node child_first() const;
  Node child_last() const;
  Node child_first(NodeType type) const
  {
    Node node = child_first();
    return (node.type() == type) ? node : node.next(type);
  }
  Node child_last(NodeType type) const
  {
    Node node = child_last();
    return (node.type() == type) ? node : node.prev(type);
  }

  int child_count() const;
  bool is_empty() const;
  bool is_valid() const
  {
    return id != -1;
  }

  void print_ast() const;
  std::string_view str() const;

  bool operator==(NodeType type) const
  {
    return this->type() == type;
  }
  bool operator!=(NodeType type) const
  {
    return this->type() != type;
  }

  std::ostream &operator<<(std::ostream &os)
  {
    os << str();
    return os;
  }

  /* Traversal. */

  template<typename NodeT, typename CallbackT> void foreach_recursive(CallbackT cb) const
  {
    for (Node end = find_recursion_end(),
              node = first_node_of_type(children(), NodeT::NodeEnumVal, end);
         node.id != end.id;
         node = next_node_of_type(node, NodeT::NodeEnumVal, end))
    {
      cb(NodeT(node));
    }
  }

  template<typename NodeT, typename CallbackT> void foreach(CallbackT cb) const
  {
    for (Node node = child_first(NodeT::NodeEnumVal); node.is_valid();
         node = node.next(NodeT::NodeEnumVal))
    {
      cb(NodeT(node));
    }
  }

  template<typename CallbackT> void foreach_child(CallbackT cb) const
  {
    for (Node node = child_first(); node.is_valid(); node = node.next()) {
      cb(node);
    }
  }

 private:
  static Node first_node_of_type(Node node, NodeType type, Node end)
  {
    while (node.id != end.id && node.type() != type) {
      node = Node(node.p, node.id + 1);
    }
    return node;
  }

  static Node next_node_of_type(Node node, NodeType type, Node end)
  {
    do {
      node = Node(node.p, node.id + 1);
    } while (node.id != end.id && node.type() != type);
    return node;
  }

  Node find_recursion_end() const
  {
    {
      /* Try using parent first. */
      Node node = *this;
      while (node.is_valid()) {
        node = node.parent();
        if (node.next().is_valid()) {
          return node.next();
        }
      }
    }
    {
      /* Using children in last resort. */
      Node node = *this;
      while (node.is_valid()) {
        node = node.child_last();
        if (!node.child_last().is_valid()) {
          /* No children. Deepest latest node. */
          return Node(p, node.id + 1);
        }
      }
    }
    /* Nothing to iterate on. */
    return *this;
  }
};

#define NODE_COMMON(Type) \
  static constexpr NodeType NodeEnumVal = NodeType::Type; \
  Type() = default; \
  Type(const Node &node) : Node(node.type() == NodeType::Type ? node : Node{}) {}

using TokenRange = Token; /* TODO */

struct FuncParamList;
struct LocalScope;
struct ClassDecl;
struct InitializerList;
struct TemplateParamList;

struct Preprocessor : Node {
  NODE_COMMON(Preprocessor);
};

struct NumConst : Node {
  NODE_COMMON(NumConst);
};

struct Id : Node {
  NODE_COMMON(Id);

  /* Return the next Id in an IdQualified.  */
  Id next_id() const
  {
    return next().type() == NodeType::NamespaceSeparator ? next().next() : Node{};
  }

  bool is_namespace() const
  {
    return next().type() == NodeType::NamespaceSeparator;
  }
};

struct IdQualified : Node {
  NODE_COMMON(IdQualified);

  bool has_namespace() const
  {
    return child_first(NodeType::NamespaceSeparator).is_valid();
  }

  bool is_global() const
  {
    return child_first().type() == NodeType::NamespaceSeparator;
  }

  Id name() const
  {
    return child_last(NodeType::Id);
  }

  Id namespace_start() const
  {
    return child_first(NodeType::Id);
  }

  TemplateParamList template_params() const;
};

struct TemplateParamList : Node {
  NODE_COMMON(TemplateParamList);
};

inline TemplateParamList IdQualified::template_params() const
{
  return child_last(NodeType::TemplateParamList);
}

struct OpDeref : Node {
  NODE_COMMON(OpDeref);
};

struct StaticStmt : Node {
  NODE_COMMON(StaticStmt);
};

struct AccessSpecifier : Node {
  NODE_COMMON(AccessSpecifier);
};

struct TemplateExplicit : Node {
  NODE_COMMON(TemplateExplicit);
};

struct TemplateSpec : Node {
  NODE_COMMON(TemplateSpec);

  TemplateParamList parameters() const;

  bool is_function() const
  {
    return child_last().type() == NodeType::FuncDecl;
  }

  bool is_class() const
  {
    return child_last().type() == NodeType::ClassDecl;
  }

  Node decl() const
  {
    return child_last();
  }
};

struct TemplateInst : Node {
  NODE_COMMON(TemplateInst);

  TemplateParamList parameters() const;

  bool is_function() const
  {
    return child_last().type() == NodeType::FuncForwardDecl;
  }

  bool is_class() const
  {
    return child_last().type() == NodeType::ClassDecl;
  }

  Node decl() const
  {
    return child_last();
  }
};

struct TemplateArg : Node {
  NODE_COMMON(TemplateArg);

  IdQualified type() const
  {
    return child_first();
  }

  IdQualified id() const
  {
    return child_last();
  }
};

struct TemplateArgList : Node {
  NODE_COMMON(TemplateArgList);
};

struct TemplateDecl : Node {
  NODE_COMMON(TemplateDecl);

  TemplateArgList arguments() const
  {
    return child_first();
  }

  bool is_function() const
  {
    return child_last().type() == NodeType::FuncDecl;
  }

  bool is_class() const
  {
    return child_last().type() == NodeType::ClassDecl;
  }

  Node decl() const
  {
    return child_last();
  }
};

struct Const : Node {
  NODE_COMMON(Const);
};

struct Reference : Node {
  NODE_COMMON(Reference);
};

struct Expr : Node {
  NODE_COMMON(Expr);
};

struct ExprSub : Node {
  NODE_COMMON(ExprSub);

  Expr expr()
  {
    return child_first();
  }
};

struct UsingStmt : Node {
  NODE_COMMON(UsingStmt);

  bool is_namespace() const;

  IdQualified id() const
  {
    return child_first();
  }

  IdQualified aliased() const
  {
    return child_last();
  }
};

struct LocalVar : Node {
  NODE_COMMON(LocalVar);

  IdQualified identifier() const
  {
    return child_first();
  }
};

struct ReturnStmt : Node {
  NODE_COMMON(ReturnStmt);

  Expr expression() const
  {
    return child_last();
  }
};

struct IdType : Node {
  NODE_COMMON(IdType);

  bool is_const() const
  {
    return constant().is_valid();
  }

  bool is_constexpr() const;

  bool is_static() const
  {
    return child_first(NodeType::StaticStmt).is_valid();
  }

  Const constant() const
  {
    return child_first(NodeType::Const);
  }

  IdQualified id() const
  {
    return child_first(NodeType::IdQualified);
  }

  Reference reference() const
  {
    return child_first(NodeType::Reference);
  }
};

struct Subscript : Node {
  NODE_COMMON(Subscript);

  Expr expr() const
  {
    return child_first();
  }

  Subscript sub() const
  {
    return child_last();
  }

  /* Return 0 if invalid. */
  int dimensions() const
  {
    int array_dimensions = 0;
    Subscript sub = *this;
    while (sub.is_valid()) {
      array_dimensions++;
      sub = sub.sub();
    }
    return array_dimensions;
  }
};

struct DesignatedInitializer : Node {
  NODE_COMMON(DesignatedInitializer);
};

struct Initializer : Node {
  NODE_COMMON(Initializer);
};

struct InitializerList : Node {
  NODE_COMMON(InitializerList);
};

struct Attr : Node {
  NODE_COMMON(Attr);

  Id identifier() const
  {
    return child_first();
  }

  FuncParamList parameters() const;
};

struct AttrList : Node {
  NODE_COMMON(AttrList);

  bool contains_attr(std::string_view attr_name) const
  {
    bool found = false;
    foreach<Attr>([&](Attr attr) {
      if (attr.identifier().str() == attr_name) {
        found = true;
      }
    });
    return found;
  }
};

struct LocalStmt : Node {
  NODE_COMMON(LocalStmt);

  AttrList attributes() const
  {
    return child_first();
  }

  Expr expr() const
  {
    return child_last();
  }
};

struct AssignStmt : Node {
  NODE_COMMON(AssignStmt);

  InitializerList initializer_list() const
  {
    return child_first();
  }

  Expr expr() const
  {
    return child_first();
  }
};

struct Declarator : Node {
  NODE_COMMON(Declarator);

  bool is_reference() const
  {
    return reference().is_valid();
  }

  bool is_array() const
  {
    return array().is_valid();
  }

  IdType type() const
  {
    return prev(NodeType::IdType);
  }

  Reference reference() const
  {
    return child_first();
  }

  IdQualified identifier() const
  {
    return child_first(NodeType::IdQualified);
  }

  Subscript array() const
  {
    return identifier().next();
  }

  InitializerList initializer_list() const
  {
    return child_last();
  }

  AssignStmt initial_value() const
  {
    return child_last();
  }
};

struct VarDecl : Node {
  NODE_COMMON(VarDecl);

  bool is_const() const
  {
    return type().is_const();
  }

  AttrList attributes() const
  {
    return child_first();
  }

  IdType type() const
  {
    return child_first(NodeType::IdType);
  }
};

struct FuncArg : Node {
  NODE_COMMON(FuncArg);

  AttrList attributes() const
  {
    return child_first();
  }

  IdType type() const
  {
    return child_first(NodeType::IdType);
  }

  Declarator declarator() const
  {
    return child_first(NodeType::Declarator);
  }

  bool is_reference() const
  {
    return declarator().is_reference();
  }

  bool is_const() const
  {
    return type().is_const();
  }

  IdQualified identifier() const
  {
    return declarator().identifier();
  }

  Subscript array() const
  {
    return declarator().array();
  }

  InitializerList initializer_list() const
  {
    return declarator().initializer_list();
  }

  AssignStmt initial_value() const
  {
    return declarator().initial_value();
  }
};

struct FuncArgList : Node {
  NODE_COMMON(FuncArgList);
};

struct FuncParamList : Node {
  NODE_COMMON(FuncParamList);

  bool is_empty() const;
};

inline FuncParamList Attr::parameters() const
{
  return FuncParamList(children().next());
}

struct FuncForwardDecl : Node {
  NODE_COMMON(FuncForwardDecl);

  bool is_method() const
  {
    return parent(NodeType::ClassDecl).is_valid();
  }

  bool is_static() const
  {
    return child_first(NodeType::StaticStmt).is_valid();
  }

  bool is_const() const
  {
    return child_last(NodeType::Const).is_valid();
  }

  AttrList attributes() const
  {
    return child_first();
  }

  IdType return_type() const
  {
    return child_first(NodeType::IdType);
  }

  IdQualified identifier() const
  {
    return return_type().next();
  }

  FuncArgList arguments() const
  {
    return child_first(NodeType::FuncArgList);
  }

  /* Associated class if this function is a class method. */
  ClassDecl parent_class() const;
};

struct FuncDecl : Node {
  NODE_COMMON(FuncDecl);

  bool is_method() const
  {
    return parent(NodeType::ClassDecl).is_valid();
  }

  bool is_static() const
  {
    return child_first(NodeType::StaticStmt).is_valid();
  }

  bool is_const() const
  {
    return child_last(NodeType::Const).is_valid();
  }

  AttrList attributes() const
  {
    return child_first();
  }

  IdType return_type() const
  {
    return child_first(NodeType::IdType);
  }

  IdQualified identifier() const
  {
    return return_type().next();
  }

  FuncArgList arguments() const
  {
    return child_first(NodeType::FuncArgList);
  }

  /* Associated class if this function is a class method. */
  ClassDecl parent_class() const;

  LocalScope body() const;
};

struct FuncCall : Node {
  NODE_COMMON(FuncCall);

  IdQualified identifier() const
  {
    return child_first();
  }

  FuncParamList parameters() const
  {
    return child_last();
  }
};

struct EnumValue : Node {
  NODE_COMMON(EnumValue);

  IdQualified identifier() const
  {
    return child_first();
  }

  AssignStmt value() const
  {
    return child_last();
  }
};

struct LocalScope : Node {
  NODE_COMMON(LocalScope);
};

struct Condition : Node {
  NODE_COMMON(Condition);
};

struct IfStmt : Node {
  NODE_COMMON(IfStmt);

  Condition condition() const
  {
    return child_first();
  }

  AttrList attributes() const
  {
    return child_last(NodeType::AttrList);
  }

  LocalScope body() const
  {
    return child_last();
  }
};

struct ElseIfStmt : Node {
  NODE_COMMON(ElseIfStmt);

  Condition condition() const
  {
    return child_first();
  }

  AttrList attributes() const
  {
    return child_last(NodeType::AttrList);
  }

  LocalScope body() const
  {
    return child_last();
  }
};

struct ElseStmt : Node {
  NODE_COMMON(ElseStmt);

  LocalScope body() const
  {
    return child_last();
  }
};

struct ForLoop : Node {
  NODE_COMMON(ForLoop);

  Condition condition() const
  {
    return child_first();
  }

  LocalScope body() const
  {
    return child_last();
  }
};

struct WhileLoop : Node {
  NODE_COMMON(WhileLoop);

  Condition condition() const
  {
    return child_first();
  }

  LocalScope body() const
  {
    return child_last();
  }
};

struct DoWhileLoop : Node {
  NODE_COMMON(DoWhileLoop);

  Condition condition() const
  {
    return child_last();
  }

  LocalScope body() const
  {
    return child_first();
  }
};

struct SwitchStmt : Node {
  NODE_COMMON(SwitchStmt);

  Condition condition() const
  {
    return child_first();
  }
};

struct SwitchCase : Node {
  NODE_COMMON(SwitchCase);

  bool is_default_case() const
  {
    return child_first().type() == NodeType::LocalScope;
  }

  /* Either NumConst or IdQualified. */
  Node value() const
  {
    return child_first();
  }

  LocalScope body() const
  {
    return child_last();
  }
};

/* Enum, Struct, Class. */
struct ClassDecl : Node {
  NODE_COMMON(ClassDecl);

  bool is_enum() const;
  bool is_enum_class() const;
  bool is_union() const;

  bool is_anonymous() const
  {
    return !identifier().is_valid();
  }

  IdQualified identifier() const;

  AttrList attributes() const
  {
    return child_first();
  }

  IdQualified parent_class() const;

  LocalScope body() const
  {
    return child_last();
  }
};

inline ClassDecl FuncDecl::parent_class() const
{
  return parent().parent();
}

inline LocalScope FuncDecl::body() const
{
  return child_last(NodeType::LocalScope);
}

inline TemplateParamList TemplateSpec::parameters() const
{
  Node node = child_last();
  if (node.type() == NodeType::ClassDecl) {
    ClassDecl decl(node);
    return decl.identifier().template_params();
  }
  if (node.type() == NodeType::FuncDecl) {
    FuncDecl decl(node);
    return decl.identifier().template_params();
  }
  assert(0);
  return {};
}

inline TemplateParamList TemplateInst::parameters() const
{
  Node node = child_last();
  if (node.type() == NodeType::ClassDecl) {
    ClassDecl decl(node);
    return decl.identifier().template_params();
  }
  if (node.type() == NodeType::FuncForwardDecl) {
    FuncForwardDecl decl(node);
    return decl.identifier().template_params();
  }
  assert(0);
  return {};
}

struct Namespace : Node {
  NODE_COMMON(Namespace);

  LocalScope body() const
  {
    return child_last();
  }

  IdQualified identifier() const
  {
    return child_first();
  }
};

#undef NODE_COMMON

}  // namespace ast
}  // namespace blender::gpu::shader::parser
