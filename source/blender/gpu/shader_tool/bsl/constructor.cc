/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "symbol_table.hh"

namespace bsl {
using namespace std;
using namespace blender::gpu::shader::parser;
using namespace blender::gpu::shader::parser::ast;

/* Generates every single legal BSL constructor dynamically. */
vector<SymbolTable::BuiltinFunc> SymbolTable::generate_all_constructors(
    const vector<BuiltinType> &types)
{
  vector<BuiltinFunc> funcs;

  /* Group types by their base scalar/component type (e.g., float, int, bool) and dimension. */
  struct TypeFamily {
    const BuiltinType *scalar = nullptr; /* size 1 (e.g., float) */
    const BuiltinType *vec2 = nullptr;   /* size 2 (e.g., float2) */
    const BuiltinType *vec3 = nullptr;   /* size 3 (e.g., float3) */
    const BuiltinType *vec4 = nullptr;   /* size 4 (e.g., float4) */
  };

  unordered_map<string, TypeFamily> families;
  for (const auto &t : types) {
    if (t.size == 1) {
      families[t.base].scalar = &t;
    }
    else if (t.size == 2) {
      families[t.base].vec2 = &t;
    }
    else if (t.size == 3) {
      families[t.base].vec3 = &t;
    }
    else if (t.size == 4) {
      families[t.base].vec4 = &t;
    }
  }

  /* Vector and Scalar Constructors (restricted to matching component types). */
  for (const auto &[base_name, family] : families) {
    const auto *s = family.scalar;
    const auto *v2 = family.vec2;
    const auto *v3 = family.vec3;
    const auto *v4 = family.vec4;

    /* Scalar Copy: float(float) */
    if (s) {
      funcs.push_back({s->name, s->name, {s->name}});
    }

    /* Vec2 Constructors */
    if (v2) {
      /* Copy: float2(float2) */
      funcs.push_back({v2->name, v2->name, {v2->name}});
      if (s) {
        /* Broadcast: float2(float) */
        funcs.push_back({v2->name, v2->name, {s->name}});
        /* Component-wise (1+1): float2(float, float) */
        funcs.push_back({v2->name, v2->name, {s->name, s->name}});
      }
    }

    /* Vec3 Constructors */
    if (v3) {
      /* Copy: float3(float3) */
      funcs.push_back({v3->name, v3->name, {v3->name}});
      if (s) {
        /* Broadcast: float3(float) */
        funcs.push_back({v3->name, v3->name, {s->name}});
        /* 1 + 1 + 1: float3(float, float, float) */
        funcs.push_back({v3->name, v3->name, {s->name, s->name, s->name}});
      }
      if (s && v2) {
        /* 1 + 2: float3(float, float2) */
        funcs.push_back({v3->name, v3->name, {s->name, v2->name}});
        /* 2 + 1: float3(float2, float) */
        funcs.push_back({v3->name, v3->name, {v2->name, s->name}});
      }
    }

    /* Vec4 Constructors */
    if (v4) {
      /* Copy: float4(float4) */
      funcs.push_back({v4->name, v4->name, {v4->name}});
      if (s) {
        /* Broadcast: float4(float) */
        funcs.push_back({v4->name, v4->name, {s->name}});
        /* 1 + 1 + 1 + 1: float4(float, float, float, float) */
        funcs.push_back({v4->name, v4->name, {s->name, s->name, s->name, s->name}});
      }
      if (s && v3) {
        /* 1 + 3: float4(float, float3) */
        funcs.push_back({v4->name, v4->name, {s->name, v3->name}});
        /* 3 + 1: float4(float3, float) */
        funcs.push_back({v4->name, v4->name, {v3->name, s->name}});
      }
      if (v2) {
        /* 2 + 2: float4(float2, float2) */
        funcs.push_back({v4->name, v4->name, {v2->name, v2->name}});
      }
      if (s && v2) {
        /* 2 + 1 + 1: float4(float2, float, float) */
        funcs.push_back({v4->name, v4->name, {v2->name, s->name, s->name}});
        /* 1 + 2 + 1: float4(float, float2, float) */
        funcs.push_back({v4->name, v4->name, {s->name, v2->name, s->name}});
        /* 1 + 1 + 2: float4(float, float, float2) */
        funcs.push_back({v4->name, v4->name, {s->name, s->name, v2->name}});
      }
    }
  }

  /* Matrix Constructors (restricted to matching "float" component type). */
  for (int n = 2; n <= 4; ++n) {
    for (int m = 2; m <= 4; ++m) {
      string mat_name = "float" + to_string(n) + "x" + to_string(m);
      /* Broadcast: float3x3(float) */
      funcs.push_back({mat_name, mat_name, {"float"}});
      /* All-Scalar: float3x3(float, float, ...) [N*N scalars] */
      funcs.push_back({mat_name, mat_name, vector<string>(n * n, "float")});
      /* All-Vector: float3x3(float3, float3, float3) [N vectors] */
      funcs.push_back({mat_name, mat_name, vector<string>(n, "float" + to_string(n))});
    }
  }

  return funcs;
}

}  // namespace bsl
