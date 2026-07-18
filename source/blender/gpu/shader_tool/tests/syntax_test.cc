/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "shader_tool_testing.hh"

namespace blender::gpu::tests {

using namespace shader;
using namespace std;

TEST(shader_tool, Array)
{
  {
    string input = R"(
float a[] = {0, 1};
float b[2] = {
    a[0],
    a(0, 1),
};
float d[] = {a[0], a(0, 1)};
)";
    string expect =
        R"(
float a[2] = ARRAY_T(float) ARRAY_V( 0, 1 );
float b[2] = ARRAY_T(float) ARRAY_V(
    a[0],
    a(0, 1)
 );
float d[2] = ARRAY_T(float) ARRAY_V( a[0], a(0, 1) );
)";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(expect, output);
    EXPECT_EQ(error, "");
  }
  {
    string input = R"(
float c[] = {};
)";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(error, "Array size must be greater than zero.");
  }
  {
    string input = R"(
float c[0] = {};
)";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(error, "Array size must be greater than zero.");
  }
  {
    string input = R"(
float2 c[2] = {{0, 1}, {0, 1}};
)";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(error, "Nested initializer list is not supported.");
  }
}

TEST(shader_tool, Swizzle)
{
  {
    string input = R"(a.xyzw().aaa().xxx().grba().yzww; aaaa();)";
    string expect = R"(a.xyzw  .aaa  .xxx  .grba  .yzww; aaaa();)";
    auto [output, _, error] = process_test_local(input);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
}

TEST(shader_tool, BinaryLiterals)
{
  {
    string input = R"(0b1 0b10u 0b10001000100010001000100010001000)";
    string expect = R"(1 2u 2290649224)";
    auto [output, _, error] = process_test_local(input);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
}

TEST(shader_tool, MatrixConstructors)
{
#ifndef __APPLE__ /* This processing is only done for metal compatibility. */
  GTEST_SKIP() << "This processing is only done for metal compatibility.";
  return;
#endif
  {
    string input = R"(mat3(a); mat3 a; my_mat4x4(a); mat2x2(a); mat3x2(a);)";
    string expect = R"(__mat3x3(a); mat3 a; my_mat4x4(a); __mat2x2(a); mat3x2(a);)";
    auto [output, _, error] = process_test_string(input, Language::GLSL);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
}

TEST(shader_tool, CommaDeclaration)
{
  {
    string input = R"(
struct A {
  int a, b;
};
)";
    string expect =
        R"(
struct A {
  int a;int b;
};
#line 2
                 A A_ctor_() {A r;r.a=0;r.b=0;return r;}


)";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(expect, output);
    EXPECT_EQ(error, "");
  }
}

TEST(shader_tool, Reference)
{
  {
    string input = R"(void func() { auto &a = b; a.a = 0; c = a(a); a_c_a = a; })";
    string expect = R"(void func() {              b.a = 0; c = a(b); a_c_a = b; })";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
  {
    string input = R"(void func() { const int &a = b; a.a = 0; c = a(a); })";
    string expect = R"(void func() {                   b.a = 0; c = a(b); })";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
  {
    string input = R"(void func() { const int i = 0; auto &a = b[i]; a.a = 0; })";
    string expect = R"(void func() { const int i = 0;                 b[i].a = 0; })";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
  {
    string input = R"(void func() { auto &a = b(0); })";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(error, "Reference definitions cannot contain function calls.");
  }
  {
    string input = R"(void func() { int i = 0; auto &a = b[i++]; })";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(error, "Reference definitions cannot have side effects.");
  }
  {
    string input = R"(void func() { auto &a = b[0 + 1]; })";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(error,
              "Array subscript inside reference declaration must be a single variable or a "
              "constant, not an expression.");
  }
  {
    string input = R"(void func() { auto &a = b[c]; })";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(error,
              "Cannot locate array subscript variable declaration. "
              "If it is a global variable, assign it to a temporary const variable for "
              "indexing inside the reference.");
  }
  {
    string input = R"(void func() { int c = 0; auto &a = b[c]; })";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(error, "Array subscript variable must be declared as const qualified.");
  }
  {
    string input = R"(auto &a = b;)";
    auto [output, _, error] = process_test_string(input);
    EXPECT_EQ(error, "Unexpected token \"&\": Expecting declaration");
  }
}

TEST(shader_tool, Constexpr)
{
  {
    string input =
        "static constexpr int a{1};\n"
        "static constexpr int b = {3};\n"
        //"// static constexpr int c = int{3}; /* TODO */\n"
        "static constexpr int d = 4;\n"
        "static constexpr int e = {{4}};\n"
        "static constexpr int f = (a + b / 2) * d - e;\n"
        "int func() { return f; }\n";
    string expect =
        "       constexpr int a= 1;\n"
        "       constexpr int b = 3;\n"
        "       constexpr int d = 4;\n"
        "       constexpr int e = 4;\n"
        "       constexpr int f = 4;\n"
        "int func() { return 4; }\n";
    auto [output, _, error] = process_test_string(input, Language::BSL);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
}

TEST(shader_tool, Auto)
{
  {
    string input =
        "auto a{0};\n"
        "auto b{0.0f};\n"
        "auto c{0.0f + 0};\n"
        "auto d = 0;\n"
        "auto e = 0.0f;\n"
        "auto f = 0.0f + 0;\n"
        "auto g = a * b;\n"
        "auto h = a * a;\n";
    string expect =
        "int a{0};\n"
        "float b{0.0f};\n"
        "float c{0.0f + 0};\n"
        "int d = 0;\n"
        "float e = 0.0f;\n"
        "float f = 0.0f + 0;\n"
        "float g = a * b;\n"
        "int h = a * a;\n";
    auto [output, _, error] = process_test_string(input, shader::Language::BSL);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
}

}  // namespace blender::gpu::tests
