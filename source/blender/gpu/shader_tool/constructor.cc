/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include "symbol.hh"

namespace blender::gpu::shader::parser {

/* Generates every single legal BSL constructor dynamically. */
vector<SymbolTable::BuiltinFunc> SymbolTable::generate_all_constructors(
    const vector<BuiltinType> &types)
{
  vector<BuiltinFunc> funcs;

  /* Isolate types by size for component-wise and mixed constructors. */
  vector<BuiltinType> scalars;
  vector<BuiltinType> vec2s;
  vector<BuiltinType> vec3s;
  for (const auto &t : types) {
    if (t.size == 1) {
      scalars.push_back(t);
    }
    else if (t.size == 2) {
      vec2s.push_back(t);
    }
    else if (t.size == 3) {
      vec3s.push_back(t);
    }
  }

  /* Vector and Scalar Constructors. */
  for (const auto &t : types) {
    /* Single-Argument Constructors. */
    for (const auto &u : types) {
      if (t.size == u.size) {
        /* Cast or Copy (e.g., float3(int3), int(float), float(float)) */
        funcs.push_back({t.name, t.name, {u.name}});
      }
      else if (t.size > 1 && u.size == 1) {
        /* Scalar-to-Vector Broadcast (e.g., float4(float), int3(bool)) */
        funcs.push_back({t.name, t.name, {u.name}});
      }
    }

    /* Component-wise Constructors.
     * Generates all scalar permutations matching the vector's size.
     * e.g., float3(int, float, bool). */
    if (t.size == 2) {
      for (const auto &s1 : scalars) {
        for (const auto &s2 : scalars) {
          funcs.push_back({t.name, t.name, {s1.name, s2.name}});
        }
      }
    }
    else if (t.size == 3) {
      /* 1 + 1 + 1 (e.g., float3(int, float, bool)). */
      for (const auto &s1 : scalars) {
        for (const auto &s2 : scalars) {
          for (const auto &s3 : scalars) {
            funcs.push_back({t.name, t.name, {s1.name, s2.name, s3.name}});
          }
        }
      }
      /* 1 + 2 (e.g., float3(float, int2)). */
      for (const auto &s : scalars) {
        for (const auto &v2 : vec2s) {
          funcs.push_back({t.name, t.name, {s.name, v2.name}});
        }
      }
      /* 2 + 1 (e.g., float3(int2, float)). */
      for (const auto &v2 : vec2s) {
        for (const auto &s : scalars) {
          funcs.push_back({t.name, t.name, {v2.name, s.name}});
        }
      }
    }
    else if (t.size == 4) {
      /* 1 + 1 + 1 + 1 (e.g., float4(int, float, bool, int)) */
      for (const auto &s1 : scalars) {
        for (const auto &s2 : scalars) {
          for (const auto &s3 : scalars) {
            for (const auto &s4 : scalars) {
              funcs.push_back({t.name, t.name, {s1.name, s2.name, s3.name, s4.name}});
            }
          }
        }
      }
      /* 1 + 3 (e.g., float4(float, int3)) */
      for (const auto &s : scalars) {
        for (const auto &v3 : vec3s) {
          funcs.push_back({t.name, t.name, {s.name, v3.name}});
        }
      }
      /* 3 + 1 (e.g., float4(int3, float)) */
      for (const auto &v3 : vec3s) {
        for (const auto &s : scalars) {
          funcs.push_back({t.name, t.name, {v3.name, s.name}});
        }
      }
      /* 2 + 2 (e.g., float4(float2, int2)) */
      for (const auto &v2a : vec2s) {
        for (const auto &v2b : vec2s) {
          funcs.push_back({t.name, t.name, {v2a.name, v2b.name}});
        }
      }
      /* 2 + 1 + 1 (e.g., float4(float2, int, int)) */
      for (const auto &v2 : vec2s) {
        for (const auto &s1 : scalars) {
          for (const auto &s2 : scalars) {
            funcs.push_back({t.name, t.name, {v2.name, s1.name, s2.name}});
          }
        }
      }
      /* 1 + 2 + 1 (e.g., float4(int, float2, int)) */
      for (const auto &s1 : scalars) {
        for (const auto &v2 : vec2s) {
          for (const auto &s2 : scalars) {
            funcs.push_back({t.name, t.name, {s1.name, v2.name, s2.name}});
          }
        }
      }
      /* 1 + 1 + 2 (e.g., float4(int, int, float2)) */
      for (const auto &s1 : scalars) {
        for (const auto &s2 : scalars) {
          for (const auto &v2 : vec2s) {
            funcs.push_back({t.name, t.name, {s1.name, s2.name, v2.name}});
          }
        }
      }
    }
  }

  /* Generates matrices across all base scalar types. */
  for (int n = 2; n <= 4; ++n) {
    for (int m = 2; m <= 4; ++m) {
      /* Construct the matrix type name (e.g., float3x3). */
      string mat_name = "float" + to_string(n) + "x" + to_string(m);

      /* Broadcast Constructor: Matrix initialized by a single scalar.
       * e.g., float3x3(float). */
      for (const auto &s : scalars) {
        funcs.push_back({mat_name, mat_name, {s.name}});
      }

      /* All-Scalar Constructor: Matrix initialized by N*N scalars.
       * Uses uniform scalar types to avoid combinatorial memory explosion.
       * e.g., float2x2(float, float, float, float) */
      for (const auto &s : scalars) {
        vector<string> args(n * n, s.name);
        funcs.push_back({mat_name, mat_name, args});
      }

      /* All-Vector Constructor: Matrix initialized by N column/row vectors.
       * Requires vectors matching the dimension N.
       * e.g., float3x3(float3, float3, float3), float4x4(int4, int4, int4, int4) */
      vector<string> args(n, "float" + to_string(n));
      funcs.push_back({mat_name, mat_name, args});
    }
  }

  return funcs;
}

}  // namespace blender::gpu::shader::parser
