/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h" /* IWYU pragma: export */

#include "processor.hh"

namespace blender::gpu::tests {

struct Result {
  /* Resulting Intermediate Language source. */
  std::string source;
  /* Parsed metadata. */
  shader::metadata::Source metadata;
  /* Error or empty string. */
  std::string error;
};

static inline Result process_test_string(
    std::string str, shader::Language language = shader::Language::BLENDER_GLSL)
{
  using namespace shader;
  SourceProcessor processor(str, "test.bsl", language, {});

  auto [result, metadata, error] = processor.convert();

  /* Strip first line directive as they are platform dependent. */
  size_t newline = result.find('\n');
  return {result.substr(newline + 1), metadata, error ? error.value().message : std::string()};
}

/* Process a test string inside a wrapper function. */
static inline Result process_test_local(std::string str,
                                        shader::Language language = shader::Language::BLENDER_GLSL)
{
  std::string prefix = "void wrapper_func() {";
  std::string suffix = "\n}";
  auto [result, metadata, error] = process_test_string(prefix + str + suffix, language);
  result = result.substr(prefix.size(), result.size() - suffix.size() - prefix.size());
  if (result.starts_with("\n\n")) {
    /* For compatibility with results set before the refactor. */
    result = result.substr(1);
  }
  return {result, metadata, error};
}

}  // namespace blender::gpu::tests
