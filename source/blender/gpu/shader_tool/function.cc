/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "intermediate.hh"
#include "metadata.hh"
#include "processor.hh"

namespace blender::gpu::shader {
using namespace std;
using namespace shader::parser;
using namespace metadata;

/* Parse entry point definitions and mutating all parameter usage to global resources. */
void SourceProcessor::lower_entry_points(Parser &parser)
{
  using namespace metadata;

  auto to_uppercase = [](string str) {
    for (char &c : str) {
      c = toupper(c);
    }
    return str;
  };

  parser().foreach_function([&](bool, Token type, Token fn_name, Scope args, bool, Scope fn_body) {
    bool is_entry_point = false;
    bool is_compute_func = false;
    bool is_vertex_func = false;
    bool is_fragment_func = false;
    bool use_early_frag_test = false;
    bool use_clip_control = false;
    bool use_texture_atomic = false;
    string metal_max_total_threads_per_threadgroup;
    string local_size;

    if (type.prev() == ']' && type.prev().scope().type() == ScopeType::Subscript) {
      Scope attributes = type.prev().prev().scope();
      attributes.foreach_attribute([&](Token attr, Scope attr_scope) {
        const string attr_str(attr.str());
        if (attr_str == "vertex") {
          is_vertex_func = true;
          is_entry_point = true;
        }
        else if (attr_str == "fragment") {
          is_fragment_func = true;
          is_entry_point = true;
        }
        else if (attr_str == "compute") {
          is_compute_func = true;
          is_entry_point = true;
        }
        else if (attr_str == "early_fragment_tests") {
          use_early_frag_test = true;
        }
        else if (attr_str == "local_size") {
          local_size = attr_scope.str();
        }
        else if (attr_str == "metal_max_total_threads_per_threadgroup") {
          metal_max_total_threads_per_threadgroup = attr_scope.str();
        }
        else if (attr_str == "clip_control") {
          use_clip_control = true;
        }
        else if (attr_str == "texture_atomic") {
          use_texture_atomic = true;
        }
      });
    }

    if (is_entry_point && type.str() != "void") {
      report_error(type, "Entry point function must return void.");
      return;
    }

    auto parse_condition = [&](const Scope &attributes) {
      string cond;
      attributes.foreach_attribute([&](Token attribute_name, Scope attribute_parameters) {
        if (attribute_name.str() == "condition") {
          if (!cond.empty()) {
            report_error(attribute_name, "Only one condition attribute is allowed.");
            return;
          }
          attribute_parameters[1].scope().foreach_token(Word, [&](const Token tok) {
            cond += "int " + string(tok.str()) + " = ";
            cond += "ShaderCreateInfo::find_constant(constants, \"" + string(tok.str()) + "\"); ";
          });
          cond += "return " + string(attribute_parameters[1].scope().str()) + ";";
        }
      });

      if (!cond.empty()) {
        cond = ", [](blender::Span<CompilationConstant> constants) { " + cond + "}";
      }
      return cond;
    };

    auto replace_word = [&](const string &replaced, const string &replacement) {
      fn_body.foreach_token(Word, [&](const Token tok) {
        if (tok.str() == replaced) {
          parser.replace(tok, replacement, true);
        }
      });
    };

    auto replace_word_and_accessor = [&](const string &replaced, const string &replacement) {
      fn_body.foreach_token(Word, [&](const Token tok) {
        if (tok.next().type() == Dot && tok.str() == replaced) {
          parser.replace(tok, tok.next(), replacement);
        }
      });
    };

    /* For now, just emit good old create info macros. */
    string create_info_decl;

    if (!local_size.empty()) {
      if (!is_compute_func) {
        report_error(type, "Only compute entry point function can use [[local_size(x,y,z)]].");
      }
      else {
        create_info_decl += "LOCAL_GROUP_SIZE" + local_size + "\n";
      }
    }

    if (use_early_frag_test) {
      if (!is_fragment_func) {
        report_error(type, "Only fragment entry point function can use [[use_early_frag_test]].");
      }
      else {
        create_info_decl += "EARLY_FRAGMENT_TEST(true)\n";
      }
    }

    if (use_clip_control) {
      if (!is_vertex_func) {
        report_error(type, "Only vertex entry point function can use [[clip_control]].");
      }
      else {
        create_info_decl += "BUILTINS(BuiltinBits::CLIP_CONTROL)\n";
      }
    }

    if (use_texture_atomic) {
      create_info_decl += "BUILTINS(BuiltinBits::TEXTURE_ATOMIC)\n";
    }

    if (!metal_max_total_threads_per_threadgroup.empty()) {
      if (!is_compute_func) {
        report_error(type,
                     "Only compute entry point function can use "
                     "[[metal_max_total_threads_per_threadgroup(x)]].");
      }
      else {
        create_info_decl += "MTL_MAX_TOTAL_THREADS_PER_THREADGROUP" +
                            metal_max_total_threads_per_threadgroup + "\n";
      }
    }

    auto process_argument = [&](Token type, Token var, Scope attributes) {
      const bool is_const = type.prev() == TokenType::Const;
      string srt_type(type.str());
      string srt_var(var.str());
      string srt_attr(attributes[1].str());

      if (srt_attr == "vertex_id" && is_entry_point) {
        if (!is_vertex_func) {
          report_error(attributes[1], "[[vertex_id]] is only supported in vertex functions.");
        }
        else if (!is_const || srt_type != "int") {
          report_error(type, "[[vertex_id]] must be declared as `const int`.");
        }
        replace_word(srt_var, "gl_VertexID");
        metadata_.builtins.emplace_back(Builtin(hash("gl_VertexID")));
        create_info_decl += "BUILTINS(BuiltinBits::VERTEX_ID)\n";
      }
      else if (srt_attr == "instance_id" && is_entry_point) {
        if (!is_vertex_func) {
          report_error(attributes[1], "[[instance_id]] is only supported in vertex functions.");
        }
        else if (!is_const || srt_type != "int") {
          report_error(type, "[[instance_id]] must be declared as `const int`.");
        }
        replace_word(srt_var, "gl_InstanceID");
        metadata_.builtins.emplace_back(Builtin(hash("gl_InstanceID")));
        create_info_decl += "BUILTINS(BuiltinBits::INSTANCE_ID)\n";
      }
      else if (srt_attr == "instance_index" && is_entry_point) {
        if (!is_vertex_func) {
          report_error(attributes[1], "[[instance_index]] is only supported in vertex functions.");
        }
        else if (!is_const || srt_type != "int") {
          report_error(type, "[[instance_index]] must be declared as `const int`.");
        }
        replace_word(srt_var, "gpu_InstanceIndex");
        metadata_.builtins.emplace_back(Builtin(hash("gpu_InstanceIndex")));
        create_info_decl += "BUILTINS(BuiltinBits::INSTANCE_ID)\n";
      }
      else if (srt_attr == "base_instance" && is_entry_point) {
        if (!is_vertex_func) {
          report_error(attributes[1], "[[base_instance]] is only supported in vertex functions.");
        }
        else if (!is_const || srt_type != "int") {
          report_error(type,
                       "[[base_instance]] must be declared as "
                       "`const int`.");
        }
        replace_word(srt_var, "gpu_BaseInstance");
        metadata_.builtins.emplace_back(Builtin(hash("gpu_BaseInstance")));
        create_info_decl += "BUILTINS(BuiltinBits::INSTANCE_ID)\n";
      }
      else if (srt_attr == "point_size" && is_entry_point) {
        if (!is_vertex_func) {
          report_error(attributes[1], "[[point_size]] is only supported in vertex functions.");
        }
        else if (is_const || srt_type != "float") {
          report_error(type,
                       "[[point_size]] must be declared as non-const reference (aka `float &`).");
        }
        replace_word(srt_var, "gl_PointSize");
        create_info_decl += "BUILTINS(BuiltinBits::POINT_SIZE)\n";
      }
      else if (srt_attr == "clip_distance" && is_entry_point) {
        if (!is_vertex_func) {
          report_error(attributes[1], "[[clip_distance]] is only supported in vertex functions.");
        }
        else if (is_const || srt_type != "float") {
          report_error(type,
                       "[[clip_distance]] must be declared as non-const reference "
                       "(aka `float (&)[]`).");
        }
        replace_word(srt_var, "gl_ClipDistance");
        create_info_decl += "BUILTINS(BuiltinBits::CLIP_DISTANCES)\n";
      }
      else if (srt_attr == "layer" && is_entry_point) {
        if (is_compute_func) {
          report_error(attributes[1],
                       "[[layer]] is only supported in vertex and fragment functions.");
        }
        else if (is_vertex_func && (is_const || srt_type != "int")) {
          report_error(type,
                       "[[layer]] must be declared as non-const reference "
                       "(aka `int &`).");
        }
        else if (is_fragment_func && (!is_const || srt_type != "int")) {
          report_error(type,
                       "[[layer]] must be declared as const reference "
                       "(aka `const int &`).");
        }
        replace_word(srt_var, "gl_Layer");
        create_info_decl += "BUILTINS(BuiltinBits::LAYER)\n";
      }
      else if (srt_attr == "viewport_index" && is_entry_point) {
        if (is_compute_func) {
          report_error(attributes[1],
                       "[[viewport_index]] is only supported in vertex and "
                       "fragment functions.");
        }
        else if (is_vertex_func && (is_const || srt_type != "int")) {
          report_error(type,
                       "[[viewport_index]] must be declared as non-const reference "
                       "(aka `int &`).");
        }
        else if (is_fragment_func && (!is_const || srt_type != "int")) {
          report_error(type,
                       "[[viewport_index]] must be declared as const reference "
                       "(aka `const int &`).");
        }
        replace_word(srt_var, "gpu_ViewportIndex");
        create_info_decl += "BUILTINS(BuiltinBits::VIEWPORT_INDEX)\n";
      }
      else if (srt_attr == "position" && is_entry_point) {
        if (!is_vertex_func) {
          report_error(attributes[1], "[[position]] is only supported in vertex functions.");
        }
        else if (is_const || srt_type != "float4") {
          report_error(type,
                       "[[position]] must be declared as non-const reference (aka `float4 &`).");
        }
        else {
          replace_word(srt_var, "gl_Position");
        }
      }
      else if (srt_attr == "frag_coord" && is_entry_point) {
        if (!is_fragment_func) {
          report_error(attributes[1], "[[frag_coord]] is only supported in fragment functions.");
        }
        else if (!is_const || srt_type != "float4") {
          report_error(type, "[[frag_coord]] must be declared as `const float4`.");
        }
        else {
          create_info_decl += "BUILTINS(BuiltinBits::FRAG_COORD)\n";
          replace_word(srt_var, "gl_FragCoord");
        }
      }
      else if (srt_attr == "point_coord" && is_entry_point) {
        if (!is_fragment_func) {
          report_error(attributes[1], "[[point_coord]] is only supported in fragment functions.");
        }
        else if (!is_const || srt_type != "float2") {
          report_error(type, "[[point_coord]] must be declared as `const float2`.");
        }
        else {
          create_info_decl += "BUILTINS(BuiltinBits::POINT_COORD)\n";
          replace_word(srt_var, "gl_PointCoord");
        }
      }
      else if (srt_attr == "front_facing" && is_entry_point) {
        if (!is_fragment_func) {
          report_error(attributes[1], "[[front_facing]] is only supported in fragment functions.");
        }
        else if (!is_const || srt_type != "bool") {
          report_error(type, "[[front_facing]] must be declared as `const bool`.");
        }
        else {
          create_info_decl += "BUILTINS(BuiltinBits::FRONT_FACING)\n";
          replace_word(srt_var, "gl_FrontFacing");
        }
      }
      else if (srt_attr == "global_invocation_id" && is_entry_point) {
        if (!is_compute_func) {
          report_error(attributes[1],
                       "[[global_invocation_id]] is only supported in compute functions.");
        }
        else if (!is_const || srt_type != "uint3") {
          report_error(type, "[[global_invocation_id]] must be declared as `const uint3`.");
        }
        else {
          create_info_decl += "BUILTINS(BuiltinBits::GLOBAL_INVOCATION_ID)\n";
          replace_word(srt_var, "gl_GlobalInvocationID");
        }
      }
      else if (srt_attr == "local_invocation_id" && is_entry_point) {
        if (!is_compute_func) {
          report_error(attributes[1],
                       "[[local_invocation_id]] is only supported in compute functions.");
        }
        else if (!is_const || srt_type != "uint3") {
          report_error(type, "[[local_invocation_id]] must be declared as `const uint3`.");
        }
        else {
          create_info_decl += "BUILTINS(BuiltinBits::LOCAL_INVOCATION_ID)\n";
          replace_word(srt_var, "gl_LocalInvocationID");
        }
      }
      else if (srt_attr == "local_invocation_index" && is_entry_point) {
        if (!is_compute_func) {
          report_error(attributes[1],
                       "[[local_invocation_index]] is only supported in compute functions.");
        }
        else if (!is_const || srt_type != "uint") {
          report_error(type, "[[local_invocation_index]] must be declared as `const uint`.");
        }
        else {
          create_info_decl += "BUILTINS(BuiltinBits::LOCAL_INVOCATION_INDEX)\n";
          replace_word(srt_var, "gl_LocalInvocationIndex");
        }
      }
      else if (srt_attr == "work_group_id" && is_entry_point) {
        if (!is_compute_func) {
          report_error(attributes[1], "[[work_group_id]] is only supported in compute functions.");
        }
        else if (!is_const || srt_type != "uint3") {
          report_error(type,
                       "[[work_group_id]] must be declared as "
                       "`const uint3`.");
        }
        else {
          create_info_decl += "BUILTINS(BuiltinBits::WORK_GROUP_ID)\n";
          replace_word(srt_var, "gl_WorkGroupID");
        }
      }
      else if (srt_attr == "num_work_groups" && is_entry_point) {
        if (!is_compute_func) {
          report_error(attributes[1],
                       "[[num_work_groups]] is only supported in compute functions.");
        }
        else if (!is_const || srt_type != "uint3") {
          report_error(type,
                       "[[num_work_groups]] must be declared as "
                       "`const uint3`.");
        }
        else {
          create_info_decl += "BUILTINS(BuiltinBits::NUM_WORK_GROUP)\n";
          replace_word(srt_var, "gl_NumWorkGroups");
        }
      }
      else if (srt_attr == "in") {
        if (is_compute_func) {
          report_error(attributes[1],
                       "[[in]] is only supported in vertex and fragment functions.");
        }
        else if (!is_const) {
          report_error(type, "[[in]] must be declared as const reference.");
        }
        else if (is_vertex_func) {
          replace_word_and_accessor(srt_var, "");
          create_info_decl += "ADDITIONAL_INFO(" + srt_type + ")\n";
        }
        else if (is_fragment_func) {
          replace_word_and_accessor(srt_var, srt_type + "_");
          // create_info_decl += "VERTEX_OUT(" + srt_type + ")\n";
        }
      }
      else if (srt_attr == "subpass_in") {
        if (is_compute_func) {
          report_error(attributes[1], "[[subpass_in]] is only supported in fragment functions.");
        }
        else if (!is_const) {
          report_error(type, "[[subpass_in]] must be declared as const reference.");
        }
        else if (is_fragment_func) {
          replace_word_and_accessor(srt_var, srt_type + "_");
          create_info_decl += "ADDITIONAL_INFO(" + srt_type + ")\n";
        }
      }
      else if (srt_attr == "out") {
        if (is_compute_func) {
          report_error(attributes[1],
                       "[[out]] is only supported in vertex and fragment functions.");
        }
        else if (is_const) {
          report_error(type, "[[out]] must be declared as non-const reference.");
        }
        else if (is_vertex_func) {
          replace_word_and_accessor(srt_var, srt_type + "_");
          create_info_decl += "VERTEX_OUT(" + srt_type + "_t)\n";
        }
        else if (is_fragment_func) {
          replace_word_and_accessor(srt_var, srt_type + "_");
          create_info_decl += "ADDITIONAL_INFO(" + srt_type + ")\n";
        }
      }
      else if (srt_attr == "resource_table") {
        if (is_entry_point) {
          /* Add dummy var at start of function body. */
          parser.insert_after(fn_body.front().str_index_start(),
                              " " + srt_type + " " + srt_var + "{};");
          string res_condition_lambda = parse_condition(attributes);
          if (res_condition_lambda.empty()) {
            create_info_decl += "ADDITIONAL_INFO(" + srt_type + ")\n";
          }
          else {
            create_info_decl += ".additional_info_with_condition(\"" + srt_type + "\"" +
                                res_condition_lambda + ")\n";
          }
        }
      }
      else if (srt_attr == "frag_depth") {
        if (srt_type != "float") {
          report_error(type, "[[frag_depth]] needs to be declared as float");
        }
        const string mode(attributes[3].str());

        if (mode != "any" && mode != "greater" && mode != "less") {
          report_error(attributes[3], "unrecognized mode, expecting 'any', 'greater' or 'less'");
        }
        else {
          create_info_decl += "DEPTH_WRITE(DepthWrite::" + to_uppercase(mode) + ")\n";
          replace_word(srt_var, "gl_FragDepth");
        }
      }
      else if (srt_attr == "frag_stencil_ref") {
        if (srt_type != "int") {
          report_error(type, "[[frag_stencil_ref]] needs to be declared as int");
        }
        else {
          create_info_decl += "BUILTINS(BuiltinBits::STENCIL_REF)\n";
          replace_word(srt_var, "gl_FragStencilRefARB");
        }
      }
      else {
        report_error(attributes[1], "Invalid attribute.");
      }
    };

    args.foreach_match("[[..]]c?AA", [&](const vector<Token> toks) {
      process_argument(toks[8], toks[9], toks[1].scope());
    });
    args.foreach_match("[[..]]c?A&A", [&](const vector<Token> toks) {
      process_argument(toks[8], toks[10], toks[1].scope());
    });

    args.foreach_match("[[..]]c?A(&A)", [&](const vector<Token> toks) {
      process_argument(toks[8], toks[11], toks[1].scope());
    });

    if (is_entry_point) {
      if (create_info_decl.empty()) {
        /* Add unused define to avoid warning about unused expression. */
        create_info_decl += "DEFINE(\"EMPTY_CREATE_INFO\")\n";
      }
      create_info_decl = "GPU_SHADER_CREATE_INFO(" + string(fn_name.str()) + "_infos_)\n" +
                         create_info_decl + "GPU_SHADER_CREATE_END()\n";
      metadata_.create_infos_declarations.emplace_back(create_info_decl);
    }
  });

  parser.apply_mutations();
}

/* Removes entry point arguments to make it compatible with the legacy code.
 * Has to run after mutation related to function arguments. */
void SourceProcessor::lower_entry_points_signature(Parser &parser)
{
  using namespace metadata;

  parser().foreach_function([&](bool, Token type, Token name, Scope args, bool, Scope fn_body) {
    bool is_entry_point = false;

    if (type.prev() == ']' && type.prev().scope().type() == ScopeType::Subscript) {
      Scope attributes = type.prev().prev().scope();
      attributes.foreach_attribute([&](Token attr, Scope) {
        const string attr_str(attr.str());
        if (attr_str == "vertex" || attr_str == "fragment" || attr_str == "compute") {
          is_entry_point = true;
        }
      });
    }

    if (is_entry_point && args.str() != "()") {
      parser.erase(args.front().next(), args.back().prev());
    }

    /* Mute entry points when not enabled.
     * Could be lifted at some point, but for now required because of stage_in/out parameters. */
    if (is_entry_point) {
      /* Take attributes into account. */
      parser.insert_directive(type.prev().scope().front().prev(),
                              "#if defined(ENTRY_POINT_" + string(name.str()) + ")");
      parser.insert_directive(fn_body.back(), "#endif");
    }
  });

  parser.apply_mutations();
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

  auto add_vec_scalar_ternary = [&](const string &name, const vector<string> &types) {
    const string &scalar = types[0];
    for (size_t i = 1; i < types.size(); ++i) {
      functions.push_back({types[i], name, {types[i], scalar, scalar}});
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
      "floor", "trunc", "round", "roundEven", "ceil", "fract", "isnan", "isinf"};
  for (const auto &name : common_float_unary) {
    add_unary(name, f_types);
  }

  add_unary("abs", f_types);
  add_unary("abs", i_types);

  add_unary("sign", f_types);
  add_unary("sign", i_types);

  add_binary("mod", f_types);
  add_vec_scalar_binary("mod", f_types);

  for (const auto &tlist : {f_types, i_types, u_types}) {
    add_binary("min", tlist);
    add_vec_scalar_binary("min", tlist);
    add_binary("max", tlist);
    add_vec_scalar_binary("max", tlist);
    add_ternary("clamp", tlist);
    add_vec_scalar_ternary("clamp", tlist);
  }

  add_ternary("mix", f_types);
  add_vec_scalar_ternary("mix", f_types);
  for (size_t i = 0; i < f_types.size(); ++i) {
    functions.push_back({f_types[i], "mix", {f_types[i], f_types[i], b_types[i]}});
  }

  add_binary("step", f_types);
  add_vec_scalar_binary("step", f_types);
  add_ternary("smoothstep", f_types);
  add_vec_scalar_ternary("smoothstep", f_types);

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

  const vector<string> samplers = {"sampler1D",
                                   "sampler2D",
                                   "sampler3D",
                                   "samplerCube",
                                   "sampler1DArray",
                                   "sampler2DArray",
                                   "samplerCubeArray"};

  for (size_t i = 0; i < samplers.size(); ++i) {
    string coord_type = f_types[i < 4 ? i : (i == 4 ? 1 : (i == 5 ? 2 : 3))];
    functions.push_back({"float4", "texture", {samplers[i], coord_type}});
    functions.push_back({"float4", "textureLod", {samplers[i], coord_type, "float"}});
    functions.push_back({"int3", "textureSize", {samplers[i], "int"}});
  }

  const vector<string> images = {"image2D", "image2DArray"};

  for (size_t i = 0; i < images.size(); ++i) {
    string coord_type = f_types[i == 0 ? 1 : 2];
    functions.push_back({"float4", "imageLoad", {images[i], coord_type}});
    functions.push_back({"void", "imageStore", {images[i], coord_type, "float4"}});
    /* TODO atomics */
    functions.push_back({"int3", "imageSize", {images[i]}});
  }

  return functions;
}

}  // namespace blender::gpu::shader
