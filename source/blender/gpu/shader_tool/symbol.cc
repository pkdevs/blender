/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "expression.hh"
#include "processor.hh"
#include "symbol.hh"
#include "type_checker.hh"

namespace blender::gpu::shader::parser {

using namespace ast;
using namespace std;

static const char *subscript_operator_id = "arr_op_";
static const char *ns_sep = SourceProcessor::namespace_separator;

Result<pair<ResourceType, ResourceTableType>> resource_type_from_attributes(AttrList list)
{
  static unordered_map<string, pair<ResourceType, ResourceTableType>> attr_map = {
      /* Noop, to remove. */
      {"host_shared", {ResourceType::NONE, ResourceTableType::NONE}},
      /* Vertex inputs. */
      {"attribute", {ResourceType::VERT_ATTR_IN, ResourceTableType::VERTEX_IN}},
      /* Vertex outputs. */
      {"flat", {ResourceType::VERT_ATTR_OUT, ResourceTableType::VERTEX_OUT}},
      {"no_perspective", {ResourceType::VERT_ATTR_OUT, ResourceTableType::VERTEX_OUT}},
      {"smooth", {ResourceType::VERT_ATTR_OUT, ResourceTableType::VERTEX_OUT}},
      /* Fragment outputs. */
      {"frag_color", {ResourceType::FRAG_OUT, ResourceTableType::FRAGMENT_OUT}},
      {"frag_stencil_ref", {ResourceType::FRAG_OUT, ResourceTableType::FRAGMENT_OUT}},
      /* Fragment inputs. */
      {"subpass_input", {ResourceType::FRAG_IN, ResourceTableType::NONE}},
      /* Entry point type. */
      {"vertex", {ResourceType::NONE, ResourceTableType::NONE}},
      {"fragment", {ResourceType::NONE, ResourceTableType::NONE}},
      {"compute", {ResourceType::NONE, ResourceTableType::NONE}},
      /* Entry point attrib. */
      {"early_fragment_tests", {ResourceType::NONE, ResourceTableType::NONE}},
      {"num_work_groups", {ResourceType::NONE, ResourceTableType::NONE}},
      {"texture_atomic", {ResourceType::NONE, ResourceTableType::NONE}},
      {"metal_max_total_threads_per_threadgroup", {ResourceType::NONE, ResourceTableType::NONE}},
      {"local_size", {ResourceType::NONE, ResourceTableType::NONE}},
      /* Resource table. */
      {"compilation_constant",
       {ResourceType::COMPILATION_CONST, ResourceTableType::RESOURCE_TABLE}},
      {"legacy_info", {ResourceType::LEGACY_INFO, ResourceTableType::RESOURCE_TABLE}},
      {"push_constant", {ResourceType::PUSH_CONST, ResourceTableType::RESOURCE_TABLE}},
      {"resource_table", {ResourceType::RESOURCE_TABLE, ResourceTableType::RESOURCE_TABLE}},
      {"shared", {ResourceType::SHARED, ResourceTableType::RESOURCE_TABLE}},
      {"sampler", {ResourceType::SAMPLER, ResourceTableType::RESOURCE_TABLE}},
      {"specialization_constant",
       {ResourceType::SPECIALIZATION_CONST, ResourceTableType::RESOURCE_TABLE}},
      {"uniform", {ResourceType::UNIFORM_BUF, ResourceTableType::RESOURCE_TABLE}},
      {"storage", {ResourceType::STORAGE_BUF, ResourceTableType::RESOURCE_TABLE}},
      {"image", {ResourceType::IMAGE, ResourceTableType::RESOURCE_TABLE}},
      /* Entry point argument. */
      {"base_instance", {ResourceType::BASE_INSTANCE, ResourceTableType::ENTRY_POINT}},
      {"clip_control", {ResourceType::CLIP_CONTROL, ResourceTableType::ENTRY_POINT}},
      {"clip_distance", {ResourceType::CLIP_DISTANCE, ResourceTableType::ENTRY_POINT}},
      {"front_facing", {ResourceType::FRONT_FACING, ResourceTableType::ENTRY_POINT}},
      {"global_invocation_id",
       {ResourceType::GLOBAL_INVOCATION_ID, ResourceTableType::ENTRY_POINT}},
      {"in", {ResourceType::IN, ResourceTableType::ENTRY_POINT}},
      {"instance_id", {ResourceType::INSTANCE_ID, ResourceTableType::ENTRY_POINT}},
      {"instance_index", {ResourceType::INSTANCE_INDEX, ResourceTableType::ENTRY_POINT}},
      {"layer", {ResourceType::LAYER, ResourceTableType::ENTRY_POINT}},
      {"local_invocation_id", {ResourceType::LOCAL_INVOCATION_ID, ResourceTableType::ENTRY_POINT}},
      {"local_invocation_index",
       {ResourceType::LOCAL_INVOCATION_INDEX, ResourceTableType::ENTRY_POINT}},
      {"out", {ResourceType::OUT, ResourceTableType::ENTRY_POINT}},
      {"point_coord", {ResourceType::POINT_COORD, ResourceTableType::ENTRY_POINT}},
      {"point_size", {ResourceType::POINT_SIZE, ResourceTableType::ENTRY_POINT}},
      {"position", {ResourceType::POSITION, ResourceTableType::ENTRY_POINT}},
      {"subpass_in", {ResourceType::SUBPASS_IN, ResourceTableType::ENTRY_POINT}},
      {"vertex_id", {ResourceType::VERTEX_ID, ResourceTableType::ENTRY_POINT}},
      {"viewport_index", {ResourceType::VIEWPORT_INDEX, ResourceTableType::ENTRY_POINT}},
      {"work_group_id", {ResourceType::WORK_GROUP_ID, ResourceTableType::ENTRY_POINT}},
      {"num_work_groups", {ResourceType::NUM_WORK_GROUP, ResourceTableType::ENTRY_POINT}},
      {"frag_depth", {ResourceType::FRAG_DEPTH, ResourceTableType::ENTRY_POINT}},
      {"frag_coord", {ResourceType::FRAG_COORD, ResourceTableType::ENTRY_POINT}},
      {"frag_stencil_ref", {ResourceType::FRAG_STENCIL_REF, ResourceTableType::ENTRY_POINT}},
      /* Misc. */
      {"fallthrough", {ResourceType::NONE, ResourceTableType::NONE}},
      {"maybe_unused", {ResourceType::NONE, ResourceTableType::NONE}},
      {"nodiscard", {ResourceType::NONE, ResourceTableType::NONE}},
      {"node", {ResourceType::NONE, ResourceTableType::NONE}},
      {"static_branch", {ResourceType::NONE, ResourceTableType::NONE}},
      {"unroll", {ResourceType::NONE, ResourceTableType::NONE}},
      {"condition", {ResourceType::NONE, ResourceTableType::NONE}},
      {"frequency", {ResourceType::NONE, ResourceTableType::NONE}},
      {"unroll_n", {ResourceType::NONE, ResourceTableType::NONE}},
      {"index", {ResourceType::NONE, ResourceTableType::NONE}},
      {"raster_order_group", {ResourceType::NONE, ResourceTableType::NONE}},
  };

  Result<pair<ResourceType, ResourceTableType>> result;
  for (Attr attr : list.children_of_type<Attr>()) {
    if (auto it = attr_map.find(string(attr.identifier().str())); it != attr_map.end()) {
      if (it->second.first != ResourceType::NONE) {
        if (result.value.first == ResourceType::NONE) {
          result.value = it->second;
        }
        else {
          result.err = AstNodeException(
              attr, "Declaration is already of type '" + to_str(result.value.first) + "'");
        }
      }
    }
  }
  return result;
}

Result<SymbolClass *> SymbolTable::expr_type_analysis(const SymbolScope &scope, Expr expr) const
{
  assert(expr.is_valid());
  return ExpressionTypeParser(this, &scope, expr.child_first()).eval();
}

Result<SymbolClass *> SymbolTable::resolve_auto_type(SymbolScope &scope, Declarator decl) const
{
  std::optional<AstNodeException> err;
  Node node = decl.child_last();
  AssignStmt assign(node);

  if (Expr expr = assign.expr(); expr.is_valid()) {
    return expr_type_analysis(scope, expr);
  }

  InitializerList list(node);
  if (!list.is_valid()) {
    list = assign.initializer_list();
  }

  if (list.is_valid()) {
    Initializer init(list.child_first());
    if (init.child_first().type() == NodeType::InitializerList) {
      err = {init.child_first(),
             "Cannot deduce type for variable '" + string(decl.identifier().str()) +
                 "' with type 'auto' from nested initializer list"};
    }
    else if (init.next().is_valid()) {
      err = {init.next(),
             "Initializer for variable '" + string(decl.identifier().str()) +
                 "' with type 'auto' contains multiple expressions"};
    }
    else {
      return expr_type_analysis(scope, Expr(init.child_first()));
    }
  }

  return {scope.root_scope()->lookup_class(SymbolTable::err_symbol), err};
}

/* Return size padded to the given alignment. */
static int pad(int size, int align)
{
  return ((size + align - 1) / align) * align;
}

Result<StringPair> SymbolTable::mangle_identifier(TemplateArgList args,
                                                  TemplateParamList list,
                                                  const SymbolScope &scope)
{
  Expr param = list.child_first();

  std::optional<AstNodeException> err;

  StringPair result = {};
  for (TemplateArg arg : args.children_range()) {
    assert(arg.is_valid());
    if (!param.is_valid()) {
      err = AstNodeException(
          list, "Missing argument for template parameter '" + string(arg.type().str()) + "'");
    }
    else if (arg.front() == Typename) {
      if (param.child_count() != 1 || param.child_first().type() != NodeType::LocalVar) {
        err = AstNodeException(
            param, "Invalid argument for template parameter '" + string(arg.type().str()) + "'");
      }
      else {
        IdQualified type_id(LocalVar(param.child_first()).identifier());
        auto *type = scope.lookup_class_base(type_id);
        if (type->template_data) {
          TemplateParamList list_nested = type_id.template_params();
          if (!list_nested.is_valid()) {
            err = AstNodeException(
                type_id, "Missing template parameters for '" + string(type_id.str()) + "'");
          }
          else {
            /* Recursive. */
            auto [type_, _, err_] = type->template_data->lookup_inst(list_nested, scope);
            type = type_;
            if (err_) {
              err = err_;
            }
          }
        }
        if (type->is_error) {
          err = AstNodeException(param,
                                 "Use of undeclared identifier '" + string(type_id.str()) + "'");
        }
        else {
          result.str += "T" + type->resolved->identifier;
          result.str_debug += ", " + type->resolved->identifier;
        }
      }
    }
    else {
      auto [val, err_] = SymbolTable::evaluate_constexpr(scope, param.child_first());
      if (err_) {
        err = err_;
      }
      /* Replace minus sign by underscore. */
      result.str += string("T") + (val < 0 ? "_" : "") + to_string(abs(val));
      result.str_debug += ", " + to_string(val);
    }
    param = param.next();
  }
  return {result, err};
}

template<typename T>
ResultPair<T *, string> SymbolTemplate<T>::lookup_inst(TemplateParamList list,
                                                       const SymbolScope &scope) const
{
  auto [mangled, err] = SymbolTable::mangle_identifier(decl.arguments(), list, scope);
  auto [args, args_debug] = mangled;
  auto it = instances.find(args);
  string full_id = string(id().str()) + "<" + args_debug.substr(2) + ">";

  if (it == instances.end() && !err) {
    err = {list, "Missing explicit instantiation of template '" + full_id + "'"};
  }

  if (err) {
    if constexpr (is_same_v<T, SymbolFunction>) {
      return {scope.root_scope()->lookup_function(SymbolTable::err_symbol), full_id, err};
    }
    else {
      return {scope.root_scope()->lookup_class(SymbolTable::err_symbol), full_id, err};
    }
  }
  return {it->second, full_id, err};
}

template<typename T> void SymbolTemplate<T>::init_adl()
{
  TemplateDecl temp = this->decl;
  if (temp.is_class()) {
    return;
  }
  TemplateArgList tmp_args(temp.arguments());

  int tmp_arg_count = tmp_args.child_count();
  if (tmp_arg_count == 0) {
    return;
  }
  temp_arg_index_in_fn_arg.reserve(tmp_arg_count);

  FuncDecl decl(temp.decl());
  for (FuncArg arg : decl.arguments().children_of_type<FuncArg>()) {
    int arg_id = -1;
    int id = 0;
    for (TemplateArg tmp_arg : tmp_args.children_of_type<TemplateArg>()) {
      if (tmp_arg.id().str() == arg.type().id().str()) {
        arg_id = id;
      }
      ++id;
    }
    if (arg_id != -1) {
      temp_arg_index_in_fn_arg.emplace_back(arg_id);
    }
  }

  if (temp_arg_index_in_fn_arg.size() != tmp_arg_count) {
    temp_arg_index_in_fn_arg.clear();
  }
}

Result<StringPair> SymbolTable::mangle_identifier(const SymbolFunctionTemplate &tmp,
                                                  FuncParamList list,
                                                  const SymbolScope &scope) const
{
  vector<SymbolClass *> arg_cls;
  for (Expr expr : list.children_of_type<Expr>()) {
    auto [type, _] = expr_type_analysis(scope, expr);
    arg_cls.emplace_back(type);
  }

  StringPair result;
  for (int i : tmp.temp_arg_index_in_fn_arg) {
    if (i < arg_cls.size()) {
      result.str += "T" + arg_cls[i]->resolved->identifier;
      result.str_debug += ", " + arg_cls[i]->resolved->identifier;
    }
    else {
      result.str += "T" + err_symbol;
      result.str_debug += ", " + err_symbol;
    }
  }
  return {result, {}};
}

template<typename T>
Result<T *> SymbolTemplate<T>::lookup_adl(const SymbolTable &symbols,
                                          FuncParamList list,
                                          const SymbolScope &scope) const
{
  if constexpr (is_same_v<T, SymbolClass>) {
    return {scope.root_scope()->lookup_class(SymbolTable::err_symbol),
            AstNodeException(list, "Cannot use ADL on types")};
  }
  else {
    if (!is_adl_possible()) {
      return {scope.root_scope()->lookup_function(SymbolTable::err_symbol),
              AstNodeException(list.prev(), "Missing explicit template arguments")};
    }
    auto [mangled, err] = symbols.mangle_identifier(*this, list, scope);
    auto [args, args_debug] = mangled;
    auto it = instances.find(args);
    if (it == instances.end() && !err) {
      err = {list,
             "Missing explicit instantiation of template '" + string(id().str()) + "<" +
                 args_debug.substr(2) + ">'"};
    }

    if (err) {
      return {scope.root_scope()->lookup_function(SymbolTable::err_symbol), err};
    }
    return {it->second, err};
  }
}

template struct SymbolTemplate<SymbolClass>;
template struct SymbolTemplate<SymbolFunction>;

SymbolClass::SymbolClass(SymbolScope *parent, ClassDecl decl, const string &suffix)
    : SymbolScope(parent,
                  decl.front(),
                  decl.identifier().str().empty() ?
                      string("a") + std::to_string(parent->classes.size()) :
                      string(decl.identifier().name().str()) + suffix,
                  CLASS)

{
  this->is_anonymous = decl.identifier().str().empty();
  this->is_union = decl.front() == Union;
  this->is_enum = decl.front() == Enum;
}

void SymbolClass::ensure_size_and_align(NodeErrorHandler *err_handler, bool check_st430)
{
  if (size != -1 && (err_handler == nullptr || is_builtin)) {
    return;
  }
  size = 0;
  align = 1;
  is_std140_compatible = true;
  is_std430_compatible = true;
  for (SymbolVariable *var : non_static_variables_in_declaration_order()) {
    SymbolClass &cls = *var->type;

    if (err_handler && !cls.is_enum) {
      auto scope = err_handler->note(var->loc.tok,
                                     "In declaration of '" + var->identifier + "' of type '" +
                                         cls.identifier + "'");
      cls.ensure_size_and_align(err_handler, check_st430);
    }
    else {
      cls.ensure_size_and_align();
    }

    int member_size = cls.size;
    if (var->array_elements == 0) {
      /* Not an array. */
    }
    else if (var->array_elements == -1) {
      /* Array size need to be known at compile time for shared structs. */
      is_std140_compatible = false;
      is_std430_compatible = false;
      if (err_handler) {
        err_handler->error(var->loc.tok, "Array size is not a constant expression");
      }
    }
    else {
      if (cls.size == 12) {
        /* Arrays of float3 padded to 16 bytes for each element for std430. */
        is_std140_compatible = false;
        if (err_handler) {
          err_handler->error(var->loc.tok,
                             "Implicit padding of 4 bytes per array element, do not use 12 bytes "
                             "types in arrays");
        }
      }
      else if (cls.size % 16 != 0) {
        /* Arrays are padded to 16 bytes for each element for std140. */
        is_std140_compatible = false;
        if (err_handler && !check_st430) {
          err_handler->error(var->loc.tok,
                             "Implicit padding of " + to_string(16 - (cls.size % 16)) +
                                 " bytes per array element, use a 16 bytes type or use "
                                 "a storage buffer");
        }
      }
      member_size *= var->array_elements;
    }

    /* Inherit base compatibility from the member. */
    is_std140_compatible &= cls.is_std140_compatible;
    is_std430_compatible &= cls.is_std430_compatible;

    if (err_handler) {
      string hint;
      if (cls.is_enum) {
        hint = " because its underlying type is not 'uint' or 'int'";
      }
      else if (cls.resolved->identifier == "float3") {
        hint = "; use 'packed_float3' instead";
      }
      else if (cls.resolved->identifier == "int3") {
        hint = "; use 'packed_int3' instead";
      }
      else if (cls.resolved->identifier == "uint3") {
        hint = "; use 'packed_uint3' instead";
      }
      else if (cls.resolved->identifier == "bool") {
        hint = "; use 'bool32_t' instead";
      }
      else if (cls.resolved->identifier == "float3x3") {
        hint = "; use 'float3x4' instead";
      }

      if (!is_std140_compatible && !check_st430) {
        err_handler->error(var->loc.tok,
                           "Type '" + var->type->identifier +
                               "' is not allowed in uniform and storage buffer" + hint);
      }
      else if (!is_std430_compatible) {
        err_handler->error(var->loc.tok,
                           "Type '" + var->type->identifier +
                               "' is not allowed in storage buffer" + hint);
      }
    }

    if (this->is_union) {
      size = max(size, member_size);
    }
    else {
      /* Padding to next member alignment.
       * Force struct and arrays to all have alignment of float4. */
      int align = (cls.is_builtin || cls.is_enum) && var->array_dimensions == 0 ? cls.align : 16;
      int padded = pad(size, align);
      /* Consider class compatible if it has no implicit padding. */
      is_std140_compatible &= padded == size;
      is_std430_compatible &= padded == size;
      if (err_handler && padded != size) {
        err_handler->error(var->loc.tok,
                           "Implicit " + to_string(padded - size) + " bytes padding before '" +
                               var->identifier + "', add manual padding");
      }
      size = padded;
      /* Add member size. */
      size += member_size;
    }
    /* Update the overall alignment of the current struct. */
    align = max(align, cls.align);

    if (!cls.is_builtin && !cls.is_enum && cls.size % 16 != 0) {
      is_std140_compatible = false;
      if (err_handler && !check_st430) {
        err_handler->error(
            var->loc.tok,
            "Type '" + var->type->identifier +
                "' is not allowed in uniform buffer because it contains implicit padding");
      }
    }
  }

  /* The total size of the struct must be a multiple of its alignment. */
  if (align > 0) {

    int padded = pad(size, align);
    if (padded != size) {
      is_std140_compatible = false;
      is_std430_compatible = false;
      if (err_handler) {
        err_handler->error(loc.tok,
                           "Implicit " + to_string(padded - size) +
                               " bytes padding at the end of '" + identifier +
                               "' , add manual padding");
      }
    }
    size = padded;
  }

#if 0 /* For debugging. */
  std::cout << "size:" << size << " align:" << align << " id:" << identifier << std::endl;
#endif

  if (size == 0) {
    is_std140_compatible = false;
    is_std430_compatible = false;

    if (err_handler) {
      err_handler->error(loc.tok, "Empty classes are not supported in storage or uniform buffers");
    }
  }
  /* The overall size and alignment of a struct in std140 must be rounded up to the base alignment
   * of a vec4 (16 bytes). */
  if (size % 16 != 0) {
    is_std140_compatible = false;

    if (err_handler && !check_st430) {
      err_handler->error(loc.tok,
                         "Implicit " + to_string(16 - (size % 16)) +
                             " bytes padding at the end of '" + identifier +
                             "' , add manual padding");
    }
  }
}

struct SymbolParser : NodeErrorHandler {
  SymbolTable &table;
  SymbolScope &global;

  SymbolParser(ErrorHandler &handler, SymbolTable &table, SymbolScope &global)
      : NodeErrorHandler(handler), table(table), global(global)
  {
  }

  void parse_scope(SymbolScope &scope,
                   Node node,
                   const std::string &prefix,
                   TemplateInst template_inst = {})
  {
    Node parent = node.parent();

    if (parent.parent().type() == NodeType::TemplateDecl) {
      parse_template_arguments(scope, parent.parent(), template_inst);
    }

    if (parent.type() == NodeType::FuncDecl) {
      parse_function_return_type(scope, parent);
      parse_function_arguments(scope, parent);
    }
    if (parent.type() == NodeType::ForLoop) {
      parse_loop_arguments(scope, parent);
    }

    int offset = 0;

    if (node.type() == NodeType::FuncDecl || node.type() == NodeType::ClassDecl ||
        node.type() == NodeType::LocalScope)
    {
      for (Node child = node.child_first(); child.is_valid(); child = child.next()) {
        switch (child.type()) {
          case NodeType::Preprocessor:
            parse_preprocessor(scope, child);
            break;
          case NodeType::Namespace:
            parse_namespace(scope, child, prefix);
            break;
          case NodeType::UsingStmt:
            parse_using_stmt(scope, child);
            break;
          case NodeType::LocalScope:
            parse_local_scope(scope, child, prefix);
            break;
          case NodeType::ClassDecl:
            parse_class_decl(scope, child, parent, offset, prefix);
            break;
          case NodeType::EnumValue:
            parse_enum_value(scope, child, parent, prefix);
            break;
          case NodeType::FuncDecl:
            parse_func_decl(scope, child, prefix);
            break;
          case NodeType::VarDecl:
            parse_var_decl(scope, child, offset, prefix);
            break;
          case NodeType::StructuredBinding:
            parse_structured_binding(scope, child);
            break;
          case NodeType::TemplateDecl:
            parse_template_decl(scope, child);
            break;
          case NodeType::TemplateInst:
            parse_template_inst(scope, child, prefix);
            break;
          case NodeType::TemplateSpec:
            parse_template_spec(scope, child, prefix);
            break;
          case NodeType::SwitchStmt:
            /* Special case for switch statements which are 3 level deep. */
            for (SwitchCase stmt : child.children_of_type<SwitchCase>()) {
              for (LocalScope local : stmt.children_of_type<LocalScope>()) {
                parse_local_scope(scope, local, prefix);
              }
            }
            break;
          case NodeType::ForLoop:
          case NodeType::DoWhileLoop:
          case NodeType::WhileLoop:
          case NodeType::IfStmt:
          case NodeType::ElseIfStmt:
          case NodeType::ElseStmt:
          case NodeType::ReturnStmt:
            /* Convert local scopes that are nested one level deeper (eg.  if, etc...). */
            for (LocalScope local : child.children_of_type<LocalScope>()) {
              parse_local_scope(scope, local, prefix);
            }
            break;
          default:
            break;
        }
      }
    }
  }

  void parse_local_scope(SymbolScope &scope, LocalScope local_node, const std::string &prefix)
  {
    SymbolScope *sym = table.scp_arena.alloc(&scope, local_node);
    scope.scopes.emplace(sym->identifier, sym);

    parse_scope(*sym, local_node, prefix + sym->identifier + ns_sep);
  }

  /* Create declaration for each template argument so name lookup will work with these. */
  void parse_template_arguments(SymbolScope &scope, TemplateDecl decl, TemplateInst inst)
  {
    TemplateParamList params = inst.parameters();
    Expr param = params.child_first();

    assert(scope.parent != nullptr);
    /* Search for types in the parent namespace, where the instantiation resides. */
    SymbolScope &inst_scope = *scope.parent;

    for (TemplateArg arg : decl.arguments().children_range()) {
      assert(arg.is_valid());
      if (arg.front() == Typename) {
        IdQualified arg_id = arg.type();
        string id(arg_id.str());
        if (param.child_count() != 1 || param.child_first().type() != NodeType::LocalVar) {
          error(param, "Invalid argument for template parameter '" + id + "'");
          return;
        }
        IdQualified type_id(LocalVar(param.child_first()).identifier());
        SymbolClass *cls = table.cls_arena.alloc(&scope, arg.id().front(), id);
        auto [resolved, err] = inst_scope.lookup_class(type_id);
        error(err);
        cls->resolved = resolved;
        if (resolved->is_error) {
          error(param, "Unknown type in template instantiation");
        }
        scope.classes.emplace(id, cls);
      }
      else {
        IdQualified type = arg.type();
        string id(arg.id().str());
        auto [cls, err] = inst_scope.lookup_class(type);
        error(err);
        SymbolVariable *var = table.var_arena.alloc(&scope, cls, arg.id().front(), id);
        var->is_constexpr = true;
        auto [val, err_expr] = SymbolTable::evaluate_constexpr(scope, param.child_first());
        error(err_expr);
        var->value = val;
        scope.variables.emplace(id, var);
      }
      param = param.next();
    }
  }

  void parse_function_return_type(SymbolScope &scope, FuncDecl decl)
  {
    SymbolFunction *fn_sym = static_cast<SymbolFunction *>(&scope);
    auto [type, err] = fn_sym->lookup_class(decl.return_type().id());
    error(err);
    fn_sym->return_type = type;

    if (fn_sym->is_entry_point() && type->identifier != "void") {
      error(decl.return_type(), "Entry point functions must return 'void'");
    }
  }

  /* Create declaration for each function argument so name lookup will work with these.
   * Also create declaration for `this_`. */
  void parse_function_arguments(SymbolScope &scope, FuncDecl decl)
  {
    SymbolFunction *fn_sym = static_cast<SymbolFunction *>(&scope);
    const bool is_entry_point = fn_sym->is_entry_point();

    for (FuncArg arg : decl.arguments().children_of_type<FuncArg>()) {
      auto [res_packed, err_attr] = resource_type_from_attributes(arg.attributes());
      auto [res_type, srt_type] = res_packed;
      error(err_attr);

      auto [type, err] = scope.lookup_class(arg.type().id());
      error(err);
      SymbolVariable *var = table.var_arena.alloc(&scope, type, arg.declarator());
      var->type = type;
      scope.variables.emplace(var->identifier, var);
      /* Register argument type for argument resolution. */
      fn_sym->arg_types.emplace_back(type);
      fn_sym->arg_defaults.emplace_back(arg.declarator().initial_value().expr());

      auto generate_resolved_builtin = [&](const char *builtin) {
        SymbolVariable *resolved = table.var_arena.alloc(*var);
        var->resolved = resolved;
        resolved->identifier = builtin;
      };
      switch (res_type) {
        case ResourceType::BASE_INSTANCE:
          generate_resolved_builtin("gpu_BaseInstance");
          break;
        case ResourceType::FRONT_FACING:
          generate_resolved_builtin("gl_FrontFacing");
          break;
        case ResourceType::INSTANCE_ID:
          generate_resolved_builtin("gl_InstanceID");
          break;
        case ResourceType::POINT_SIZE:
          generate_resolved_builtin("gl_PointSize");
          break;
        case ResourceType::CLIP_DISTANCE:
          generate_resolved_builtin("gl_ClipDistance");
          break;
        case ResourceType::LAYER:
          generate_resolved_builtin("gl_Layer");
          break;
        case ResourceType::INSTANCE_INDEX:
          generate_resolved_builtin("gpu_InstanceIndex");
          break;
        case ResourceType::VIEWPORT_INDEX:
          generate_resolved_builtin("gpu_ViewportIndex");
          break;
        case ResourceType::VERTEX_ID:
          generate_resolved_builtin("gl_VertexID");
          break;
        case ResourceType::POSITION:
          generate_resolved_builtin("gl_Position");
          break;
        case ResourceType::FRAG_DEPTH:
          generate_resolved_builtin("gl_FragDepth");
          break;
        case ResourceType::FRAG_COORD:
          generate_resolved_builtin("gl_FragCoord");
          break;
        case ResourceType::FRAG_STENCIL_REF:
          generate_resolved_builtin("gl_FragStencilRefARB");
          break;
        case ResourceType::POINT_COORD:
          generate_resolved_builtin("gl_PointCoord");
          break;
        case ResourceType::LOCAL_INVOCATION_ID:
          generate_resolved_builtin("gl_GlobalInvocationID");
          break;
        case ResourceType::LOCAL_INVOCATION_INDEX:
          generate_resolved_builtin("gl_LocalInvocationIndex");
          break;
        case ResourceType::GLOBAL_INVOCATION_ID:
          generate_resolved_builtin("gl_GlobalInvocationID");
          break;
        case ResourceType::WORK_GROUP_ID:
          generate_resolved_builtin("gl_WorkGroupID");
          break;
        case ResourceType::NUM_WORK_GROUP:
          generate_resolved_builtin("gl_NumWorkGroups");
          break;
          /* TODO  */
        // case ResourceType::IN:
        //   generate_resolved_builtin("");
        //   break;
        // case ResourceType::OUT:
        //   generate_resolved_builtin("");
        //   break;
        default:
          break;
      }

      if (is_entry_point && srt_type != ResourceTableType::ENTRY_POINT &&
          res_type != ResourceType::RESOURCE_TABLE)
      {
        error(arg, "Expected entry point resource attribute but got '" + to_str(srt_type) + "'");
      }
      else if (!is_entry_point && srt_type != ResourceTableType::NONE &&
               res_type != ResourceType::RESOURCE_TABLE)
      {
        error(arg, "Resource attributes are only supported on entry point arguments");
      }
    }

    if (scope.parent != nullptr) {
      /* Add `this_` to the var declarations. */
      auto &fns = scope.parent->functions;
      if (auto it = fns.find(scope.identifier); it != fns.end()) {
        SymbolFunction *fn = it->second;
        if (fn->fn_type == SymbolFunction::MEMBER) {
          SymbolVariable *var = table.var_arena.alloc(
              &scope, fn->parent_class(), decl.front(), "this_");
          scope.variables.emplace(var->identifier, var);
        }
      }
    }
  }

  /* Parse declarators inside for loop conditions. */
  void parse_loop_arguments(SymbolScope &scope, ForLoop loop)
  {
    for (VarDecl var : loop.condition().descendants_of_type<VarDecl>()) {
      auto [type, err] = scope.lookup_class(var.type().id());
      error(err);
      for (Declarator decl : var.children_of_type<Declarator>()) {
        SymbolVariable sym(&scope, type, decl);
        sym.type = type;
        scope.variables.emplace(sym.identifier, table.var_arena.alloc(sym));
      }
    }
  }

  void parse_preprocessor(SymbolScope &scope, Preprocessor directive)
  {
    /* Keep track of all defines with value to make sure to not hit an undefined symbol error. */
    Token type = directive.front().next();
    if (type.str() == "define") {
      Token name = type.next();
      if (name == directive.back()) {
        /* Doesn't define anything. */
        return;
      }
      Token value = name.next();
      bool is_func = false;
      if (value == '(' && name.str_with_whitespace().back() != ' ') {
        /* This is a functional macro. */
        is_func = true;
        value = value.next(')');
        if (value == directive.back()) {
          /* Doesn't define anything. */
          return;
        }
        value = value.next();
      }

      SymbolScope *root = scope.root_scope();
      /* Macro is not empty and defines something. Assume it is a int constant. */
      SymbolClass *int_cls = root->lookup_class("int");
      if (is_func) {
        SymbolFunction *fn = table.fun_arena.alloc(
            root, name, string(name.str()), SymbolFunction::GLOBAL);
        fn->return_type = int_cls;
        root->functions.try_emplace(fn->identifier, fn);
      }
      else {
        SymbolVariable *var = table.var_arena.alloc(root, int_cls, name, string(name.str()));
        root->variables.try_emplace(var->identifier, var);
      }
      /* TODO: Make a warning/error if this just defines a constant in global space without #if
       * directives. */
    }
  }

  void parse_namespace(SymbolScope &scope, Namespace decl, string ns_prefix)
  {
    IdQualified ns = decl.identifier();
    /* Walk nested identifier */
    SymbolScope *ns_scope = &scope;
    for (Id id : ns.children_of_type<Id>()) {
      SymbolScope *sym = table.scp_arena.alloc(ns_scope, id, SymbolScope::NAMESPACE);
      ns_scope->scopes.emplace(sym->identifier, sym);
      ns_scope = sym;
      ns_prefix = ns_prefix + sym->identifier + ns_sep;
    }
    parse_scope(*ns_scope, decl.body(), ns_prefix);
  }

  void parse_using_stmt(SymbolScope &scope, UsingStmt stmt)
  {
    if (stmt.is_namespace()) {
      error(stmt, "Using namespace statement are not supported");
    }
    else {
      IdQualified aliased(stmt.aliased());
      IdQualified name(stmt.id());
      auto [type, err] = scope.lookup_class(aliased);
      error(err);
      if (type->is_error) {
        error(aliased, "Unknown Type");
        return;
      }
      string identifier(name.name().str());
      if (type) {
        scope.classes.emplace(identifier, type);
        return;
      }
    }
  }

  void parse_enum_value(SymbolScope &scope, EnumValue val, ClassDecl cls, const string &prefix)
  {
    auto [enum_cls, err_enum] = scope.lookup_class(cls.identifier());
    error(err_enum);
    auto [type, err_type] = scope.lookup_class(cls.parent_class());
    error(err_type);
    SymbolVariable *sym = table.var_arena.alloc(&scope, type, val);
    sym->is_static = true;
    sym->is_constexpr = true;
    /* Resolve its value. */
    if (AssignStmt assign = val.value(); assign.is_valid()) {
      if (InitializerList list = assign.initializer_list(); list.is_valid()) {
        sym->value = eval_scalar_initializer_list(scope, list);
      }
      auto [val, err] = SymbolTable::evaluate_constexpr(scope, assign.expr().child_first());
      error(err);
      sym->value = val;
    }
    else {
      sym->value = enum_cls->enum_last_val + 1;
    }
    scope.variables.emplace(sym->identifier, sym);
    /* Save value for the next member. */
    enum_cls->enum_last_val = sym->value;

    /* Instantiate in global resolved namespace. */
    {
      SymbolVariable *flat_sym = table.var_arena.alloc(*sym);
      sym->resolved = flat_sym;
      flat_sym->resolved = sym;
      flat_sym->identifier = prefix + sym->identifier;
      global.variables.try_emplace(flat_sym->identifier, flat_sym);
    }

    if (!cls.is_enum_class()) {
      /* Alias to the parent namespace for non-anonymous, non-class enum. */
      scope.parent->variables.emplace(sym->identifier, sym);
    }
  }

  SymbolClass *parse_class_decl(SymbolScope &scope,
                                ClassDecl decl,
                                ClassDecl parent_class,
                                int &offset,
                                const std::string &prefix,
                                const std::string &suffix = "",
                                TemplateInst temp = {},
                                SymbolClassTemplate *cls_template = nullptr)
  {
    SymbolClass *cls = table.cls_arena.alloc(&scope, decl, suffix);
    scope.classes.emplace(cls->identifier, cls);
    scope.scopes.emplace(cls->identifier, cls);

    if (cls->is_anonymous && anonymous_scope_prefix(cls).non_anonymous_parent == nullptr) {
      error(decl, "Anonymous unions at namespace or global scope are not supported");
      return scope.root_scope()->lookup_class(SymbolTable::err_symbol);
    }

    if (cls_template) {
      /* Add the template definition early so it can be queried during instantiation. */
      cls_template->instances.emplace(suffix, cls);
      /* Create an alias to the unspecified class name (no template arg): e.g. `A` -> `A<T>`. */
      string alias_name = cls->identifier.substr(0, cls->identifier.size() - suffix.size());
      cls->classes.emplace(alias_name, cls);
    }

    /* Instantiate in global resolved namespace. */
    SymbolClass *flat_cls = table.cls_arena.alloc(*cls);
    cls->resolved = flat_cls;
    flat_cls->resolved = cls;
    flat_cls->identifier = prefix + cls->identifier;
    global.classes.try_emplace(flat_cls->identifier, flat_cls);

    parse_scope(*cls, decl.body(), prefix + cls->identifier + ns_sep, temp);

    if (cls->is_enum) {
      auto [underlying_type, err] = scope.lookup_class(decl.parent_class());
      error(err);
      cls->size = underlying_type->size;
      cls->align = underlying_type->align;
      cls->is_std140_compatible = cls->size == 4;
      cls->is_std430_compatible = cls->size == 4;
    }
    else {
      cls->ensure_size_and_align();
    }

    if (cls->is_anonymous) {
      /* Instantiate into parent class as member. */
      SymbolVariable *var = table.var_arena.alloc(
          &scope, cls, decl.front(), cls->identifier + ns_sep);
      scope.variables.emplace(cls->identifier, var);

      var->type = cls;
      var->set_offset(parent_class.is_union(), offset);
    }
    return cls;
  }

  SymbolFunction *parse_func_decl(SymbolScope &scope,
                                  FuncDecl func,
                                  const std::string &prefix,
                                  const std::string &suffix = "",
                                  TemplateInst temp = {})
  {
    SymbolFunction *fn = table.fun_arena.alloc(&scope, func, suffix);
    bool is_overload = scope.function_emplace(fn);
    if (is_overload && fn->is_entry_point()) {
      error(func.identifier(),
            "Redefinition of entry point function '" + string(func.identifier().str()) + "'");
    }

    parse_scope(*fn, func.body(), prefix + fn->identifier + ns_sep, temp);

    {
      /* Instantiate in global resolved namespace. */
      string identifier_mangled;
      switch (fn->fn_type) {
        case SymbolFunction::GLOBAL:
        case SymbolFunction::STATIC:
          identifier_mangled = prefix + fn->identifier;
          break;
        case SymbolFunction::MEMBER:
          /* Member function use overload resolution. */
          identifier_mangled = "_" + fn->identifier;
          break;
      }
      SymbolFunction *flat_func = table.fun_arena.alloc(*fn);
      fn->resolved = flat_func;
      flat_func->resolved = fn;
      flat_func->identifier = identifier_mangled;
      global.functions.try_emplace(identifier_mangled, flat_func);
    }

    return fn;
  }

  void parse_template_decl(SymbolScope &scope, TemplateDecl decl)
  {
    /* Do not parse. Only keep the symbol definition. The instantiation will do the parsing. */
    if (decl.is_function()) {
      SymbolFunctionTemplate *temp = table.tmp_fun_arena.alloc(decl);
      SymbolFunction *fn = table.fun_arena.alloc(&scope, decl.decl());
      fn->template_data = temp;
      scope.functions.emplace(fn->identifier, fn);
    }
    else {
      SymbolClassTemplate *temp = table.tmp_cls_arena.alloc(decl);
      SymbolClass *cls = table.cls_arena.alloc(&scope, decl.decl());
      cls->template_data = temp;
      scope.classes.emplace(cls->identifier, cls);
    }
  }

  template<typename DeclAst, typename SymbolT>
  TemplateDecl check_template_instance(TemplateParamList temp_params,
                                       DeclAst &decl,
                                       const SymbolT &sym)
  {
    if (sym.is_error) {
      error(decl.identifier(), "Missing template definition");
      return {};
    }
    if (sym.template_data == nullptr) {
      error(decl.identifier(), "Function is not a template");
      return {};
    }
    TemplateDecl temp_decl = sym.template_data->decl;
    if (temp_decl.arguments().child_count() != temp_params.child_count()) {
      error(temp_params, "Template parameter count must match declaration argument count");
      return {};
    }
    return temp_decl;
  }

  void parse_template_inst(SymbolScope &scope, TemplateInst temp, const std::string &prefix)
  {
    TemplateParamList tmp_params = temp.parameters();

    if (temp.is_function()) {
      const FuncForwardDecl decl = temp.decl();
      const IdQualified id = decl.identifier();
      SymbolFunction *fn = scope.lookup_function_base(id);

      if (TemplateDecl temp_decl = check_template_instance(tmp_params, decl, *fn);
          temp_decl.is_valid())
      {
        auto [mangled,
              err] = SymbolTable::mangle_identifier(temp_decl.arguments(), tmp_params, scope);
        error(err);

        SymbolScope *parent = &scope;
        if (fn->fn_type == SymbolFunction::MEMBER) {
          parent = nullptr;
          Id start = id.namespace_start();
          Id last = id.child_last(NodeType::Id).prev(NodeType::Id);
          if (auto [parent_cls, err_cls] = scope.lookup_class(start, last); parent_cls) {
            error(err_cls);
            if (parent_cls->template_data && !last.template_params().is_valid()) {
              error(last, "Missing template parameters");
            }
            parent = parent_cls;
          }

          if (parent == nullptr) {
            error(id,
                  "Missing instantiation of parent class for instantiation of '" +
                      string(id.str()) + "'");
            return;
          }
        }

        string full_id = string(decl.identifier().str()) + "<" + mangled.str_debug.substr(2) + ">";
        NOTE(decl.identifier(),
             "In instantiation of function template specialization '" + full_id +
                 "' requested here");
        SymbolFunction *fn_inst = parse_func_decl(
            *parent, temp_decl.decl(), prefix, mangled.str, temp);
        fn->template_data->instances.emplace(mangled.str, fn_inst);
      }
    }
    else {
      const ClassDecl decl(temp.decl());
      auto *cls = scope.lookup_class_base(decl.identifier());

      if (TemplateDecl temp_decl = check_template_instance(tmp_params, decl, *cls);
          temp_decl.is_valid())
      {
        auto [mangled,
              err] = SymbolTable::mangle_identifier(temp_decl.arguments(), tmp_params, scope);
        error(err);
        string full_id = string(decl.identifier().str()) + "<" + mangled.str_debug.substr(2) + ">";
        NOTE(decl.identifier(),
             "In instantiation of class template specialization '" + full_id + "' requested here");
        int unused_offset = 0;
        parse_class_decl(scope,
                         temp_decl.decl(),
                         Node{},
                         unused_offset,
                         prefix,
                         mangled.str,
                         temp,
                         cls->template_data);
      }
    }
  }

  void parse_template_spec(SymbolScope &scope, TemplateSpec temp, const std::string &prefix)
  {
    TemplateParamList temp_params = temp.parameters();
    /* Instantiate the whole symbol into the current namespace. */
    if (temp.is_function()) {
      const FuncDecl decl(temp.decl());
      auto *fn = scope.lookup_function_base(decl.identifier());

      if (TemplateDecl temp_decl = check_template_instance(temp_params, decl, *fn);
          temp_decl.is_valid())
      {
        auto [mangled,
              err] = SymbolTable::mangle_identifier(temp_decl.arguments(), temp_params, scope);
        error(err);
        string full_id = string(decl.identifier().str()) + "<" + mangled.str_debug.substr(2) + ">";
        NOTE(decl.identifier(),
             "In instantiation of function template specialization '" + full_id +
                 "' requested here");
        SymbolFunction *spec = parse_func_decl(scope, decl, prefix, mangled.str);
        fn->template_data->instances.emplace(mangled.str, spec);
      }
    }
    else {
      const ClassDecl decl(temp.decl());
      auto *cls = scope.lookup_class_base(decl.identifier());

      if (TemplateDecl temp_decl = check_template_instance(temp_params, decl, *cls);
          temp_decl.is_valid())
      {
        auto [mangled,
              err] = SymbolTable::mangle_identifier(temp_decl.arguments(), temp_params, scope);
        error(err);
        string full_id = string(decl.identifier().str()) + "<" + mangled.str_debug.substr(2) + ">";
        NOTE(decl.identifier(),
             "In instantiation of class template specialization '" + full_id + "' requested here");
        int unused_offset = 0;
        SymbolClass *spec = parse_class_decl(
            scope, decl, Node{}, unused_offset, prefix, mangled.str);
        cls->template_data->instances.emplace(mangled.str, spec);
      }
    }
  }

  void parse_structured_binding(SymbolScope &scope, StructuredBinding decl)
  {
    AssignStmt assign = decl.assign();
    Expr expr = assign.expr();
    if (!expr.is_valid()) {
      error(decl, "Cannot deduce actual type for variable '' with type 'auto'");
      return;
    }

    auto [cls, err] = table.expr_type_analysis(scope, expr);
    error(err);
    if (cls->is_error) {
      error(decl, "Cannot deduce actual type for variable '' with type 'auto'");
      return;
    }

    Id id = decl.child_first();

    /* Create temp variable to write to. */
    SymbolVariable *tmp = table.var_arena.alloc(&scope, cls, decl.front(), decl.tmp_id());
    scope.variables.emplace(tmp->identifier, tmp);

    for (SymbolVariable *var : cls->non_static_variables_in_declaration_order()) {
      if (!id.is_valid()) {
        error(decl, "Not enough names in structure binding");
        return;
      }
      /* Make copy of the variable in local scope. */
      SymbolVariable *sym = table.var_arena.alloc(*var);
      sym->identifier = id.str();
      sym->parent = &scope;
      scope.variables.emplace(sym->identifier, sym);
      /* Resolve to the tmp variable member. */
      SymbolVariable *res = table.var_arena.alloc(*var);
      res->identifier = tmp->identifier + '.' + var->identifier;
      res->parent = &scope;
      sym->resolved = res;

      id = id.next();
    }
    if (id.is_valid()) {
      error(decl, "Too many names in structure binding");
      return;
    }
  }

  void parse_var_decl(SymbolScope &scope, VarDecl var, int &offset, const std::string &prefix)
  {
    IdQualified type_id = var.type().id();
    string_view type_id_str = type_id.str();

    /* Assumes C++ compilation already checked that all declarator uses the same type.
     * Deduce type using only the first declarator. */
    Declarator first_decl = var.child_first(NodeType::Declarator);

    auto [type, err] = (type_id_str == "auto") ? table.resolve_auto_type(scope, first_decl) :
                                                 scope.lookup_class(type_id);
    error(err);

    if (type->is_error) {
      error(var, "Unknown type name '" + string(type_id_str) + "'");
      return;
    }

    SymbolClass *cls = scope.as_class();

    auto [res_packed, err_attr] = resource_type_from_attributes(var.attributes());
    auto [res_type, srt_type] = res_packed;
    error(err_attr);

    if (res_type == ResourceType::UNIFORM_BUF && !type->is_std140_compatible) {
      NOTE(type_id, "In this uniform buffer declaration");
      type->ensure_size_and_align(this);
    }
    else if (res_type == ResourceType::STORAGE_BUF && !type->is_std430_compatible) {
      NOTE(type_id, "In this storage buffer declaration");
      type->ensure_size_and_align(this, true);
    }

    if (cls) {
      if (cls->srt_type == ResourceTableType::NONE && srt_type != ResourceTableType::NONE) {
        if (cls->is_anonymous) {
          error(var, "Resources cannot be declared in an anonymous struct");
        }
        if (srt_type != ResourceTableType::VERTEX_IN &&
            srt_type != ResourceTableType::VERTEX_OUT &&
            srt_type != ResourceTableType::FRAGMENT_OUT &&
            srt_type != ResourceTableType::RESOURCE_TABLE)
        {
          error(var,
                "Invalid resource of type '" + to_str(cls->srt_type) +
                    "' for resource table class");
        }
        cls->srt_type = srt_type;
      }
      else if (cls->srt_type != srt_type) {
        error(var,
              "Expected resource definition of type '" + to_str(cls->srt_type) + "' but got '" +
                  to_str(srt_type) + "'");
      }
    }
    else if (res_type == ResourceType::RESOURCE_TABLE) {
      /* Currently supported as noop for legacy resource table reference getter. */
    }
    else if (res_type != ResourceType::NONE) {
      error(var, "Out of class resource declaration");
    }

    const bool is_srt_local_ref = type->is_srt() && cls == nullptr;

    string anon_prefix;
    SymbolScope *named_parent = cls;
    if (cls && cls->is_anonymous) {
      auto prefix = anonymous_scope_prefix(cls);
      anon_prefix = prefix.prefix;
      named_parent = prefix.non_anonymous_parent;
    }

    const bool is_ref = var.is_reference();
    for (Declarator decl : var.children_of_type<Declarator>()) {
      if (is_ref != decl.is_reference()) {
        error(decl, "Variable declaration must declare either no reference or only reference");
        continue;
      }

      SymbolVariable *sym = table.var_arena.alloc(&scope, type, decl);
      sym->res_type = res_type;
      sym->is_static |= res_type != ResourceType::NONE;
      scope.variables.emplace(sym->identifier, sym);

      if (is_srt_local_ref && !is_ref) {
        error(decl, "Resource table must be declared as reference");
      }

      /* Local References. */
      if (is_ref && cls == nullptr) {
        AssignStmt stmt = decl.initial_value();
        if (stmt.is_valid() && stmt.expr().is_valid()) {
          /* Run analysis to check if we bind a temporary. */
          /* Run const analysis to check if we bind a const expr. */
          ExpressionTypeParser parser(&table, &scope, stmt.expr().child_first());
          auto [expr_cls, err] = parser.eval();
          error(err);
          if (parser.first_temporary.is_valid()) {
            error(parser.first_temporary, "Reference cannot bind to a temporary");
          }
          else if (parser.first_side_effect_op.is_valid()) {
            error(parser.first_side_effect_op,
                  "Reference cannot bind to an expression with side effect");
          }
          else if (parser.first_function_call.is_valid()) {
            error(parser.first_function_call,
                  "Function calls are not allowed in reference definition");
          }
          else if (parser.first_non_const_var_in_subscript.is_valid()) {
            error(parser.first_non_const_var_in_subscript,
                  "Reference cannot bind to non-const subscript because '" +
                      string(parser.first_non_const_var_in_subscript.str()) +
                      "' is not defined as 'const'");
          }
          else if (type->resolved != expr_cls->resolved) {
            error(decl,
                  "Reference to type '" + type->resolved->identifier +
                      "' cannot bind to a value of unrelated type '" + expr_cls->identifier + "'");
          }

          sym->reference_value = stmt.expr();
        }
        else {
          error(decl,
                "Declaration of reference variable '" + string(decl.identifier().str()) +
                    "' requires an initializer");
        }
      }

      if (cls) {
        sym->set_offset(cls->is_union, offset);

        /* Alias to the parent class for anonymous structs. */
        if (cls->is_anonymous || cls->is_union) {
          /* Append accessor function. */
          string suffix = cls->is_union ? "()" : "";
          /* Create a resolved symbol. */
          SymbolVariable *resolved = table.var_arena.alloc(&scope, type, decl);
          resolved->identifier = anon_prefix + resolved->identifier + suffix;
          sym->resolved = resolved;
          /* Alias to the first, non-anonymous parent. */
          named_parent->variables.emplace(sym->identifier, sym);
        }
      }

      if (sym->is_static) {
        /* Instantiate in global resolved namespace. */
        SymbolVariable *flat_var = table.var_arena.alloc(*sym);
        sym->resolved = flat_var;
        flat_var->resolved = sym;
        if (res_type != ResourceType::VERT_ATTR_OUT && res_type != ResourceType::FRAG_OUT) {
          /* Some resource are declared in global namespace without prefix. */
          flat_var->identifier = sym->identifier;
        }
        else {
          flat_var->identifier = prefix + sym->identifier;
        }
        global.variables.try_emplace(flat_var->identifier, flat_var);
      }

      if (sym->is_constexpr) {
        if (sym->is_static) {
          sym->value = initialize_constexpr(scope, sym->type, decl);
          sym->resolved->value = sym->value;
        }
        else {
          error(var.type(), "Constexpr declaration must also be static");
        }
      }
    }
  }

  int64_t eval_scalar_initializer_list(const SymbolScope &scope, InitializerList list)
  {
    if (list.is_empty()) {
      return 0;
    }
    if (list.child_count() != 1) {
      error(list, "Excess elements in scalar initializer");
      return 0;
    }
    if (Initializer init = list.child_first(); init.is_valid()) {
      Node node = init.child_first();
      if (node.type() == NodeType::InitializerList) {
        return eval_scalar_initializer_list(scope, node);
      }
      auto [val, err] = SymbolTable::evaluate_constexpr(scope, node.child_first());
      error(err);
      return val;
    }
    return 0;
  }

  int64_t initialize_constexpr(const SymbolScope &scope, SymbolClass *cls, Declarator decl)
  {
    if (cls->identifier != "int" && cls->identifier != "uint") {
      error(decl, "Constexpr variable must be of type int or uint");
      return 0;
    }
    if (AssignStmt assign = decl.initial_value(); assign.is_valid()) {
      if (InitializerList list = assign.initializer_list(); list.is_valid()) {
        return eval_scalar_initializer_list(scope, list);
      }
      auto [val, err] = SymbolTable::evaluate_constexpr(scope, assign.expr().child_first());
      error(err);
      return val;
    }
    if (InitializerList list = decl.initializer_list(); list.is_valid()) {
      return eval_scalar_initializer_list(scope, list);
    }
    error(decl,
          "Constexpr variable '" + string(decl.identifier().str()) +
              "' must be initialized by a constant expression");
    return 0;
  }

  struct AnonScopePrefix {
    string prefix;
    SymbolScope *non_anonymous_parent;
  };

  static AnonScopePrefix anonymous_scope_prefix(SymbolClass *cls)
  {
    string prefix;
    SymbolClass *parent = nullptr;
    /* Support multiple nesting level. */
    do {
      /* Union members need to go through the getter functions. */
      parent = cls->parent->as_class();
      string access = parent && parent->is_union && parent->is_anonymous ? "()" : "";
      prefix = cls->identifier + "_" + access + "." + prefix;
    } while ((cls = parent, cls && cls->is_anonymous));

    return {prefix, cls};
  }
};  // namespace blender::gpu::shader::parser

SymbolVariable::SymbolVariable(SymbolScope *parent, SymbolClass *type, Declarator decl)
    : Symbol(string(decl.identifier().str()), decl.front(), parent),
      type(type),
      array_dimensions(decl.array().dimensions()),
      is_static(decl.type().is_static()),
      is_compilation_const(AttrList(decl.type().prev()).contains_attr("compilation_constant"))
{
  if (Const constant = decl.type().constant(); constant.is_valid()) {
    is_constexpr = constant.front() == Constexpr;
    is_const = true;
  }
  if (ArrayDecl array = decl.array(); array.is_valid()) {
    Subscript sub = array.sub();
    array_elements = 1;
    for (int i = 0; i < array_dimensions; ++i, sub = sub.next()) {
      Node node = sub.expr().child_first();
      if (!node.is_valid()) {
        array_elements = -1;
        break;
      }
      auto [value, err] = SymbolTable::evaluate_constexpr(*parent, node);
      if (err) {
        array_elements = -1;
        break;
      }
      array_elements *= value;
    }
  }
}

void SymbolVariable::set_offset(bool is_union, int &offset)
{
  if (is_union) {
    this->offset = 0;
  }
  else {
    this->offset = pad(offset, this->type->align);
    offset = this->offset;
    offset += this->type->size;
  }
}

Result<string> SymbolTable::expr_to_string(const SymbolScope &scope,
                                           Node start,
                                           int &node_count,
                                           bool allow_compilation_constant)
{
  const char *expr_type = allow_compilation_constant ? "static branch condition" :
                                                       "constant expression";
  std::optional<AstNodeException> error;
  string expr_str;
  for (Node child = start; child.is_valid(); child = child.next()) {
    switch (child.type()) {
      case NodeType::LocalVar: {
        SymbolVariable *var = scope.lookup_variable(LocalVar(child).identifier());
        /* Resolve member access. */

        for (Node next = child.next(); next.type() == NodeType::Op && next.front() == '.';
             next = child.next())
        {
          child = next.next();

          if (child.type() == NodeType::LocalVar) {
            var = var->type->lookup_variable(LocalVar(child).identifier());
          }
          else if (child.type() == NodeType::FuncCall) {
            return {"0",
                    AstNodeException(child, string(expr_type) + " cannot contain function calls")};
          }
          else {
            return {"0",
                    AstNodeException(child,
                                     "Unsupported symbol '" + string(child.str()) + "' inside " +
                                         string(expr_type))};
          }
        }

        if (var->is_error) {
          return {"0", AstNodeException(child, "Unknown variable '" + string(child.str()) + "'")};
        }

        if (allow_compilation_constant && var->is_compilation_const) {
          /* Compilation constants are in global namespace. */
          expr_str += "SRT_CONSTANT_" + var->identifier;
        }
        else if (!var->is_constexpr) {
          return {"0",
                  AstNodeException(child,
                                   "Read of non-const variable is not allowed in a " +
                                       string(expr_type))};
        }
        else {
          expr_str += to_string(var->value);
        }
        node_count++;
        break;
      }
      case NodeType::FuncCall:
        return {"0",
                AstNodeException(child, string(expr_type) + " cannot contain function calls")};
      case NodeType::ExprSub: {
        auto [str, err] = expr_to_string(scope, ExprSub(child).expr().child_first(), node_count);
        expr_str += '(';
        expr_str += str;
        expr_str += ')';
        node_count++;
        if (err) {
          return {"0", err};
        }
        break;
      }
      case NodeType::Op:
      case NodeType::NumConst:
        expr_str += child.str();
        node_count++;
        break;
      default:
        return {"0",
                AstNodeException(child,
                                 "Unsupported symbol '" + string(child.str()) + "' inside " +
                                     string(expr_type))};
    }
  }
  return {expr_str, error};
}

Result<int64_t> SymbolTable::evaluate_constexpr(const SymbolScope &scope, Node start)
{
  /* First build a string containing only numerical constants by substituting all the symbols. */
  int count = 0;
  auto [expr_str, error] = expr_to_string(scope, start, count);
  if (error) {
    return {0, error};
  }

  /* Fast path. */
  if (count == 1) {
    try {
      return {stoll(expr_str), {}};
    }
    catch (...) {
      return {0, AstNodeException(start, "Error parsing integer literal")};
    }
  }

  /* Then use the expression parser to evaluate it. */
  try {
    ExpressionParser expression_parser;
    expression_parser.lexical_analysis(expr_str);
    return {expression_parser.eval(), {}};
  }
  catch (const std::exception &e) {
    return {0,
            AstNodeException(start, "Failed to evaluate expanded expression '" + expr_str + "'")};
  }
}

Result<vector<SymbolClass *>> SymbolFunction::to_arg_types(const SymbolTable &table,
                                                           const SymbolScope &scope,
                                                           FuncParamList list)
{
  std::optional<AstNodeException> error;
  vector<SymbolClass *> arg_types;
  for (Expr expr : list.children_of_type<Expr>()) {
    auto [cls, err] = table.expr_type_analysis(scope, expr);
    arg_types.emplace_back(cls);
    if (err && !error) {
      error = err;
    }
  }
  return {arg_types, error};
}

Result<vector<SymbolClass *>> SymbolFunction::to_arg_types(const SymbolTable & /*table*/,
                                                           const SymbolScope &scope,
                                                           FuncArgList list)
{
  vector<SymbolClass *> arg_types;
  for (FuncArg arg : list.children_of_type<FuncArg>()) {
    auto [cls, _] = scope.lookup_class(arg.type().id());
    arg_types.emplace_back(cls);
  }
  return {arg_types, std::nullopt};
}

SymbolFunction *SymbolFunction::lookup_overload(const vector<SymbolClass *> &arg_types)
{
  for (SymbolFunction *fn = this; fn; fn = fn->overload_next) {
    if (fn->argument_matches(arg_types)) {
      return fn;
    }
  }
  return root_scope()->lookup_function(SymbolTable::err_symbol);
}

bool SymbolFunction::argument_matches(const vector<SymbolClass *> &arg_types) const
{
  /* Use GLSL strict matching for now, no implicit conversion. */
  return this->arg_types == arg_types;
}

template<typename T, typename AstNodeT>
static Result<T *> resolve_template_instantiation(T *sym, AstNodeT id, const SymbolScope &scope)
{
  if (TemplateParamList param = id.template_params();
      param.is_valid() && sym && sym->template_data)
  {
    auto [inst, _, err] = sym->template_data->lookup_inst(param, scope);
    return {inst, err};
  }
  return {sym, std::nullopt};
}

#if 0 /* For debugging. */
#  define LOOKUP_LOG(a) cerr << a;
#else
#  define LOOKUP_LOG(a)
#endif

Result<SymbolFunction *> SymbolScope::lookup_function(IdQualified id) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  SymbolFunction *func = lookup_generic<SymbolFunction>(id, id.front());
  return resolve_template_instantiation(func, id, *this);
}

template<typename ArgOrParamList>
Result<SymbolFunction *> SymbolScope::lookup_function(IdQualified id,
                                                      ArgOrParamList params,
                                                      const SymbolTable &table) const
{
  auto [func, err] = lookup_function(id);
  if (func->template_data && !id.template_params().is_valid()) {
    auto [func_, err_adl] = func->template_data->lookup_adl(table, params, *this);
    err = err ? err : err_adl;
    func = func_;
  }
  else if (func->overload_next) {
    auto [arg_types, err_args] = SymbolFunction::to_arg_types(table, *this, params);
    err = err ? err : err_args;
    SymbolFunction *overload = func->lookup_overload(arg_types);
    if (overload->is_error) {
      string parameters = arg_types.empty() ? ", " : "";
      for (SymbolClass *cls : arg_types) {
        parameters += ", '" + cls->identifier + "'";
      }
      if (!err) {
        err = {id,
               "No matching function for call to '" + func->identifier + "' for parameters " +
                   parameters.substr(2)};
      }
    }
    func = overload;
  }
  else if (func->is_error) {
    /* Try to resolve type constructors (only for builtins for now). */
    if (auto [cls, err_cls] = lookup_class(id);
        !cls->is_error && cls->resolved && cls->resolved->is_builtin)
    {
      func = root_scope()->lookup_function(cls->resolved->identifier);
      err = {};
    }
    else {
      err = {id, "Unknown function name"};
    }
  }
  return {func, err};
}

template Result<SymbolFunction *> SymbolScope::lookup_function(IdQualified id,
                                                               FuncParamList params,
                                                               const SymbolTable &table) const;
template Result<SymbolFunction *> SymbolScope::lookup_function(IdQualified id,
                                                               FuncArgList params,
                                                               const SymbolTable &table) const;

Result<SymbolClass *> SymbolScope::lookup_class(IdQualified id) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  SymbolClass *cls = lookup_generic<SymbolClass>(id, id.front());
  return resolve_template_instantiation(cls, id, *this);
}

Result<SymbolClass *> SymbolScope::lookup_class(Id id, Id last) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  SymbolClass *cls = lookup_generic_nested<SymbolClass>(id, last, id.front());
  return resolve_template_instantiation(cls, last, *this);
}

SymbolFunction *SymbolScope::lookup_function_base(IdQualified id) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  return lookup_generic<SymbolFunction>(id, id.front());
}

SymbolClass *SymbolScope::lookup_class_base(IdQualified id) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  return lookup_generic<SymbolClass>(id, id.front());
}

SymbolVariable *SymbolScope::lookup_variable(IdQualified id) const
{
#ifdef DEBUG_SYMBOL_LOOKUP
  LOOKUP_LOG("\n")
#endif
  return lookup_generic<SymbolVariable>(id, id.front());
}

const SymbolScope *SymbolScope::root_scope() const
{
  const SymbolScope *scope = this;
  while (scope->parent) {
    scope = scope->parent;
  }
  return scope;
}

SymbolScope *SymbolScope::root_scope()
{
  SymbolScope *scope = this;
  while (scope->parent) {
    scope = scope->parent;
  }
  return scope;
}

/* Helper for debug print. */
template<typename T> const char *get_symbol_type_name()
{
  if constexpr (std::is_same_v<T, SymbolFunction>) {
    return "Function";
  }
  else if constexpr (std::is_same_v<T, SymbolClass>) {
    return "Class";
  }
  else if constexpr (std::is_same_v<T, SymbolVariable>) {
    return "Variable";
  }
  else {
    return "Unknown";
  }
}

template<typename T>
T *SymbolScope::lookup_generic(IdQualified id, const SourceLocation &loc) const
{
  assert(id.is_valid());

  LOOKUP_LOG("[Lookup] Looking up " << get_symbol_type_name<T>() << " '" << id.str()
                                    << "' in scope '" << identifier << "'\n")

  /* Bubble up immediately if it's a global lookup. */
  if (id.is_global() && parent) {
    LOOKUP_LOG("[Lookup] Global lookup: Bubbling up to parent scope " << parent->identifier
                                                                      << "\n")
    return parent->lookup_generic<T>(id, loc);
  }

  /* Try to match within this scope. */
  if (auto v = lookup_generic_nested<T>(id.namespace_start(), id.child_last(NodeType::Id), loc)) {
    LOOKUP_LOG("[Lookup] Symbol found locally '" << identifier << "'.\n")
    return v;
  }

  /* Delegate to parent scope if not found locally. */
  if (parent) {
    LOOKUP_LOG("[Lookup] Not found locally. Delegating to parent scope\n")
    return parent->lookup_generic<T>(id, loc);
  }

  /* Lookup failure at root level, try to return error symbol. */
  LOOKUP_LOG("[Lookup] FAILED at root scope.\n")

  if constexpr (is_same_v<T, SymbolFunction>) {
    if (auto it = functions.find(SymbolTable::err_symbol); it != functions.end()) {
      return it->second;
    }
  }
  else if constexpr (is_same_v<T, SymbolClass>) {
    if (auto it = classes.find(SymbolTable::err_symbol); it != classes.end()) {
      return it->second;
    }
  }
  else if constexpr (is_same_v<T, SymbolVariable>) {
    if (auto it = variables.find(SymbolTable::err_symbol); it != variables.end()) {
      return it->second;
    }
  }
  else {
    static_assert(false);
  }
  /* Couldn't find the error symbol. Catastrophic failure. */
  assert(0);
  return nullptr;
}

template<typename T>
T *SymbolScope::lookup_generic_nested(Id id, Id last, const SourceLocation &loc) const
{
  LOOKUP_LOG("[Nested] Looking up " << get_symbol_type_name<T>() << " '"
                                    << id.front().buf_->substr(id.front(), last.front())
                                    << "' in scope '" << identifier << "'\n")

  /* Look up anonymous namespace first. */
  if (auto it = scopes.find(""); it != scopes.end()) {
    LOOKUP_LOG("[Nested] Looking up anonymous namespace scope\n")
    Id next_search_id = id.id != last.id ? id.next_id() : id;
    if (auto ret = it->second->lookup_generic_nested<T>(next_search_id, last, loc)) {
      return ret;
    }
  }

  /* Resolve target based on whether it's a namespace or the final symbol. */
  if (id.id != last.id) {
    /* Only classes can have template specifiers. */
    if (TemplateParamList list = id.template_params(); list.is_valid()) {
      LOOKUP_LOG("[Nested] Segment '" << id.str() << "' has template params. Checking classes.\n")
      if (auto it = classes.find(string(id.str())); it != classes.end()) {
        if (SymbolClass *cls = it->second->as_class(); cls && cls->template_data) {
          if (auto [tmp, _, err] = cls->template_data->lookup_inst(list, *this); !tmp->is_error) {
            LOOKUP_LOG("[Nested] Found template instance scope '" << tmp->identifier << "'.\n")
            return tmp->lookup_generic_nested<T>(id.next_id(), last, loc);
          }
        }
      }
      LOOKUP_LOG("[Nested] Template instance lookup failed for '" << id.str() << "'.\n")
      return nullptr;
    }

    if (auto it = scopes.find(string(id.str())); it != scopes.end()) {
      LOOKUP_LOG("[Nested] Match sub-scope/namespace '"
                 << id.str() << "' at " << it->second->identifier << ". Descending...\n")
      return it->second->lookup_generic_nested<T>(id.next_id(), last, loc);
    }
    LOOKUP_LOG("[Nested] Mid-segment '" << id.str() << "' could not be resolved as a scope.\n")
    return nullptr;
  }

  if constexpr (is_same_v<T, SymbolFunction>) {
    if (auto it = functions.find(string(id.str())); it != functions.end()) {
      if (it->second->loc <= loc) {
        LOOKUP_LOG("[Nested] Found function match: " << it->second->identifier << "\n")
        return it->second;
      }
      LOOKUP_LOG(
          "[Nested] Found function, but declared AFTER current source location "
          "(shadowed/invisible).\n")
    }
  }
  else if constexpr (is_same_v<T, SymbolClass>) {
    if (auto it = classes.find(string(id.str())); it != classes.end()) {
      if (it->second->loc <= loc) {
        LOOKUP_LOG("[Nested] Found class match: " << it->second->identifier << "\n")
        return it->second;
      }
      LOOKUP_LOG(
          "[Nested] Found class, but declared AFTER current source location "
          "(shadowed/invisible).\n")
    }
  }
  else if constexpr (is_same_v<T, SymbolVariable>) {
    if (auto it = variables.find(string(id.str())); it != variables.end()) {
      if (it->second->loc <= loc) {
        if (TemplateParamList list = id.template_params(); list.is_valid()) {
          LOOKUP_LOG(
              "[Nested] Found variable match, but rejected: Variables cannot be templated.\n")
          return nullptr;
        }
        LOOKUP_LOG("[Nested] Found variable match: " << it->second->identifier << "\n")
        return it->second;
      }
      LOOKUP_LOG(
          "[Nested] Found variable, but declared AFTER current source location "
          "(shadowed/invisible).\n")
    }
  }
  else {
    static_assert(false);
  }
  return nullptr;
}

SymbolClass *SymbolScope::lookup_class(string id) const
{
  return classes.find(id)->second;
}

SymbolFunction *SymbolScope::lookup_function(string id) const
{
  return functions.find(id)->second;
}

SymbolVariable *SymbolScope::lookup_variable(string id) const
{
  return variables.find(id)->second;
}

void SymbolScope::print() const
{
  enum class SymbolType { Variable, FunctionTemplate, ClassTemplate, Function, Class, Scope };
  /* Helper lambda to handle formatting and tree indentation. */
  auto print_line = [](const SourceLocation src_loc,
                       SymbolType type,
                       const string &identifier,
                       const SymbolClass *decl_type,
                       const string &identifier_mangled,
                       int depth,
                       const vector<bool> &is_last) {
    string loc = src_loc;
    if (loc.starts_with("builtin:") && identifier != "<root>") {
      return;
    }

    /* Create indentation based on tree depth. */
    int padding_size = max(0, 55 - static_cast<int>(loc.size()));
    string padding(padding_size, ' ');

    cout << loc << padding;

    /* Print the tree branch lines based on depth and ancestor sibling status. */
    for (int i = 0; i < depth - 1; ++i) {
      if (is_last[i]) {
        cout << "  "; /* Ancestor was the last sibling, leave space. */
      }
      else {
        cout << "│ "; /* Ancestor has more siblings, draw vertical line. */
      }
    }

    /* Print the branch for the current node. */
    if (depth > 0) {
      if (is_last.back()) {
        cout << "└─"; /* Last sibling. */
      }
      else {
        cout << "├─"; /* Not the last sibling. */
      }
    }

    /* Print the current node type and identifier. */
    cout << "o ";
    switch (type) {
      case SymbolType::Variable:
        cout << "Decl";
        break;
      case SymbolType::FunctionTemplate:
        cout << "TmpF";
        break;
      case SymbolType::ClassTemplate:
        cout << "TmpC";
        break;
      case SymbolType::Function:
        cout << "Func";
        break;
      case SymbolType::Class:
        cout << "Type";
        break;
      case SymbolType::Scope:
        cout << "Scop";
        break;
    }
    if (!identifier.empty()) {
      cout << " " << identifier;
    }
    if (decl_type) {
      cout << " : " << decl_type->identifier;
    }
    if (!identifier_mangled.empty()) {
      cout << " -> " << identifier_mangled;
    }
    cout << "\n";
  };

  /* Print the root node. */
  print_line(loc,
             SymbolType::Scope,
             this->identifier.empty() ? "<root>" : this->identifier,
             nullptr,
             "",
             0,
             {});

  /* Recursive lambda to traverse all symbol branches. */
  auto print_node = [&](auto &self, auto *node, int depth, const vector<bool> &is_last) -> void {
    struct Child {
      SymbolType type;
      const Symbol *sym;
      const Symbol *resolved;
      const SymbolClass *cls;
    };

    vector<Child> children;
    /* Gather children based on the current node type. */
    for (const auto &[name, var] : node->variables) {
      children.emplace_back(SymbolType::Variable, var, var->resolved, var->type);
    }
    for (const auto &[name, func] : node->functions) {
      children.emplace_back(SymbolType::Function, func, func->resolved, nullptr);
    }
    for (const auto &[name, cls] : node->classes) {
      children.emplace_back(SymbolType::Class, cls, cls->resolved, nullptr);
    }
    for (const auto &[name, scope_ptr] : node->scopes) {
      children.emplace_back(SymbolType::Scope, scope_ptr, nullptr, nullptr);
    }
    /* Sort elements to guarantee deterministic output (Row -> Col -> Alphabetical). */
    sort(children.begin(), children.end(), [](const Child &a, const Child &b) {
      return a.sym->loc < b.sym->loc;
    });

    /* Iterate and print the sorted children. */
    for (size_t i = 0; i < children.size(); ++i) {
      bool child_is_last = (i == children.size() - 1);

      vector<bool> next_is_last = is_last;
      next_is_last.push_back(child_is_last);

      print_line(children[i].sym->loc,
                 children[i].type,
                 children[i].sym->identifier,
                 children[i].cls,
                 children[i].resolved ? children[i].resolved->identifier : "",
                 depth + 1,
                 next_is_last);

      if (children[i].type == SymbolType::Scope) {
        const auto *next_node = static_cast<const SymbolScope *>(children[i].sym);
        /* Recurse deeper if the child acts as a container for other symbols. */
        self(self, next_node, depth + 1, next_is_last);
      }
    }
  };

  print_node(print_node, this, 0, {});
}

SymbolTable::SymbolTable(ParserBase &parser)
{
  LocalScope node = parser.root();
  root = scp_arena.alloc(node);
  flatten_root = scp_arena.alloc(node);
  register_builtins(node);
}

void SymbolTable::parse(LocalScope node, ErrorHandler &err_handler)
{
  SymbolParser symbol_parser(err_handler, *this, *flatten_root);
  symbol_parser.parse_scope(*root, node, "");

  if (err_handler.err.has_value()) {
    throw ParserException();
  }
}

void SymbolTable::register_builtins(LocalScope node)
{
  Token tok = node.back();

  struct BuiltinBasicType {
    string id;
    size_t size;
    size_t align;
  };

  const vector<BuiltinBasicType> types = {
      {err_symbol, 1, 1},

      {"void", 1, 1}, /* Only for function return type. */

      {"char", 1, 1},
      {"short", 2, 2},
      {"int", 4, 4},
      {"uchar", 1, 1},
      {"ushort", 2, 2},
      {"uint", 4, 4},
      {"half", 2, 2},
      {"float", 4, 4},
      {"bool", 1, 1},
      {"int32_t", 4, 4},
      {"uint32_t", 4, 4},
      {"bool32_t", 4, 4},

      {"string_t", 4, 4},

      {"samplerBuffer", 0, 1},
      {"sampler1D", 0, 1},
      {"sampler2D", 0, 1},
      {"sampler3D", 0, 1},
      {"isamplerBuffer", 0, 1},
      {"isampler1D", 0, 1},
      {"isampler2D", 0, 1},
      {"isampler3D", 0, 1},
      {"usamplerBuffer", 0, 1},
      {"usampler1D", 0, 1},
      {"usampler2D", 0, 1},
      {"usampler3D", 0, 1},
      {"sampler1DArray", 0, 1},
      {"sampler2DArray", 0, 1},
      {"isampler1DArray", 0, 1},
      {"isampler2DArray", 0, 1},
      {"usampler1DArray", 0, 1},
      {"usampler2DArray", 0, 1},
      {"samplerCube", 0, 1},
      {"isamplerCube", 0, 1},
      {"usamplerCube", 0, 1},
      {"samplerCubeArray", 0, 1},
      {"isamplerCubeArray", 0, 1},
      {"usamplerCubeArray", 0, 1},
      {"usampler1DAtomic", 0, 1},
      {"usampler2DAtomic", 0, 1},
      {"usampler2DArrayAtomic", 0, 1},
      {"usampler3DAtomic", 0, 1},
      {"isampler1DAtomic", 0, 1},
      {"isampler2DAtomic", 0, 1},
      {"isampler2DArrayAtomic", 0, 1},
      {"isampler3DAtomic", 0, 1},
      {"sampler2DDepth", 0, 1},
      {"sampler2DArrayDepth", 0, 1},
      {"samplerCubeDepth", 0, 1},
      {"samplerCubeArrayDepth", 0, 1},
      {"image1D", 0, 1},
      {"image2D", 0, 1},
      {"image3D", 0, 1},
      {"iimage1D", 0, 1},
      {"iimage2D", 0, 1},
      {"iimage3D", 0, 1},
      {"uimage1D", 0, 1},
      {"uimage2D", 0, 1},
      {"uimage3D", 0, 1},
      {"image1DArray", 0, 1},
      {"image2DArray", 0, 1},
      {"iimage1DArray", 0, 1},
      {"iimage2DArray", 0, 1},
      {"uimage1DArray", 0, 1},
      {"uimage2DArray", 0, 1},
      {"iimage2DAtomic", 0, 1},
      {"iimage3DAtomic", 0, 1},
      {"uimage2DAtomic", 0, 1},
      {"uimage3DAtomic", 0, 1},
      {"iimage2DArrayAtomic", 0, 1},
      {"uimage2DArrayAtomic", 0, 1},

      {"ShaderCreateInfo", 1, 1}, /* Only for compatibility. To be removed. */
  };

  for (const auto &t : types) {
    SymbolClass cls(root, tok, t.id, t.size, t.align);
    cls.is_std140_compatible = t.align == 4;
    cls.is_std430_compatible = t.align == 4;
    cls.is_error = t.id == err_symbol;

    SymbolClass *sym = cls_arena.alloc(cls);
    SymbolClass *sym_resolved = cls_arena.alloc(cls);
    sym->resolved = sym_resolved;
    root->classes.emplace(t.id, sym);
    flatten_root->classes.emplace(t.id, sym_resolved);
  }

  struct BuiltinVector {
    string id;
    string base;
    int comp;
    int align;
  };

  const std::vector<BuiltinVector> vectors = {
      {"float2", "float", 2, 8},
      {"float3", "float", 3, 16},
      {"float4", "float", 4, 16},
      {"float2x2", "float2", 2, 16},
      {"float2x3", "float3", 2, 16},
      {"float2x4", "float4", 2, 16},
      {"float3x2", "float2", 3, 16},
      {"float3x3", "float3", 3, 16},
      {"float3x4", "float4", 3, 16},
      {"float4x2", "float2", 4, 16},
      {"float4x3", "float3", 4, 16},
      {"float4x4", "float4", 4, 16},
      {"int2", "int", 2, 8},
      {"int3", "int", 3, 16},
      {"int4", "int", 4, 16},
      {"uint2", "uint", 2, 8},
      {"uint3", "uint", 3, 16},
      {"uint4", "uint", 4, 16},
      {"bool2", "bool", 2, 2},
      {"bool3", "bool", 3, 3},
      {"bool4", "bool", 4, 4},
      {"packed_float2", "float", 2, 8},
      {"packed_float3", "float", 3, 16},
      {"packed_float4", "float", 4, 16},
      {"packed_int2", "float", 2, 8},
      {"packed_int3", "float", 3, 16},
      {"packed_int4", "float", 4, 16},
  };

  const char *swizzle_xyzw[4] = {"x", "y", "z", "w"};
  const char *swizzle_rgba[4] = {"r", "g", "b", "a"};

  for (const auto &t : vectors) {
    SymbolClass *cls = cls_arena.alloc(
        root, tok, t.id, t.comp * root->classes[t.base]->size, t.align);
    root->classes.emplace(t.id, cls);
    root->scopes.emplace(t.id, cls);

    cls->is_std140_compatible = t.align >= 8 && t.id != "float3" && t.base != "float3";
    cls->is_std430_compatible = t.align >= 8 && t.id != "float3" && t.base != "float3";

    SymbolFunction *sub_op = fun_arena.alloc(
        cls, tok, subscript_operator_id, SymbolFunction::MEMBER);
    sub_op->return_type = root->classes[t.base];

    cls->operator_subscript = sub_op;
    cls->functions.emplace(subscript_operator_id, sub_op);

    SymbolClass *sym_resolved = cls_arena.alloc(*cls);
    cls->resolved = sym_resolved;
    flatten_root->classes.emplace(t.id, sym_resolved);

    for (int i = 0; i < t.comp; i++) {
      {
        string id(swizzle_xyzw[i]);
        SymbolVariable *var = var_arena.alloc(cls, root->classes[t.base], tok, id);
        cls->variables.emplace(id, var);
      }
      {
        string id(swizzle_rgba[i]);
        SymbolVariable *var = var_arena.alloc(cls, root->classes[t.base], tok, id);
        cls->variables.emplace(id, var);
      }
    }
  }

  /* Swizzle Generation */
  /* We do this in a separate loop because a swizzle on a float2 (like .xxxx)
   * might require float4 to already be registered in root->classes. */
  for (const auto &t : vectors) {
    /* Matrices (e.g., float2x2 where t.base is "float2") do not have standard swizzles.  We only
     * generate swizzles for vectors whose base components are scalars. */
    if (t.base != "float" && t.base != "int" && t.base != "uint" && t.base != "bool") {
      continue;
    }
    SymbolScope *scope = root->scopes[t.id];

    auto generate_swizzles =
        [&](auto &self, string current, int target_length, auto swizzle) -> void {
      if (current.length() == target_length) {
        /* e.g., "float" + "3" = "float3" */
        string return_type_id = t.base + to_string(target_length);

        SymbolVariable var(scope, root->classes[return_type_id], tok, current);

        scope->variables.emplace(current, var_arena.alloc(var));
        return;
      }
      /* Only allow swizzling components that actually exist on this vector type. */
      for (int i = 0; i < t.comp; i++) {
        self(self, current + swizzle[i], target_length, swizzle);
      }
    };
    /* Generate combinations for vec2, vec3, and vec4 variants. */
    for (int len = 2; len <= 4; len++) {
      generate_swizzles(generate_swizzles, "", len, swizzle_xyzw);
      generate_swizzles(generate_swizzles, "", len, swizzle_rgba);
    }
  }

  const std::vector<BuiltinFunc> functions = generate_all_builtin_functions();
  /* Builtin functions. */
  for (auto f : functions) {
    SymbolFunction *sym = fun_arena.alloc(root, tok, string(f.id), SymbolFunction::GLOBAL);
    sym->is_error = f.id == err_symbol;
    sym->return_type = root->lookup_class(f.return_type);
    SymbolFunction *sym_resolved = fun_arena.alloc(*sym);
    sym->resolved = sym_resolved;
    root->functions.emplace(sym->identifier, sym);
    flatten_root->functions.emplace(sym->identifier, sym_resolved);
  }

  /* Builtin operators. */

  const unordered_map<TokenType, const char *> unary_type_to_id = {
      {Increment, "inc_op_"},
      {Decrement, "dec_op_"},
      {Plus, "pos_op_"},
      {Minus, "neg_op_"},
      {BitwiseNot, "bitnot_op_"},
      {Not, "not_op_"},
  };

  const unordered_map<TokenType, const char *> binary_type_to_id = {
      {Plus, "add_op_"},
      {Minus, "sub_op_"},
      {Multiply, "mul_op_"},
      {Divide, "div_op_"},
      {Modulo, "mod_op_"},
      {And, "bitand_op_"},
      {Or, "bitor_op_"},
      {Xor, "bitxor_op_"},
      {LShift, "lshift_op_"},
      {RShift, "rshift_op_"},
      {Equal, "eq_op_"},
      {NotEqual, "neq_op_"},
      {LThan, "lte_op_"},
      {GThan, "gte_op_"},
      {LEqual, "leq_op_"},
      {GEqual, "geq_op_"},
      {LogicalAnd, "and_op_"},
      {LogicalOr, "or_op_"},
  };

  const vector<BuiltinType> builtin_types = generate_builtin_types();

  const vector<BuiltinOp> operators_desc = generate_all_operators(builtin_types);
  for (const auto &op : operators_desc) {
    string id((op.left.empty() ? unary_type_to_id : binary_type_to_id).find(op.op)->second);
    SymbolFunction *sym = fun_arena.alloc(root, tok, id, SymbolFunction::GLOBAL);
    sym->return_type = root->lookup_class(op.result);
    SymbolClass *left = op.left.empty() ? nullptr : root->lookup_class(op.left);
    SymbolClass *right = root->lookup_class(op.right);

    OperatorKey key(left, op.op, right);
    operators.emplace(key, sym);
  }

  const vector<BuiltinFunc> builtin_constructors = generate_all_constructors(builtin_types);
  for (const auto &fn : builtin_constructors) {
    SymbolFunction *sym = fun_arena.alloc(root, tok, fn.id, SymbolFunction::GLOBAL);
    sym->return_type = root->lookup_class(fn.return_type);
    for (auto arg : fn.arg_types) {
      sym->arg_types.emplace_back(root->lookup_class(arg));
    }
    sym->resolved = sym;
    root->function_emplace(sym);
  }

  struct BuiltinConst {
    string id;
    string type;
    bool is_constexpr;
    int value;
  };

  const std::vector<BuiltinConst> consts = {
      /* Error variable symbol. */
      {err_symbol, err_symbol, false, 1},

      {"true", "bool", true, 1},
      {"false", "bool", true, 0},
  };
  /* Boolean constants. */
  for (auto c : consts) {
    SymbolVariable *sym = var_arena.alloc(root, root->lookup_class(c.type), tok, string(c.id));
    SymbolVariable *sym_resolved = var_arena.alloc(*sym);
    sym->is_error = c.id == err_symbol;
    sym->resolved = sym_resolved;
    sym->is_static = c.is_constexpr;
    sym->is_constexpr = c.is_constexpr;
    sym->value = c.value;
    root->variables.emplace(sym->identifier, sym);
    flatten_root->variables.emplace(sym->identifier, sym_resolved);
  }
}

}  // namespace blender::gpu::shader::parser
