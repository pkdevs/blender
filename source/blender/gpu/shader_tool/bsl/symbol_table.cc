/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "expression.hh"
#include "symbol_table.hh"

namespace bsl {

using namespace blender::gpu::shader::parser;
using namespace blender::gpu::shader::parser::ast;
using namespace std;
using Token = blender::gpu::shader::parser::Token;

static const char *subscript_operator_id = "arr_op_";

SymbolTable::SymbolTable(ParserBase &parser)
{
  LocalScope node = parser.root();
  root = scp_arena.alloc(node);
  register_builtins(node);
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
      return {0, AstNodeException(start, Diag::ErrorParsingIntegerLiteral)};
    }
  }

  /* Then use the expression parser to evaluate it. */
  try {
    ExpressionParser expression_parser;
    expression_parser.lexical_analysis(expr_str);
    return {expression_parser.eval(), {}};
  }
  catch (const std::exception &e) {
    return {0, AstNodeException(start, Diag::FailedToEvaluateExpandedExpr, expr_str)};
  }
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
      err = {init.child_first(), Diag::AutoTypeNestedInitializer, string(decl.identifier().str())};
    }
    else if (init.next().is_valid()) {
      err = {init.next(), Diag::AutoTypeMultipleExpressions, string(decl.identifier().str())};
    }
    else {
      return expr_type_analysis(scope, Expr(init.child_first()));
    }
  }

  return {scope.root_scope()->lookup_class(SymbolTable::err_symbol), err};
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
      err = AstNodeException(list, Diag::TemplateMissingArgument, string(arg.type().str()));
    }
    else if (arg.front() == Typename) {
      if (param.child_count() != 1 || param.child_first().type() != NodeType::LocalVar) {
        err = AstNodeException(param, Diag::TemplateInvalidArg, string(arg.type().str()));
      }
      else {
        IdQualified type_id(LocalVar(param.child_first()).identifier());
        auto *type = scope.lookup_class_base(type_id);
        if (type->template_data) {
          TemplateParamList list_nested = type_id.template_params();
          if (!list_nested.is_valid()) {
            err = AstNodeException(
                type_id, Diag::TemplateMissingParametersForType, string(type_id.str()));
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
          err = AstNodeException(param, Diag::UndeclaredIdentifier, string(type_id.str()));
        }
        else {
          result.str += "T" + type->identifier;
          result.str_debug += ", " + type->identifier;
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
            return {
                "0",
                AstNodeException(child, Diag::ExprCannotContainFunctionCalls, string(expr_type))};
          }
          else {
            return {
                "0",
                AstNodeException(
                    child, Diag::UnsupportedSymbolInExpr, string(child.str()), string(expr_type))};
          }
        }

        if (var->is_error) {
          return {"0", AstNodeException(child, Diag::UnknownVariable, string(child.str()))};
        }

        if (allow_compilation_constant && var->is_compilation_const) {
          /* Compilation constants are in global namespace. */
          expr_str += "SRT_CONSTANT_" + var->identifier;
        }
        else if (!var->is_constexpr) {
          return {"0", AstNodeException(child, Diag::NonConstVariableInExpr, string(expr_type))};
        }
        else {
          expr_str += to_string(var->value);
        }
        node_count++;
        break;
      }
      case NodeType::FuncCall:
        return {"0",
                AstNodeException(child, Diag::ExprCannotContainFunctionCalls, string(expr_type))};
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
                AstNodeException(
                    child, Diag::UnsupportedSymbolInExpr, string(child.str()), string(expr_type))};
    }
  }
  return {expr_str, error};
}

void SymbolTable::register_builtins(LocalScope node)
{
  Token tok = node.back();

  struct BuiltinBasicType {
    string id;
    size_t size;
    size_t align;
  };

  const vector<BuiltinBasicType> basic_types = {
      {err_symbol, 1, 1},

      {"char", 1, 1},
      {"short", 2, 2},
      {"int", 4, 4},
      {"uchar", 1, 1},
      {"ushort", 2, 2},
      {"uint", 4, 4},
      {"half", 2, 2},
      {"float", 4, 4},
      {"bool", 1, 1},
      {"bool32_t", 4, 4},
  };

  for (const auto &t : basic_types) {
    SymbolClass *cls = cls_arena.alloc(root, tok, t.id, t.size, t.align);
    cls->is_std140_compatible = t.align == 4;
    cls->is_std430_compatible = t.align == 4;
    cls->comp_len = 1;
    cls->is_error = t.id == err_symbol;
    root->classes.emplace(t.id, cls);
  }

  const vector<BuiltinBasicType> special_types = {
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

  for (const auto &t : special_types) {
    SymbolClass *sym = cls_arena.alloc(root, tok, t.id, t.size, t.align);
    sym->is_error = t.id == err_symbol;
    root->classes.emplace(t.id, sym);
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
      {"char2", "char", 2, 2},
      {"char3", "char", 3, 3},
      {"char4", "char", 4, 4},
      {"uchar2", "uchar", 2, 2},
      {"uchar3", "uchar", 3, 3},
      {"uchar4", "uchar", 4, 4},
      {"short2", "short", 2, 4},
      {"short3", "short", 3, 6},
      {"short4", "short", 4, 8},
      {"ushort2", "ushort", 2, 4},
      {"ushort3", "ushort", 3, 6},
      {"ushort4", "ushort", 4, 8},
      {"int2", "int", 2, 8},
      {"int3", "int", 3, 16},
      {"int4", "int", 4, 16},
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
    SymbolClass *base = root->classes[t.base];
    SymbolClass *cls = cls_arena.alloc(root, tok, t.id, t.comp * base->size, t.align);
    root->classes.emplace(t.id, cls);
    root->scopes.emplace(t.id, cls);

    cls->is_std140_compatible = t.align >= 8 && t.id != "float3" && t.base != "float3" &&
                                base->is_std140_compatible;
    cls->is_std430_compatible = cls->is_std140_compatible;

    SymbolFunction *sub_op = fun_arena.alloc(
        cls, tok, base, subscript_operator_id, SymbolFunction::MEMBER);

    cls->operator_subscript = sub_op;
    cls->functions.emplace(subscript_operator_id, sub_op);
    cls->comp_len = t.comp * base->comp_len;

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

  /* Aliases. */
  root->classes.emplace(string("int32_t"), root->classes["int"]);
  root->classes.emplace(string("uint32_t"), root->classes["uint"]);

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

  auto builtin_function_decl = [&](const BuiltinFunc &fn) {
    SymbolClass *ret_type = root->lookup_class(fn.return_type);
    SymbolFunction *sym = fun_arena.alloc(root, tok, ret_type, fn.id, SymbolFunction::GLOBAL);
    sym->is_error = fn.id == err_symbol;
    sym->is_builtin_ctor = true;
    for (auto arg : fn.arg_types) {
      sym->add_argument(root->lookup_class(arg));
    }
    root->function_emplace(sym, true);
  };

  const vector<BuiltinFunc> functions = generate_all_builtin_functions();
  /* Builtin functions. */
  for (const auto &f : functions) {
    builtin_function_decl(f);
  }

  /* Builtin operators. */

  unary_type_to_id = {
      {Increment, "inc_op_"},
      {Decrement, "dec_op_"},
      {Plus, "pos_op_"},
      {Minus, "neg_op_"},
      {BitwiseNot, "bitnot_op_"},
      {Not, "not_op_"},
  };

  binary_type_to_id = {
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
    SymbolClass *ret_type = root->lookup_class(op.result);
    string id((op.left.empty() ? unary_type_to_id : binary_type_to_id).find(op.op)->second);
    SymbolFunction *sym = fun_arena.alloc(root, tok, ret_type, id, SymbolFunction::GLOBAL);
    SymbolClass *left = op.left.empty() ? nullptr : root->lookup_class(op.left);
    SymbolClass *right = root->lookup_class(op.right);

    OperatorKey key(left, op.op, right);
    operators.emplace(key, sym);
  }

  const vector<BuiltinFunc> builtin_constructors = generate_all_constructors(builtin_types);
  for (const auto &fn : builtin_constructors) {
    builtin_function_decl(fn);
  }

  image_formats = {
      {0, "SNORM_8"},           {1, "SNORM_8_8"},        {2, "SNORM_8_8_8_8"},
      {3, "SNORM_16"},          {4, "SNORM_16_16"},      {5, "SNORM_16_16_16_16"},
      {6, "UNORM_8"},           {7, "UNORM_8_8"},        {8, "UNORM_8_8_8_8"},
      {9, "UNORM_16"},          {10, "UNORM_16_16"},     {11, "UNORM_16_16_16_16"},
      {12, "SINT_8"},           {13, "SINT_8_8"},        {14, "SINT_8_8_8_8"},
      {15, "SINT_16"},          {16, "SINT_16_16"},      {17, "SINT_16_16_16_16"},
      {18, "SINT_32"},          {19, "SINT_32_32"},      {20, "SINT_32_32_32_32"},
      {21, "UINT_8"},           {22, "UINT_8_8"},        {23, "UINT_8_8_8_8"},
      {24, "UINT_16"},          {25, "UINT_16_16"},      {26, "UINT_16_16_16_16"},
      {27, "UINT_32"},          {28, "UINT_32_32"},      {29, "UINT_32_32_32_32"},
      {30, "SFLOAT_16"},        {31, "SFLOAT_16_16"},    {32, "SFLOAT_16_16_16_16"},
      {33, "SFLOAT_32"},        {34, "SFLOAT_32_32"},    {35, "SFLOAT_32_32_32_32"},
      {36, "UNORM_10_10_10_2"}, {37, "UINT_10_10_10_2"}, {38, "UFLOAT_11_11_10"},
  };

  struct BuiltinConst {
    string id;
    string type;
    bool is_constexpr;
    int value;
  };

  std::vector<BuiltinConst> consts = {
      /* Error variable symbol. */
      {err_symbol, err_symbol, false, 1},

      {"true", "bool", true, 1},
      {"false", "bool", true, 0},

      /* WORKAROUND: Should become an entry point argument. */
      {"gl_FragStencilRefARB", "uint", false, 0},

      {"FLT_MAX", "float", false, 0},
      {"FLT_MIN", "float", false, 0},
      {"FLT_EPSILON", "float", false, 0},
      {"SHRT_MAX", "float", false, 0},
      {"INT_MAX", "float", false, 0},
      {"USHRT_MAX", "float", false, 0},
      {"UINT_MAX", "float", false, 0},
      {"NAN_FLT", "float", false, 0},
      {"FLT_11_MAX", "float", false, 0},
      {"FLT_10_MAX", "float", false, 0},
      {"FLT_11_11_10_MAX", "float3", false, 0},
  };

  for (auto [value, name] : image_formats) {
    consts.emplace_back(name, "int", true, value);
  }

  /* Boolean constants. */
  for (auto c : consts) {
    SymbolVariable *sym = var_arena.alloc(root, root->lookup_class(c.type), tok, string(c.id));
    sym->is_error = c.id == err_symbol;
    sym->is_static = c.is_constexpr;
    sym->is_constexpr = c.is_constexpr;
    sym->value = c.value;
    root->variables.emplace(sym->identifier, sym);
  }

  /* Fast lookup cache. */
  this->float_cls = root->lookup_class("float");
  this->int_cls = root->lookup_class("int");
  this->uint_cls = root->lookup_class("uint");
  this->str_cls = root->lookup_class("string_t");
  this->err_cls = root->lookup_class(SymbolTable::err_symbol);
  this->bool_cls = root->lookup_class("bool");
  this->void_cls = root->lookup_class("void");
  this->float2_cls = root->lookup_class("float2");
  this->float3_cls = root->lookup_class("float3");
  this->float4_cls = root->lookup_class("float4");
  this->packed_float2_cls = root->lookup_class("packed_float2");
  this->packed_float3_cls = root->lookup_class("packed_float3");
  this->packed_float4_cls = root->lookup_class("packed_float4");
}

SymbolClass *SymbolTable::get_literal_type(string_view lit) const
{
  /* Convert to lowercase helper for easier suffix matching */
  string str;
  str.reserve(lit.size());
  for (char c : lit) {
    str.push_back(tolower(static_cast<unsigned char>(c)));
  }

  bool has_decimal = (str.find('.') != string_view::npos);
  bool has_exponent = !str.starts_with("0x") && (str.find('e') != string_view::npos);

  if (has_decimal || has_exponent || str.back() == 'f') {
    return float_cls;
  }
  if (str.back() == 'u') {
    return uint_cls;
  }
  if (str == "true" || str == "false") {
    return bool_cls;
  }
  return int_cls;
}

bool SymbolTable::is_convertible(const SymbolClass *from,
                                 const SymbolClass *to,
                                 bool allow_vector_promotion) const
{
  /* Check 'packed_floatX' to 'floatX' promotion. */
  if (from == packed_float3_cls && to == float3_cls) {
    return true;
  }
  if (from == packed_float2_cls && to == float2_cls) {
    return true;
  }
  if (from == packed_float4_cls && to == float4_cls) {
    return true;
  }
  /* Promote scalar. */
  if (from->comp_len == 1 && from->comp_len == to->comp_len) {
    return true;
  }
  if (allow_vector_promotion && from->comp_len != 0 && from->comp_len == to->comp_len) {
    return true;
  }
  return false;
}

/* Generates every single legal BSL builtin function dynamically. */
vector<SymbolTable::BuiltinFunc> SymbolTable::generate_all_builtin_functions()
{
  /* Start with the error symbol. */
  vector<BuiltinFunc> functions = {{"void", err_symbol, {}}};

  /* Special functions */
  functions.push_back({"void", "printf", {"string_t"}});

  /* Type definition vectors for vectorized generation */
  const vector<string> f_types = {"float", "float2", "float3", "float4"};
  const vector<string> i_types = {"int", "int2", "int3", "int4"};
  const vector<string> u_types = {"uint", "uint2", "uint3", "uint4"};
  const vector<string> b_types = {"bool", "bool2", "bool3", "bool4"};

  /* 1-argument component-wise functions (e.g. sin, cos, abs). */
  auto add_unary = [&](const string &name, const vector<string> &types) {
    for (const auto &t : types) {
      functions.push_back({t, name, {t}});
    }
  };

  /* 2-argument component-wise functions (e.g. pow, min, max). */
  auto add_binary = [&](const string &name, const vector<string> &types) {
    for (const auto &t : types) {
      functions.push_back({t, name, {t, t}});
    }
  };

  /* 3-argument component-wise functions (e.g. clamp, mix, fma). */
  auto add_ternary = [&](const string &name, const vector<string> &types) {
    for (const auto &t : types) {
      functions.push_back({t, name, {t, t, t}});
    }
  };

  /* Functions with vector-scalar variants (e.g. min(float3, float), clamp(float3, float, float)).
   */
  auto add_vec_scalar_binary = [&](const string &name, const vector<string> &types) {
    const string &scalar = types[0];
    for (size_t i = 1; i < types.size(); ++i) {
      functions.push_back({types[i], name, {types[i], scalar}});
    }
  };

  auto add_vec_scalar_scalar_ternary = [&](const string &name, const vector<string> &types) {
    const string &scalar = types[0];
    for (size_t i = 1; i < types.size(); ++i) {
      functions.push_back({types[i], name, {types[i], scalar, scalar}});
    }
  };
  auto add_vec_vec_scalar_ternary = [&](const string &name, const vector<string> &types) {
    const string &scalar = types[0];
    for (size_t i = 1; i < types.size(); ++i) {
      functions.push_back({types[i], name, {types[i], types[i], scalar}});
    }
  };

  const vector<string> trig_unary = {"radians",
                                     "degrees",
                                     "sin",
                                     "cos",
                                     "tan",
                                     "asin",
                                     "acos",
                                     "atan",
                                     "sinh",
                                     "cosh",
                                     "tanh",
                                     "asinh",
                                     "acosh",
                                     "atanh"};
  for (const auto &name : trig_unary) {
    add_unary(name, f_types);
  }
  /* 2-argument atan(y, x). */
  add_binary("atan", f_types);

  const vector<string> exp_unary = {"exp", "log", "exp2", "log2", "sqrt", "inversesqrt"};
  for (const auto &name : exp_unary) {
    add_unary(name, f_types);
  }
  add_binary("pow", f_types);

  const vector<string> common_float_unary = {
      "floor",
      "trunc",
      "round",
      "roundEven",
      "ceil",
      "fract",
      "saturate",
  };
  for (const auto &name : common_float_unary) {
    add_unary(name, f_types);
  }

  for (size_t i = 0; i < f_types.size(); ++i) {
    functions.push_back({b_types[i], "isnan", {f_types[i]}});
    functions.push_back({b_types[i], "isinf", {f_types[i]}});
  }

  add_unary("abs", f_types);
  add_unary("abs", i_types);

  add_unary("sign", f_types);
  add_unary("sign", i_types);

  add_binary("mod", f_types);
  add_binary("modf", f_types);
  add_vec_scalar_binary("mod", f_types);

  for (const auto &tlist : {f_types, i_types, u_types}) {
    add_binary("min", tlist);
    add_vec_scalar_binary("min", tlist);
    add_binary("max", tlist);
    add_vec_scalar_binary("max", tlist);
    add_ternary("clamp", tlist);
    add_vec_scalar_scalar_ternary("clamp", tlist);
  }

  add_ternary("mix", f_types);
  add_vec_vec_scalar_ternary("mix", f_types);
  for (size_t i = 0; i < f_types.size(); ++i) {
    functions.push_back({f_types[i], "mix", {f_types[i], f_types[i], b_types[i]}});
    functions.push_back({f_types[i], "select", {f_types[i], f_types[i], b_types[i]}});
  }

  add_binary("step", f_types);
  add_vec_scalar_binary("step", f_types);
  add_ternary("smoothstep", f_types);
  add_vec_vec_scalar_ternary("smoothstep", f_types);

  add_ternary("fma", f_types);

  // Bit-reinterpretation conversions
  for (size_t i = 0; i < f_types.size(); ++i) {
    functions.push_back({i_types[i], "floatBitsToInt", {f_types[i]}});
    functions.push_back({u_types[i], "floatBitsToUint", {f_types[i]}});
    functions.push_back({f_types[i], "intBitsToFloat", {i_types[i]}});
    functions.push_back({f_types[i], "uintBitsToFloat", {u_types[i]}});
  }

  functions.push_back({"uint", "packUnorm2x16", {"float2"}});
  functions.push_back({"uint", "packSnorm2x16", {"float2"}});
  functions.push_back({"uint", "packUnorm4x8", {"float4"}});
  functions.push_back({"uint", "packSnorm4x8", {"float4"}});
  functions.push_back({"uint", "packHalf2x16", {"float2"}});

  functions.push_back({"float2", "unpackUnorm2x16", {"uint"}});
  functions.push_back({"float2", "unpackSnorm2x16", {"uint"}});
  functions.push_back({"float4", "unpackUnorm4x8", {"uint"}});
  functions.push_back({"float4", "unpackSnorm4x8", {"uint"}});
  functions.push_back({"float2", "unpackHalf2x16", {"uint"}});

  for (size_t i = 0; i < f_types.size(); ++i) {
    functions.push_back({"float", "length", {f_types[i]}});
    functions.push_back({"float", "distance", {f_types[i], f_types[i]}});
    functions.push_back({"float", "dot", {f_types[i], f_types[i]}});
    functions.push_back({f_types[i], "normalize", {f_types[i]}});
    functions.push_back({f_types[i], "faceforward", {f_types[i], f_types[i], f_types[i]}});
    functions.push_back({f_types[i], "reflect", {f_types[i], f_types[i]}});
    functions.push_back({f_types[i], "refract", {f_types[i], f_types[i], "float"}});
  }
  functions.push_back({"float3", "cross", {"float3", "float3"}});

  const vector<string> square_mats = {"float2x2", "float3x3", "float4x4"};
  const vector<string> all_mats = {"float2x2",
                                   "float3x3",
                                   "float4x4",
                                   "float2x3",
                                   "float2x4",
                                   "float3x2",
                                   "float3x4",
                                   "float4x2",
                                   "float4x3"};

  for (const auto &m : all_mats) {
    functions.push_back({m, "transpose", {m}});
  }

  for (const auto &m : square_mats) {
    functions.push_back({"float", "determinant", {m}});
    functions.push_back({m, "inverse", {m}});
  }

  const vector<string> rel_ops = {
      "lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual", "equal", "notEqual"};

  for (const auto &op : rel_ops) {
    for (size_t i = 1; i < f_types.size(); ++i) {
      functions.push_back({b_types[i], op, {f_types[i], f_types[i]}});
      functions.push_back({b_types[i], op, {i_types[i], i_types[i]}});
      functions.push_back({b_types[i], op, {u_types[i], u_types[i]}});
    }
  }

  for (size_t i = 1; i < b_types.size(); ++i) {
    functions.push_back({"bool", "any", {b_types[i]}});
    functions.push_back({"bool", "all", {b_types[i]}});
    functions.push_back({b_types[i], "not", {b_types[i]}});
  }

  const vector<string> bit_unary = {"bitfieldReverse", "bitCount", "findLSB", "findMSB"};
  for (const auto &op : bit_unary) {
    add_unary(op, i_types);
    add_unary(op, u_types);
  }

  for (const auto &tlist : {i_types, u_types}) {
    for (const auto &t : tlist) {
      functions.push_back({t, "bitfieldExtract", {t, "int", "int"}});
      functions.push_back({t, "bitfieldInsert", {t, t, "int", "int"}});
    }
  }

  const vector<string> atomic_2arg = {"atomicAdd",
                                      "atomicMin",
                                      "atomicMax",
                                      "atomicAnd",
                                      "atomicOr",
                                      "atomicXor",
                                      "atomicExchange"};

  for (const auto &op : atomic_2arg) {
    functions.push_back({"int", op, {"int", "int"}});
    functions.push_back({"uint", op, {"uint", "uint"}});
  }
  functions.push_back({"int", "atomicCompSwap", {"int", "int", "int"}});
  functions.push_back({"uint", "atomicCompSwap", {"uint", "uint", "uint"}});

  const vector<string> deriv_funcs = {"gpu_dfdx", "gpu_dfdy", "gpu_fwidth"};

  for (const auto &op : deriv_funcs) {
    add_unary(op, f_types);
  }

  functions.push_back({"void", "gpu_discard_fragment", {}});
  functions.push_back({"void", "barrier", {}});
  functions.push_back({"void", "assert", {"bool"}});

  functions.push_back({"float4", "texture", {"sampler2DDepth", "float2"}});
  functions.push_back({"float4", "textureLod", {"sampler2DDepth", "float2", "float"}});
  functions.push_back({"int2", "textureSize", {"sampler2DDepth", "int"}});
  functions.push_back({"float4", "texelFetch", {"sampler2DDepth", "int2", "int"}});

  const vector<pair<string, string>> image_types = {
      {"uint4", "u"},
      {"int4", "i"},
      {"float4", ""},
  };

  for (auto [type, prefix] : image_types) {
    vector<pair<string, int>> samplers = {
        /* Name, Coordinate Dimensions. */
        {prefix + "sampler1D", 1},
        {prefix + "sampler2D", 2},
        {prefix + "sampler3D", 3},
        {prefix + "samplerCube", 3},
        {prefix + "sampler1DArray", 2},
        {prefix + "sampler2DArray", 3},
        {prefix + "samplerCubeArray", 4},
    };

    if (prefix.empty()) {
      samplers.emplace_back(prefix + "sampler2DDepth", 2);
      samplers.emplace_back(prefix + "samplerCubeDepth", 3);
      samplers.emplace_back(prefix + "sampler2DArrayDepth", 3);
      samplers.emplace_back(prefix + "samplerCubeArrayDepth", 4);
    }

    for (auto [sampler, dim] : samplers) {
      string coord_type = f_types[dim - 1];
      string icoord_type = i_types[dim - 1];
      functions.push_back({type, "texture", {sampler, coord_type}});
      functions.push_back({type, "textureLod", {sampler, coord_type, "float"}});
      functions.push_back({icoord_type, "textureSize", {sampler, "int"}});
      functions.push_back({type, "texelFetch", {sampler, icoord_type, "int"}});
    }

    const vector<string> images = {
        prefix + "image2D",
        prefix + "image2DArray",
        prefix + "image3D",
    };

    for (size_t i = 0; i < images.size(); ++i) {
      string coord_type = i_types[i == 0 ? 1 : 2];
      functions.push_back({type, "imageLoad", {images[i], coord_type}});
      functions.push_back({type, "imageLoadFast", {images[i], coord_type}});
      functions.push_back({"void", "imageStore", {images[i], coord_type, type}});
      functions.push_back({"void", "imageStoreFast", {images[i], coord_type, type}});

      if (!prefix.empty()) {
        functions.push_back({type, "imageAtomicAdd", {images[i], coord_type, type}});
        functions.push_back({type, "imageAtomicMin", {images[i], coord_type, type}});
        functions.push_back({type, "imageAtomicMax", {images[i], coord_type, type}});
        functions.push_back({type, "imageAtomicAnd", {images[i], coord_type, type}});
        functions.push_back({type, "imageAtomicOr", {images[i], coord_type, type}});
        functions.push_back({type, "imageAtomicXor", {images[i], coord_type, type}});
        functions.push_back({type, "imageAtomicExchange", {images[i], coord_type, type}});
        functions.push_back({type, "imageAtomicCompSwap", {images[i], coord_type, type, type}});
      }

      functions.push_back({coord_type, "imageSize", {images[i]}});
      functions.push_back({"void", "imageFence", {images[i]}});
    }
  }

  return functions;
}

}  // namespace bsl
