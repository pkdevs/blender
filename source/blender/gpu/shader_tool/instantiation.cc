/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include <sstream>

#include "processor.hh"
#include "symbol.hh"

namespace blender::gpu::shader {
using namespace std;
using namespace shader::parser;
using namespace shader::parser::ast;
using namespace metadata;

struct InstantiationContext {
  SourceProcessor::Parser &parser;
  SymbolTable &symbols;
  SourceProcessor::ErrorHandler &error_handler;

  void error(ast::Node node, const string &str)
  {
    error_handler.report(node.front(), str);
  }

  /* Pipe error if existing. */
  void error(std::optional<AstNodeException> &err)
  {
    if (err) {
      error(err->node, err->msg);
    }
  }

  struct StringBuilder {
    Token curr;
    stringstream ss;

    StringBuilder &operator<<(Token tok)
    {
      ss << tok.str_with_whitespace();
      curr = curr.next();
      return *this;
    }

    StringBuilder &operator<<(const string &str)
    {
      ss << str;
      curr = curr.next();
      return *this;
    }

    StringBuilder &operator<<(const string_view &str)
    {
      ss << str;
      curr = curr.next();
      return *this;
    }

    StringBuilder &operator<<(const Node &node)
    {
      ss << node.front().buf_->substr(node.front(), node.back(), true);
      curr = node.back().next();
      return *this;
    }

    StringBuilder &operator<<(const StringBuilder &sb)
    {
      ss << sb.ss.str();
      curr = curr.next();
      return *this;
    }
  } builder;

  InstantiationContext(SourceProcessor::Parser &parser,
                       SymbolTable &symbols,
                       SourceProcessor::ErrorHandler &error_handler)
      : parser(parser),
        symbols(symbols),
        error_handler(error_handler),
        builder(Token::invalid(&parser))
  {
  }

  string process(SymbolScope &symbol, LocalScope node)
  {
    local_scope(node, symbol);
    return builder.ss.str();
  }

 private:
  static string trivia(Token t)
  {
    string_view no_ws = t.str();
    return string(t.str_with_whitespace().substr(no_ws.size()));
  }

  static string trivia(Node node)
  {
    return trivia(node.back());
  }

  static string as_whitespace(string_view str)
  {
    size_t lines = count(str.begin(), str.end(), '\n');
    size_t spaces = str.find_last_of("\n");
    if (spaces != string::npos) {
      spaces = str.length() - (spaces + 1);
    }
    else {
      spaces = str.length();
    }
    return string(lines, '\n') + string(spaces, ' ');
  }

  void skip_node(Node node)
  {
    if (node.is_valid()) {
      string_view str = parser.substr(node.front(), node.back(), true);
      builder.ss << as_whitespace(str);
      builder.curr = node.back().next();
    }
  }

  /* Only go to next token if matching an optional token. */
  template<typename... Args> bool skip_if(Args... expected)
  {
    if (((builder.curr == TokenType(expected)) || ...)) {
      string_view str = builder.curr.str_with_whitespace();
      builder << as_whitespace(str);
      return true;
    }
    return false;
  }

  void line(Token tok)
  {
    builder.ss << "\n#line " + to_string(tok.line_number()) + "\n";
  }

  void line(Node node)
  {
    line(node.front());
  }

  void jump_to(Token tok)
  {
    line(tok);
    builder.ss << string(tok.char_number(), ' ');
    builder.curr = tok;
  }

  /* Only go to next token if matching an optional token. */
  template<typename... Args> bool match_if(Args... expected)
  {
    if (((builder.curr == TokenType(expected)) || ...)) {
      builder << builder.curr;
      return true;
    }
    return false;
  }

  void local_scope(LocalScope node, SymbolScope &symbol)
  {
    assert(node.is_valid());
    if (symbol.parent != nullptr) {
      if (symbol.type == SymbolScope::NAMESPACE) {
        skip_if('{');
      }
      else {
        match_if('{');
      }
    }

    if (node.child_first().is_valid()) {
      jump_to(node.child_first().front());
    }

    int local_scope_id = 0;
    for (Node node : node.children_range()) {
      switch (node.type()) {
        case NodeType::Preprocessor:
          builder << node;
          break;
        case NodeType::Namespace:
          namespace_decl(Namespace(node), symbol);
          break;
        case NodeType::VarDecl:
          var_decl(VarDecl(node), symbol);
          break;
        case NodeType::ClassDecl:
          class_decl(ClassDecl(node), symbol);
          break;
        case NodeType::FuncDecl:
          func_decl(FuncDecl(node), symbol);
          break;
        case NodeType::TemplateDecl:
          skip_node(node);
          break;
        case NodeType::TemplateInst:
          template_inst(TemplateInst(node), symbol);
          break;
        case NodeType::TemplateSpec:
          template_spec(TemplateSpec(node), symbol);
          break;
        case NodeType::UsingStmt:
          skip_node(node);
          break;
        case NodeType::PipelineDecl:
          skip_node(node);
          break;
        case NodeType::ForLoop:
          for_loop(node, *symbol.scopes[to_string(local_scope_id++)]);
          break;
        case NodeType::WhileLoop:
          while_loop(node, *symbol.scopes[to_string(local_scope_id++)]);
          break;
        case NodeType::DoWhileLoop:
          do_while_loop(node, *symbol.scopes[to_string(local_scope_id++)]);
          break;
        case NodeType::SwitchStmt:
          switch_statement(node, symbol, local_scope_id);
          break;
        case NodeType::ReturnStmt:
          return_statement(node, symbol);
          break;
        case NodeType::StructuredBinding:
          structured_binding(node, symbol);
          break;
        case NodeType::IfStmt:
        case NodeType::ElseIfStmt:
        case NodeType::ElseStmt:
          if_statement(node, *symbol.scopes[to_string(local_scope_id++)]);
          break;
        case NodeType::LocalStmt:
          local_statement(node, symbol);
          break;
        case NodeType::LocalScope:
          local_scope(LocalScope(node), *symbol.scopes[to_string(local_scope_id++)]);
          break;
        default:
          builder << node;
          break;
      }
    }

    if (symbol.parent != nullptr) {
      if (symbol.type == SymbolScope::NAMESPACE) {
        skip_if('}');
      }
      else {
        match_if('}');
      }
    }
  }

  void return_statement(ReturnStmt stmt, SymbolScope &scope)
  {
    match_if(Return);
    expr(stmt.expression(), scope);
    match_if(';');
  }

  void structured_binding(StructuredBinding decl, SymbolScope &scope)
  {
    SymbolVariable *tmp_var = scope.lookup_variable(decl.tmp_id());

    AssignStmt assign = decl.assign();

    Token decl_front = decl.front();
    Token assign_front = assign.front();

    int pad = assign_front.char_number() - decl_front.char_number();

    jump_to(decl_front);
    /* Replace 'auto [a, b]' by the tmp variable declaration. */
    string decl_str = tmp_var->type->resolved->identifier + " " + tmp_var->identifier;
    /* Add padding until the assign statement. */
    builder.ss << decl_str + string(max(0, pad - int(decl_str.size())), ' ');

    builder.curr = assign_front;
    assignment(assign, scope);
    match_if(';');
  }

  void switch_statement(SwitchStmt stmt, SymbolScope &scope, int &local_scope_id)
  {
    match_if(Switch);
    condition(stmt.condition(), scope);
    match_if('{');

    for (Node node : stmt.children_range()) {
      switch (node.type()) {
        case NodeType::Preprocessor:
          builder << node;
          break;
        case NodeType::SwitchCase: {
          SwitchCase switch_case(node);
          match_if(Case, Default);
          if (!switch_case.is_default_case()) {
            Node val = switch_case.value();
            if (val.type() == NodeType::IdQualified) {
              id_var_resolved(val, scope);
            }
            else {
              builder << val;
            }
          }
          match_if(Colon);
          local_scope(switch_case.body(), *scope.scopes[to_string(local_scope_id++)]);
          break;
        }
        case NodeType::Condition:
          break; /* Already processed. */
        default:
          assert(0);
      }
    }
    match_if('}');
  }

  void loop_unroll(ForLoop stmt, SymbolScope &scope)
  {
    VarDecl init_stmt = stmt.condition().child_first();
    LocalStmt cond_stmt = init_stmt.next();
    LocalStmt end_stmt = cond_stmt.next();
    Expr cond_expr = cond_stmt.expr();
    Expr end_expr = end_stmt.expr();
    if (!init_stmt.is_valid()) {
      error(stmt, "Init statement needs to define the loop variable for unrolled loops");
      return;
    }
    Declarator decl = init_stmt.child_first(NodeType::Declarator);
    SymbolVariable *var = scope.lookup_variable(decl.identifier());
    if (var->is_error) {
      error(decl.identifier(), "Unknown variable");
      return;
    }
    /* Check if only a single var is declared. */
    if (decl.next(NodeType::Declarator).is_valid()) {
      error(init_stmt.type(), "Multiple variable declared in unrolled loop");
      return;
    }
    /* Check if loop variable is of integer type. */
    SymbolClass *cls = var->type;
    if (cls->identifier != "int" && cls->identifier != "uint") {
      error(init_stmt.type(), "Loop variable needs to be an integer type for unrolled loops");
      return;
    }
    /* Check if loop variable is init by constexpr. */
    Expr init_expr = decl.initial_value().expr();
    if (!init_expr.is_valid()) {
      error(decl,
            "Loop variable needs to be assigned a value (using assignment) for unrolled loops");
      return;
    }
    auto [val, err] = SymbolTable::evaluate_constexpr(scope, init_expr.child_first());
    if (err) {
      error(err->node, err->msg);
      return;
    }
    /* Check if loop statement assign to and only to the loop variable. */
    for (Node node : end_expr.children_range()) {
      if (node.type() == NodeType::Op && node.front() == ',') {
        error(end_stmt, "Comma operator is not allowed in unrolled loop statement");
      }
    }
    if (error_handler.err) {
      return;
    }

    /* Replace loop variable resolved value with constexpr value. */
    var->is_constexpr = true;
    var->value = val;
    /* Run a small virtual machine. For each iteration, check the condition, and run the loop
     * statement. Break only if we  */
    for (int i = 0;; i++) {
      auto [cond_val, err] = SymbolTable::evaluate_constexpr(scope, cond_expr.child_first());
      if (err) {
        error(err->node, err->msg);
        break;
      }
      /* Break if condition is false. */
      if (cond_val == 0) {
        break;
      }
      /* Break if too many iterations. */
      if (i >= 64) {
        error(stmt, "Loop unrolling generates too many iterations (over 64)");
        break;
      }
      /* Generate the loop body. */
      jump_to(stmt.body().front());
      local_scope(stmt.body(), scope);

      auto [loop_val, err_val] = eval_constexpr_with_side_effects(var, end_expr, scope);
      if (err_val) {
        error(err_val->node, err_val->msg);
        break;
      }
      /* Evaluate the loop statement. */
      var->value = loop_val;
    }

    /* Restore. */
    var->is_constexpr = false;

    jump_to(stmt.back().next());
  }

  Result<int64_t> eval_constexpr_with_side_effects(SymbolVariable *var,
                                                   Expr expr,
                                                   SymbolScope &scope)
  {
    if (expr.child_count() == 2) {
      /* ExpressionParser will not evaluate increment and decrement operators.
       * We have to handle it ourselves. */
      if (expr.child_last().type() == NodeType::Op) {
        LocalVar local_var = expr.child_first();
        if (local_var.is_valid()) {
          if (scope.lookup_variable(local_var.identifier()) != var) {
            return {0,
                    AstNodeException(expr,
                                     "Unrolled loop expression must assign to '" +
                                         var->identifier + "'")};
          }
          if (expr.back() == Decrement) {
            return {var->value - 1, {}};
          }
          if (expr.back() == Increment) {
            return {var->value + 1, {}};
          }
        }
      }
      else if (expr.child_first().type() == NodeType::Op) {
        LocalVar local_var = expr.child_last();
        if (local_var.is_valid()) {
          if (scope.lookup_variable(local_var.identifier()) != var) {
            return {0,
                    AstNodeException(expr,
                                     "Unrolled loop expression must assign to '" +
                                         var->identifier + "'")};
          }
          if (expr.front() == Decrement) {
            return {var->value - 1, {}};
          }
          if (expr.front() == Increment) {
            return {var->value + 1, {}};
          }
        }
      }
    }
    else {
      /* ExpressionParser will not evaluate assignment operator (=,+=,-=, ..)s.
       * We have to handle it ourselves. */
      LocalVar local_var = expr.child_first();
      if (local_var.is_valid()) {
        if (scope.lookup_variable(local_var.identifier()) != var) {
          return {0,
                  AstNodeException(
                      expr, "Unrolled loop expression must assign to '" + var->identifier + "'")};
        }
        Node op = local_var.next();
        if (op.type() == NodeType::Op) {
          Node node = op.next();

          auto [loop_val, err_val] = SymbolTable::evaluate_constexpr(scope, node);

          switch (op.front().type()) {
            case Assign:
              return {loop_val, err_val};
            case AssignAdd:
              return {var->value + loop_val, err_val};
            case AssignSub:
              return {var->value - loop_val, err_val};
            case AssignMul:
              return {var->value * loop_val, err_val};
            case AssignDiv:
              return {var->value / loop_val, err_val};
            default:
              break;
          }
        }
      }
    }
    string id = var->identifier;
    return {0,
            AstNodeException(expr,
                             "Expected '++" + id + "', '--" + id + "', '" + id + "++', '" + id +
                                 "--' or '" + id + " = expr', '" + id + " += expr', '" + id +
                                 " -= expr', '" + id + " /= expr', '" + id +
                                 " *= expr' for unrolled loop expression")};
  }

  void for_loop(ForLoop stmt, SymbolScope &scope)
  {
    if (stmt.condition().attributes().contains_attr("unroll")) {
      loop_unroll(stmt, scope);
      return;
    }
    match_if(For);
    condition(stmt.condition(), scope);
    local_scope(stmt.body(), scope);
  }

  void while_loop(WhileLoop stmt, SymbolScope &scope)
  {
    match_if(While);
    condition(stmt.condition(), scope);
    local_scope(stmt.body(), scope);
  }

  void do_while_loop(DoWhileLoop stmt, SymbolScope &scope)
  {
    match_if(Do);
    local_scope(stmt.body(), scope);
    match_if(While);
    condition(stmt.condition(), scope);
    match_if(';');
  }

  void local_statement(LocalStmt stmt, SymbolScope &scope)
  {
    skip_node(stmt.attributes());
    expr(stmt.expr(), scope);
    match_if(';');
  }

  void namespace_decl(Namespace ns, SymbolScope &scope)
  {
    SymbolScope *sym = &scope;
    for (Id id : ns.identifier().children_of_type<Id>()) {
      sym = sym->scopes[string(id.str())];
    }
    skip_if(TokenType::Namespace);
    skip_node(ns.identifier());
    local_scope(ns.body(), *sym);
  }

  void enum_decl(ClassDecl decl, SymbolScope &scope)
  {
    SymbolScope &body_scope = *scope.scopes[string(decl.identifier().str())];
    LocalScope body = decl.body();

    IdQualified type = decl.parent_class();
    jump_to(decl.front());

    /* Treat enum values as static variables. */
    for (EnumValue val : body.children_of_type<EnumValue>()) {
      line(val.front());
      string type_str = string("const ") + string(type.str());
      builder << type_str;
      /* Pretty align. */
      builder.ss << string(max(size_t(0), val.front().char_number() - type_str.size()), ' ');

      SymbolVariable *var = id_var_decl_resolved(val.identifier(), body_scope);
      builder << string(" = ") + to_string(var->value);
      builder << string(";");
    }
  }

  void class_decl(ClassDecl decl,
                  SymbolScope &scope,
                  int nested_id = 0,
                  /* Resolved, templated class instance. */
                  SymbolClass *inst_cls = nullptr)
  {
    if (decl.is_enum()) {
      enum_decl(decl, scope);
      return;
    }

    SymbolClass *cls_ptr = inst_cls;
    if (!cls_ptr) {
      if (decl.identifier().is_valid()) {
        auto [res, err] = scope.lookup_class(decl.identifier());
        error(err);
        cls_ptr = res;
      }
      else {
        cls_ptr = scope.lookup_class("a" + to_string(nested_id));
      }
    }
    SymbolClass &cls = *cls_ptr;

    LocalScope body = decl.body();

    {
      /* First emit nested classes. */
      int class_id = 0;
      for (ClassDecl decl : body.children_of_type<ClassDecl>()) {
        class_decl(decl, cls, class_id);
        ++class_id;
      }
    }

    /* Rollback to front. */
    jump_to(decl.front());

    /* Then emit the class itself. */
    builder << "struct" + trivia(builder.curr);

    if (cls.is_anonymous) {
      builder.ss << cls.resolved->identifier << " ";
    }
    else {
      builder << cls.resolved->identifier + trivia(decl.identifier());
    }

    match_if('{');

    if (cls.is_union) {
      union_members_decl(cls);
    }
    else {
      class_members_decl(decl, cls);
    }

    jump_to(body.back());
    match_if('}');
    match_if(';');

    /* Don't do host shared structures. */
    if (!decl.attributes().contains_attr("host_shared")) {
      line(body.front());
      builder.ss << class_default_constructor(decl, cls) + "\n";
    }

    if (cls.is_union) {
      builder.ss << "\n";

      /* Emit getter and setters. */
      cls.visit_variables([&](SymbolVariable &var) {
        builder.ss << union_getter(cls, var) + "\n";
        builder.ss << union_setter(cls, var) + "\n";
      });
    }
    else {
      /* Emit static variables. */
      for (VarDecl decl : body.children_of_type<VarDecl>()) {
        if (decl.type().is_static()) {
          var_decl(decl, cls);
        }
      }

      /* Emit function prototypes. */
      if (body.child_first(NodeType::FuncDecl).is_valid()) {
        /* Prototypes are not needed with MSL wrapper class. */
        builder.ss << "\n#ifndef GPU_METAL\n";
        for (FuncDecl decl : body.children_of_type<FuncDecl>()) {
          func_forward_decl(decl, cls);
        }
        builder.ss << "#endif\n";
      }
      /* Emit function members. */
      for (FuncDecl decl : body.children_of_type<FuncDecl>()) {
        func_decl(decl, cls);
      }
    }
  }

  void class_members_decl(ClassDecl decl, SymbolClass &cls)
  {
    LocalScope body = decl.body();
    /* Emit member variables. */
    int member_count = 0;
    int class_id = 0;
    for (Node node : body.children_range()) {
      switch (node.type()) {
        case NodeType::VarDecl: {
          VarDecl decl = node;
          if (!decl.type().is_static()) {
            var_decl(decl, cls);
            member_count++;
          }
          break;
        }
        case NodeType::ClassDecl: {
          ClassDecl decl = node;
          if (decl.is_anonymous() && !decl.is_enum()) {
            /* Anonymous class are instantiated as regular members. */
            jump_to(decl.front());
            string id = "a" + to_string(class_id);
            id_type_resolved(id, cls);
            builder.ss << " " << id << "_;";
            member_count++;
          }
          ++class_id;
          break;
        }
        default:
          break;
      }
    }

    if (member_count == 0) {
      /* Add padding member. Empty class are invalid in GLSL. */
      builder.ss << "int _pad;";
    }
  }

  void union_members_decl(SymbolClass &cls)
  {
    for (int i = 0; i < cls.size; i += 16) {
      size_t member_size = cls.size - i;
      const char *data_type = "float4";
      if (member_size == 4) {
        data_type = "float";
      }
      else if (member_size == 8) {
        data_type = "float2";
      }
      else if (member_size == 12) {
        data_type = "float3";
      }
      builder.ss << string(data_type) << " _" << to_string(i / 16) << "; ";
    }
  }

  string union_ctor(SymbolClass &cls)
  {
    string members;
    for (int i = 0; i < cls.size; i += 16) {
      size_t member_size = cls.size - i;
      const char *data_type = "float4";
      if (member_size == 4) {
        data_type = "float";
      }
      else if (member_size == 8) {
        data_type = "float2";
      }
      else if (member_size == 12) {
        data_type = "float3";
      }
      SymbolClass *type = cls.root_scope()->lookup_class(data_type);
      members += "r._" + to_string(i / 16) + "=" + default_value(*type) + ";";
    }
    return members;
  }

  string default_value(const SymbolClass &type)
  {
    if (type.identifier == "float") {
      return "0.0f";
    }
    if (type.identifier == "uint" || type.identifier == "uchar") {
      return "0u";
    }
    if (type.identifier == "int" || type.identifier == "char") {
      return "0";
    }
    if (type.identifier == "bool") {
      return "false";
    }
    if (type.is_builtin) {
      return type.identifier + "(0)";
    }
    return type.identifier + "_ctor_()";
  };

  /* Return temp variable name for a given index. */
  static string get_temp_name(size_t index)
  {
    string name;
    /* Shift to 1-based for the bijective base-26 math. */
    size_t n = index + 1;
    while (n > 0) {
      n--; /* Map 0-25 to 'a'-'z'. */
      name += static_cast<char>('a' + (n % 26));
      n /= 26;
    }
    return name;
  }

  string class_default_constructor(ClassDecl decl, SymbolClass &cls)
  {
    const string &cls_id = cls.resolved->identifier;

    string members;
    if (cls.is_union) {
      members = union_ctor(cls);
    }
    else {
      int class_id = 0;
      for (Node node : decl.body().children_range()) {
        switch (node.type()) {
          case NodeType::VarDecl: {
            VarDecl decl = node;
            if (!decl.type().is_static()) {
              SymbolClass *type = id_type_lookup_resolved(decl.type().id(), cls);
              for (Declarator d : decl.children_of_type<Declarator>()) {
                SymbolVariable *var = cls.lookup_variable(d.identifier());
                string access;
                string close;
                int dim = 0;
                Subscript sub = d.array();
                while (sub.is_valid()) {
                  string var = get_temp_name(dim++);
                  string len = string(sub.expr().str());
                  members += "for(int " + var + " =0;" + var + " < " + len + ";++" + var + ") {";
                  close += "}";
                  access += "[" + var + "]";
                  sub = sub.sub();
                }
                members += "r." + var->identifier + access + "=" + default_value(*type) + ";";
                members += close;
              }
            }
            break;
          }
          case NodeType::ClassDecl: {
            ClassDecl decl = node;
            if (decl.is_anonymous() && !decl.is_enum()) {
              /* Anonymous class are instantiated as regular members. */
              jump_to(decl.front());
              string type_id = "a" + to_string(class_id);
              SymbolClass *type = cls.lookup_class(type_id)->resolved;
              string member_id = type_id + "_";

              members += "r." + member_id + "=" + default_value(*type) + ";";
            }
            ++class_id;
            break;
          }
          default:
            break;
        }
      }

      if (members.empty()) {
        /* Empty struct will have a padding int. */
        members += "r._pad=0;";
      }
    }

    return cls_id + " " + cls_id + "_ctor_() {" + cls_id + " r;" + members + "return r;}";
  }

  string union_data_access(const SymbolVariable &var, const size_t union_size)
  {
    const size_t member_offset = var.offset;
    const size_t member_size = var.type->size;
    string access = "_" + to_string(member_offset / 16);

    if (member_size == 12) {
      access += ".xyz";
    }
    else if (member_size == 8) {
      access += ((member_offset % 16) == 0) ? ".xy" : ".zw";
    }
    else if (member_size == 4) {
      switch (member_offset % 16) {
        case 0:
          /* Special case if last member is a scalar. */
          access += ((union_size - member_offset) == 4) ? "" : ".x";
          break;
        case 4:
          access += ".y";
          break;
        case 8:
          access += ".z";
          break;
        case 12:
          access += ".w";
          break;
      }
    }
    return access;
  }

  string member_from_float(const SymbolClass &member_type, const string &access)
  {
    /* Account for trivial types. */
    const string &type = member_type.resolved->identifier;

    if (type.starts_with("uint")) {
      return "floatBitsToUint(" + access + ")";
    }
    if (type.starts_with("int")) {
      return "floatBitsToInt(" + access + ")";
    }
    if (type == "bool32_t") {
      return access + " != 0";
    }
    return access;
  }

  string member_to_float(const SymbolClass &member_type, const string &access)
  {
    /* Account for trivial types. */
    const string &type = member_type.resolved->identifier;

    if (type.starts_with("uint")) {
      return "uintBitsToFloat(" + access + ")";
    }
    if (type.starts_with("int")) {
      return "intBitsToFloat(" + access + ")";
    }
    if (type == "bool32_t") {
      return "intBitsToFloat(int(" + access + "))";
    }
    return access;
  }

  string union_getter(SymbolClass &union_type, SymbolVariable &member)
  {
    SymbolClass &member_type = *member.type;
    const string &member_id = member.identifier;
    string union_type_id = union_type.resolved->identifier;
    string fn_type_id = member_type.resolved->identifier;
    string fn_name = "_" + member_id;
    string fn_args = "(" + union_type_id + " this_)";
    string fn_body = "{\n";
    if (member_type.is_builtin) {
      string access = "this_." + union_data_access(member, union_type.size);
      fn_body += "  return " + member_from_float(*member.type, access) + ";\n";
    }
    else {
      /* Declare return variable of the same type as the accessed member. */
      fn_body += "  " + fn_type_id + " r;\n";
      member_type.visit_variables([&](SymbolVariable &var) {
        if (var.is_static) {
          return;
        }
        string to_var = "r." + var.identifier;
        string access = "this_." + union_data_access(var, union_type.size);
        fn_body += "  " + to_var + " = " + member_from_float(*var.type, access) + ";\n";
      });
      fn_body += "  return val;\n";
    }
    fn_body += "}";
    return fn_type_id + " " + fn_name + fn_args + " " + fn_body;
  };

  string union_setter(SymbolClass &union_type, SymbolVariable &member)
  {
    SymbolClass &member_type = *member.type;
    const string &member_id = member.identifier;
    string union_type_id = union_type.resolved->identifier;
    string member_type_id = member_type.resolved->identifier;
    string fn_name = "_" + member_id + "_set_";
    string fn_args = "(" + union_type_id + " &this_, " + member_type_id + " v)";
    string fn_body = "{\n";
    if (member_type.is_builtin) {
      string to_var = "this_." + union_data_access(member, union_type.size);
      string access = "v." + member.identifier;
      fn_body += "  " + to_var + " = " + member_to_float(*member.type, access) + ";\n";
    }
    else {
      /* Declare return variable of the same type as the accessed member. */
      member_type.visit_variables([&](SymbolVariable &var) {
        if (var.is_static) {
          return;
        }
        string to_var = "this_." + union_data_access(var, union_type.size);
        string access = "val." + var.identifier;
        fn_body += "  " + to_var + " = " + member_to_float(*var.type, access) + ";\n";
      });
    }
    fn_body += "}";
    return "void " + fn_name + fn_args + " " + fn_body;
  };

  void func_forward_decl(FuncDecl decl, SymbolScope &scope)
  {
    SymbolScope &body_scope = *scope.functions[string(decl.identifier().str())];

    // jump_to(decl.front()); /* Adds too many directives. */
    skip_node(decl.attributes());
    id_type(decl.return_type(), scope);
    id_func_resolved(decl.identifier(), decl.arguments(), scope);
    func_arg_list(decl.arguments(), body_scope, false);
    builder << string(";\n");
  }

  void func_decl(FuncDecl decl,
                 SymbolScope &scope,
                 /* Resolved, templated function instance. */
                 SymbolFunction *inst_fn = nullptr)
  {
    LocalScope body = decl.body();

    if (inst_fn == nullptr) {
      inst_fn = id_func_resolved_lookup(decl.identifier(), decl.arguments(), scope);
    }

    jump_to(decl.front());
    skip_node(decl.attributes());
    /* Note that we match the id type from inside the function scope in order to match template
     * argument aliases.  */
    id_type(decl.return_type(), *inst_fn);

    builder.curr = decl.identifier().back();
    builder << inst_fn->resolved->identifier + trivia(decl.identifier());

    func_arg_list(decl.arguments(), *inst_fn);

    local_scope(body, *inst_fn);
  }

  void func_arg_list(FuncArgList list, SymbolScope &scope, bool with_trivia = true)
  {
    match_if('(');

    FuncDecl decl = list.parent();
    if (decl.is_method() && !decl.is_static()) {
      ClassDecl cls = decl.parent_class();
      if (decl.is_const()) {
        builder.ss << "const ";
      }
      id_type_resolved(cls.identifier(), scope);
      builder.ss << "&this_";
      if (!list.is_empty()) {
        builder.ss << ", ";
      }
      builder.curr = list.front().next();
    }

    if (builder.curr != ')') {
      for (FuncArg arg : list.children_of_type<FuncArg>()) {
        skip_node(arg.attributes());
        id_type(arg.type(), scope);
        declarator(arg.declarator(), scope);
        match_if(',');
      }
    }

    if (with_trivia) {
      match_if(')');
      skip_if(TokenType::Const);
    }
    else {
      builder << string(")");
      if (builder.curr == TokenType::Const) {
        builder.curr.next();
      }
    }
  }

  void template_inst(TemplateInst temp_decl, SymbolScope &scope)
  {
    if (temp_decl.is_class()) {
      ClassDecl decl = temp_decl.decl();
      auto *base_cls = scope.lookup_class_base(decl.identifier());
      /* Instantiation should have been checked already. */
      auto [temp_cls, _] = base_cls->template_data->lookup_inst(temp_decl.parameters(), scope);
      Node decl_tmp = base_cls->template_data->decl.decl();
      class_decl(decl_tmp, scope, 0, temp_cls);
    }
    else {
      FuncForwardDecl decl = temp_decl.decl();
      auto *base_fn = scope.lookup_function_base(decl.identifier());
      /* Instantiation should have been checked already. */
      auto [temp_fn, _] = base_fn->template_data->lookup_inst(temp_decl.parameters(), scope);
      Node decl_tmp = base_fn->template_data->decl.decl();
      func_decl(decl_tmp, *temp_fn->parent, temp_fn);
    }
  }

  void template_spec(TemplateSpec temp_spec, SymbolScope &scope)
  {
    if (temp_spec.is_class()) {
      ClassDecl decl = temp_spec.decl();
      auto *base_cls = scope.lookup_class_base(decl.identifier());
      /* Instantiation should have been checked already. */
      auto [temp_cls, _] = base_cls->template_data->lookup_inst(temp_spec.parameters(), scope);
      class_decl(decl, scope, 0, temp_cls);
    }
    else {
      FuncDecl decl = temp_spec.decl();
      auto *base_fn = scope.lookup_function_base(decl.identifier());
      /* Instantiation should have been checked already. */
      auto [temp_fn, _] = base_fn->template_data->lookup_inst(temp_spec.parameters(), scope);
      func_decl(decl, *temp_fn->parent, temp_fn);
    }
  }

  /* Match if/else if/else statements */
  void if_statement(Node decl, SymbolScope &scope)
  {
    match_if(Else);
    match_if(If);
    match_if(Constexpr);

    Condition cond(decl.child_first());
    if (cond.is_valid()) {
      condition(cond, scope);
    }
    local_scope(decl.child_last(NodeType::LocalScope), scope);
  }

  void condition(Condition cond, SymbolScope &scope)
  {
    match_if('(');
    for (Node child : cond.children_range()) {
      if (child.type() == NodeType::VarDecl) {
        var_decl(child, scope, false);
      }
      else if (child.type() == NodeType::LocalStmt) {
        local_statement(child, scope);
      }
      else {
        /* TODO: structured bindings. */
        assert(0);
      }
      match_if(';');
    }
    match_if(')');
    skip_node(AttrList(cond.next()));
  }

  void expr(Node decl, SymbolScope &scope)
  {
    for (Node child : decl.children_range()) {
      switch (child.type()) {
        case NodeType::LocalVar:
          local_var(scope, child, scope);
          break;
        case NodeType::FuncCall:
          func_call(scope, child, scope);
          break;
        case NodeType::InitializerList:
          initializer_list(child, scope);
          break;
        case NodeType::Constructor:
        case NodeType::Expr:
          expr(child, scope);
          break;
        case NodeType::ExprSub:
          match_if('(');
          expr(child, scope);
          match_if(')');
          break;
        case NodeType::Subscript:
          /* Subscript operator should have already been processed. */
          builder.curr = child.back().next();
          break;
        case NodeType::TemplateExplicit:
          skip_if(Template);
          expr(child, scope);
          break;
        case NodeType::Op:
          if (child.front() == '.') {
            builder.curr = child.back().next();
            /* Dot member access operator right operand should have already been processed. */
            break;
          }
          [[fallthrough]];
        case NodeType::StringConst:
        case NodeType::NumConst:
        default:
          builder << child;
          break;
      }
    }
  }

  void subscript(Subscript stmt, SymbolScope &scope)
  {
    match_if('[');
    if (stmt.expr().is_valid()) {
      expr(stmt.expr(), scope);
    }
    match_if(']');
    if (stmt.sub().is_valid()) {
      subscript(stmt.sub(), scope);
    }
  }

  void var_decl(VarDecl decl, SymbolScope &scope, bool jump = true)
  {
    if (jump) {
      jump_to(decl.front());
    }
    skip_node(decl.attributes());

    if (decl.type().id().str() == "auto") {
      auto [cls, err] = symbols.resolve_auto_type(scope, decl.child_first(NodeType::Declarator));
      if (err) {
        error(err->node, err->msg);
      }
      builder << cls->resolved->identifier + trivia(decl.type().id());
    }
    else {
      id_type(decl.type(), scope);
    }
    for (Declarator d : decl.children_of_type<Declarator>()) {
      declarator(d, scope);
      match_if(',');
    }
    match_if(';');
  }

  void id_type(IdType type, SymbolScope &scope)
  {
    skip_if(Static);
    match_if(TokenType::Const, Constexpr);
    skip_if(Struct, Class, Enum);
    id_type_resolved(type.id(), scope);
  }

  /* Process the next member access operator at the right of the node.
   * cls is the class being accessed.
   * Does nothing if there is no access operator following node. */
  void member_access(SymbolScope &expr_scope,
                     Node node,
                     SymbolClass *cls,
                     SymbolVariable *var = nullptr)
  {
    node = node.next();
    if (node.type() == NodeType::Op && node.front() == TokenType::Dot) {
      builder << node;
      Node member = node.next();
      if (cls == nullptr) {
        /* TODO(fclem): Error. */
      }
      else if (member.type() == NodeType::FuncCall) {
        func_call(expr_scope, member, *cls, true);
      }
      else {
        local_var(expr_scope, member, *cls, true);
      }
    }
    else if (node.type() == NodeType::Subscript) {
      subscript(node, expr_scope);
      /* TODO(fclem): Make this work for subscript operator on result of array deref.
       * Currently the subscript AST node contains all the subscript scopes nested.
       * We might want to not nest them inside expressions. */
      if (var && var->array_dimensions > 0) {
        /* TODO error if dimension mismatch. */
        member_access(expr_scope, node, var->type);
      }
      else if (SymbolFunction *fn = cls->operator_subscript; fn) {
        cls = fn->return_type;
        Subscript sub = node;
        while (sub.sub().is_valid()) {
          sub = sub.sub();
          cls = cls->operator_subscript->return_type;
        }
        member_access(expr_scope, node, cls);
      }
    }
  }

  bool is_member_accessed(Node node)
  {
    node = node.prev();
    return (node.type() == NodeType::Op && node.front() == TokenType::Dot) ||
           (node.type() == NodeType::Subscript);
  }

  void local_var(SymbolScope &expr_scope, LocalVar var, SymbolScope &scope, bool accessed = false)
  {
    if (is_member_accessed(var) && !accessed) {
      /* Dot member access operator right operand should have already been processed. */
      builder.curr = var.back().next();
      return;
    }

    SymbolVariable *sym = id_var_resolved(var.identifier(), scope);

    if (!sym->is_constexpr) {
      member_access(expr_scope, var, sym->type, sym);
    }

    builder.curr = var.back().next();
  }

  void func_call(SymbolScope &expr_scope, FuncCall call, SymbolScope &scope, bool accessed = false)
  {
    if (is_member_accessed(call) && !accessed) {
      /* Dot member access operator right operand should have already been processed. */
      builder.curr = call.back().next();
      return;
    }

    SymbolFunction *sym = id_func_resolved(call.identifier(), call.parameters(), scope);
    func_param_list(call.parameters(), expr_scope, sym);

    member_access(expr_scope, call, sym->return_type);

    builder.curr = call.back().next();
  }

  void func_param_list(FuncParamList list, SymbolScope &scope, SymbolFunction *sym)
  {
    match_if('(');

    if (sym->fn_type == SymbolFunction::MEMBER && list.parent().prev().back() != '.') {
      builder.ss << "this_";
      if (!list.is_empty()) {
        builder.ss << ", ";
      }
      builder.curr = list.front().next();
    }

    if (builder.curr != ')') {
      for (Expr param : list.children_of_type<Expr>()) {
        expr(param, scope);
        match_if(',');
      }
    }

    match_if(')');
  }

  void declarator(Declarator decl, SymbolScope &scope)
  {
    const bool par = match_if('(');
    match_if('&');
    id_var_decl_resolved(decl.identifier(), scope);
    if (par) {
      match_if(')');
    }
    if (decl.is_array()) {
      builder << decl.array();
    }

    SymbolVariable *var = scope.lookup_variable(decl.identifier());
    if (!var->is_error && var->is_constexpr) {
      builder.curr = decl.back();
      builder << "= " + to_string(var->value);
      return;
    }

    if (decl.initial_value().is_valid()) {
      assignment(decl.initial_value(), scope);
    }
    else if (decl.initializer_list().is_valid()) {
      initializer_list(decl.initializer_list(), scope);
    }
  }

  void assignment(AssignStmt stmt, SymbolScope &scope)
  {
    match_if('=');
    init_expression_or_initializer_list(stmt.child_first(), scope);
  }

  void init_expression_or_initializer_list(Node node, SymbolScope &scope)
  {
    if (node == NodeType::InitializerList) {
      initializer_list(node, scope);
    }
    else if (node == NodeType::Expr) {
      expr(node, scope);
    }
  }

  void initializer_list(InitializerList list, SymbolScope &scope)
  {
    match_if('{');
    const bool designated = (list.child_first().type() == NodeType::DesignatedInitializer);
    for (Node child : list.children_range()) {
      if (designated) {
        match_if('.');
        match_if(Word);
        assignment(child.child_last(), scope);
      }
      else {
        init_expression_or_initializer_list(child.child_last(), scope);
      }
      match_if(',');
    }
    match_if('}');
  }

  SymbolClass *id_type_lookup_resolved(IdQualified id, const SymbolScope &scope)
  {
    assert(id.is_valid());
    auto [cls, err] = scope.lookup_class(id);
    error(err);
    if (cls->template_data && !id.template_params().is_valid()) {
      error(id, "Missing explicit template arguments");
    }
    else if (cls->is_error) {
      error(id, "Unknown type name");
    }
    if (cls->resolved) {
      return cls->resolved;
    }
    return cls;
  }

  void id_type_resolved(IdQualified id, const SymbolScope &scope)
  {
    auto *cls_resolved = id_type_lookup_resolved(id, scope);
    builder.curr = id.back();
    builder << cls_resolved->identifier + trivia(id);
  }

  /* Lookup only in the give scope. */
  SymbolClass *id_type_resolved(string id, const SymbolScope &scope)
  {
    auto it = scope.classes.find(id);
    SymbolClass *cls = it->second;
    builder.ss << cls->resolved->identifier;
    return cls;
  }

  template<typename ArgOrParamList>
  SymbolFunction *id_func_resolved_lookup(IdQualified id,
                                          ArgOrParamList params,
                                          const SymbolScope &scope)
  {
    assert(id.is_valid());
    auto [func, err] = scope.lookup_function(id);
    error(err);
    if (func->template_data && !id.template_params().is_valid()) {
      auto [func_, err_adl] = func->template_data->lookup_adl(symbols, params, scope);
      error(err_adl);
      func = func_;
    }
    else if (func->overload_next) {
      auto [arg_types, err_args] = SymbolFunction::to_arg_types(symbols, scope, params);
      error(err_args);
      SymbolFunction *overload = func->lookup_overload(arg_types);
      if (overload->is_error) {
        error(id, "No matching function for call to '" + func->identifier + "'");
      }
      func = overload;
    }
    else if (func->is_error) {
      /* Try to resolve type constructors (only for builtins for now). */
      if (auto [cls, err_cls] = scope.lookup_class(id); cls->resolved && cls->resolved->is_builtin)
      {
        return scope.root_scope()->lookup_function(cls->resolved->identifier);
      }
      error(id, "Unknown function name");
    }
    return func;
  }

  template<typename ArgOrParamList>
  SymbolFunction *id_func_resolved(IdQualified id, ArgOrParamList params, const SymbolScope &scope)
  {
    SymbolFunction *func = id_func_resolved_lookup(id, params, scope);
    builder.curr = id.back();
    builder << func->resolved->identifier + trivia(id);
    return func;
  }

  /* Resolve a variable in an expression. */
  SymbolVariable *id_var_resolved(IdQualified id, const SymbolScope &scope)
  {
    assert(id.is_valid());
    SymbolVariable *var = scope.lookup_variable(id);
    if (var->is_error) {
      error(id, "Unknown variable");
      builder << id;
      return var;
    }

    if (var->is_constexpr) {
      builder.ss << to_string(var->value) + trivia(id);
      return var;
    }

    bool preceded_by_dot = id.front().prev() == '.';
    if (!var->is_static && !preceded_by_dot && var->parent->as_class() != nullptr) {
      builder.ss << "this_.";
    }

    if (var->resolved) {
      builder.curr = id.back();
      builder << var->resolved->identifier + trivia(id);
    }
    else {
      builder << id;
    }
    return var;
  }

  /* Resolve a variable in a declaration. */
  SymbolVariable *id_var_decl_resolved(IdQualified id, const SymbolScope &scope)
  {
    assert(id.is_valid());
    SymbolVariable *var = scope.lookup_variable(id);
    if (var->is_error) {
      error(id, "Unknown variable");
      builder << id;
      return var;
    }
    /* Note we only resolve static variable. */
    if (var->is_static) {
      builder.curr = id.back();
      builder << var->resolved->identifier + trivia(id);
    }
    else {
      builder << id;
    }
    return var;
  }
};

/* Lower namespaces by adding namespace prefix to all the contained structs and functions. */
void SourceProcessor::lower_namespaces_ast(Parser &parser, SymbolTable &symbols)
{
  /* First, create original <-> mangled symbol mapping. */
  // string fn = "000 " + filename;
  // auto new_root = make_unique<SymbolScope>(fn, LocalScope(parser.root()), nullptr);
  // flatten_symbols_recursive(*symbols.root, *new_root, "");

  // symbols.root->print();
  // symbols.flatten_root->print();
  // new_root->print();

  InstantiationContext ctx(parser, symbols, error_handler);
  string str = ctx.process(*symbols.root, LocalScope(Node(&parser, 0)));

  if (error_handler.err.has_value()) {
    throw ParserException();
  }

  parser.set_str(str);
}

}  // namespace blender::gpu::shader
