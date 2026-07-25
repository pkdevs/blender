/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BKE_appdir.hh"

#include "BLI_fileops.hh"
#include "BLI_path_utils.hh"
#ifdef _WIN32
#  include "BLI_winstuff.hh"
#endif

#include "vk_shader.hh"
#include "vk_shader_compiler.hh"

#include <cstring>
#include <iostream>
#include <string>

#include "CLG_log.h"

#include "spirv/unified1/spirv.h"

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.vulkan"};

static std::optional<std::string> cache_dir_get()
{
  static std::optional<std::string> result = []() -> std::optional<std::string> {
    static char tmp_dir_buffer[FILE_MAX];
    /* Shader builder doesn't return the correct appdir. */
    BKE_appdir_folder_caches(tmp_dir_buffer, sizeof(tmp_dir_buffer));

    std::string cache_dir = std::string(tmp_dir_buffer) + "vk-spirv-cache" + SEP_STR;
    BLI_dir_create_recursive(cache_dir.c_str());
    return cache_dir;
  }();

  return result;
}

/* -------------------------------------------------------------------- */
/** \name SPIR-V disk cache
 * \{ */

struct SPIRVSidecar {
  /** Size of the SPIRV binary. */
  uint64_t spirv_size;
};

static bool read_spirv_from_disk(VKShaderModule &shader_module, StringRef hash_extra)
{
  if (G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) {
    /* Debug information is not part of the cached SPIR-V, so don't use the cache in this case. */
    return false;
  }
  if (!cache_dir_get().has_value()) {
    return false;
  }
  shader_module.build_sources_hash(hash_extra);
  std::string spirv_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash + ".spv";
  std::string sidecar_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash +
                             ".sidecar.bin";

  if (!BLI_exists(spirv_path.c_str()) || !BLI_exists(sidecar_path.c_str())) {
    return false;
  }

  BLI_file_touch(spirv_path.c_str());
  BLI_file_touch(sidecar_path.c_str());

  /* Read sidecar. */
  fstream sidecar_file(sidecar_path, std::ios::binary | std::ios::in | std::ios::ate);
  std::streamsize sidecar_size_on_disk = sidecar_file.tellg();
  SPIRVSidecar sidecar = {};
  if (sidecar_size_on_disk != sizeof(sidecar)) {
    return false;
  }
  sidecar_file.seekg(0, std::ios::beg);
  sidecar_file.read(reinterpret_cast<char *>(&sidecar), sizeof(sidecar));

  /* Read spirv binary. */
  fstream spirv_file(spirv_path, std::ios::binary | std::ios::in | std::ios::ate);
  std::streamsize size = spirv_file.tellg();
  if (size != sidecar.spirv_size) {
    return false;
  }
  spirv_file.seekg(0, std::ios::beg);
  shader_module.spirv_binary.resize(size / 4);
  spirv_file.read(reinterpret_cast<char *>(shader_module.spirv_binary.data()), size);

  CLOG_TRACE(&LOG, "reading SpirV from disk %s", spirv_path.c_str());
  return true;
}

static void write_spirv_to_disk(VKShaderModule &shader_module)
{
  if (G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) {
    /* Debug information is not part of the cached SPIR-V, so don't use the cache in this case. */
    return;
  }
  if (!cache_dir_get().has_value()) {
    return;
  }

  /* Write the SPIR-V binary. Note that spirv_binary is used instead of compilation_result so
   * that any post-processing (e.g. the injected OpString shader name) is stored in the cache and
   * does not have to be redone every time the cache entry is read back. */
  std::string spirv_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash + ".spv";
  CLOG_TRACE(&LOG, "write SpirV to disk %s", spirv_path.c_str());
  size_t size = shader_module.spirv_binary.size() * sizeof(uint32_t);
  fstream spirv_file(spirv_path, std::ios::binary | std::ios::out);
  spirv_file.write(reinterpret_cast<const char *>(shader_module.spirv_binary.data()), size);

  /* Write the sidecar */
  SPIRVSidecar sidecar = {size};
  std::string sidecar_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash +
                             ".sidecar.bin";
  fstream sidecar_file(sidecar_path, std::ios::binary | std::ios::out);
  sidecar_file.write(reinterpret_cast<const char *>(&sidecar), sizeof(SPIRVSidecar));
}

void VKShaderCompiler::cache_dir_clear_old()
{
  if (!cache_dir_get().has_value()) {
    return;
  }

  direntry *entries = nullptr;
  uint32_t dir_len = BLI_filelist_dir_contents(cache_dir_get()->c_str(), &entries);
  for (int i : IndexRange(dir_len)) {
    direntry entry = entries[i];
    if (S_ISDIR(entry.s.st_mode)) {
      continue;
    }
    const time_t ts_now = time(nullptr);
    const time_t delete_threshold = 60 /*seconds*/ * 60 /*minutes*/ * 24 /*hours*/ * 30 /*days*/;
    if (entry.s.st_mtime + delete_threshold < ts_now) {
      BLI_delete(entry.path, false, false);
    }
  }
  BLI_filelist_free(entries, dir_len);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compilation
 * \{ */

static StringRef to_stage_name(shaderc_shader_kind stage)
{
  switch (stage) {
    case shaderc_vertex_shader:
      return "vertex";
    case shaderc_geometry_shader:
      return "geometry";
    case shaderc_fragment_shader:
      return "fragment";
    case shaderc_compute_shader:
      return "compute";

    default:
      BLI_assert_msg(false, "Do not know how to convert shaderc_shader_kind to stage name.");
      break;
  }
  return "unknown stage";
}

/**
 * Injects a standalone "OpString" holding the shader name into the SPIR-V binary.
 * This way, the human-readable shader name can be found/referenced when inspecting the binary or
 * debugging even when not using the flag G_DEBUG_GPU_SHADER_DEBUG_INFO. The OpString instruction
 * ID is currently not used in OpSource or OpName instructions. glslang references it in OpSource
 * together with the shader code, but OpSource is avoided when not explicitly using
 * G_DEBUG_GPU_SHADER_DEBUG_INFO to avoid unnecessarily increasing the SPIR-V code size when it is
 * sufficient to just know which shader name corresponds to the binary.
 */
static void spirv_inject_op_string(Vector<uint32_t> &spirv, StringRef name)
{
  /* The SPIR-V header contains: Magic, Version, Generator, Bound, Schema */
  constexpr uint32_t SPIRV_HEADER_WORD_COUNT = 5u;
  constexpr uint32_t SPIRV_BOUND_INDEX = 3u;

  if (spirv.size() < SPIRV_HEADER_WORD_COUNT || spirv[0] != SpvMagicNumber) {
    return;
  }

  /* The "Bound" header entry indicates the next available result ID. */
  const uint32_t result_id = spirv[SPIRV_BOUND_INDEX];

  /* Literal string: UTF-8, null-terminated, packed 4 bytes/word, zero-padded. */
  const size_t byte_len = name.size() + 1; /* +1 for null terminator. */
  const uint32_t num_string_words = uint32_t((byte_len + 3) / 4);
  Vector<uint32_t> string_words(num_string_words, 0u);
  memcpy(string_words.data(), name.data(), name.size());

  const uint32_t word_count = 2u + num_string_words;
  Vector<uint32_t> instruction;
  instruction.append((word_count << SpvWordCountShift) | SpvOpString);
  instruction.append(result_id);
  instruction.extend(string_words);

  /* Insertion point = start of the debug section: after all OpCapability / OpExtension /
   * OpExtInstImport / OpMemoryModel / OpEntryPoint / OpExecutionMode(Id) instructions. */
  int64_t pos = SPIRV_HEADER_WORD_COUNT;
  while (pos < spirv.size()) {
    const uint32_t opcode = spirv[pos] & SpvOpCodeMask;
    const uint32_t length = spirv[pos] >> SpvWordCountShift;
    if (length == 0) {
      /* Invalid SPIR-V binary. */
      return;
    }
    const bool pre_debug = ELEM(opcode,
                                SpvOpCapability,
                                SpvOpExtension,
                                SpvOpExtInstImport,
                                SpvOpMemoryModel,
                                SpvOpEntryPoint,
                                SpvOpExecutionMode,
                                SpvOpExecutionModeId);
    if (!pre_debug) {
      break;
    }
    pos += length;
  }

  spirv.insert(pos, instruction.as_span());
  spirv[SPIRV_BOUND_INDEX] = result_id + 1u;
}

static std::string patch_line_directives(std::string source)
{
  /* Patch line directives so that we can make error reporting consistent. */
  size_t start_pos = 0;
  while ((start_pos = source.find("#line ", start_pos)) != std::string::npos) {
    source[start_pos] = '/';
    source[start_pos + 1] = '/';
  }
  return source;
}

static bool compile_ex(shaderc::Compiler &compiler,
                       VKShader &shader,
                       shaderc_shader_kind stage,
                       VKShaderModule &shader_module)
{
  std::string full_name = shader.name_get() + "_" + to_stage_name(stage);

  shader_module.original_sources = std::move(shader_module.combined_sources);

  Shader::dump_source_to_disk(
      shader.name_get(), full_name, ".glsl", shader_module.original_sources);

  if (!shader.skip_preprocessor) {
    shader_module.combined_sources = Shader::run_preprocessor(shader_module.original_sources,
                                                              G.debug & G_DEBUG_GPU_SHADER_NO_DCE);

    Shader::dump_source_to_disk(
        shader.name_get(), full_name + ".expanded", ".glsl", shader_module.combined_sources);
  }
  else {
    shader_module.combined_sources = shader_module.original_sources;
  }

  /* The cached SPIR-V binary contains an injected OpString, which depends on the shader name.
   * Include the name in the cache key so an entry is only reused for an identical result. This
   * also invalidates older caches that stored the SPIR-V without the injected OpString. */
  const std::string hash_extra = std::string("op_string:") + shader.name_get().c_str();

  if (read_spirv_from_disk(shader_module, hash_extra)) {
    return true;
  }

  shaderc::CompileOptions options;
  bool do_optimize = true;
  options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
  if (G.debug & G_DEBUG_GPU_RENDERDOC) {
    do_optimize = false;
  }
  /* WORKAROUND: Qualcomm driver can crash when handling optimized SPIR-V. */
  if (GPU_type_matches(GPU_DEVICE_QUALCOMM, GPU_OS_ANY, GPU_DRIVER_ANY)) {
    do_optimize = false;
  }
  options.SetOptimizationLevel(do_optimize ? shaderc_optimization_level_performance :
                                             shaderc_optimization_level_zero);

  /* Increase the max id bound.
   *
   * SPIR-V has a default max id bound set to 0x3fffff which is the minimum amount of ids that
   * needs to be supported by any platform. However during optimization the max id bound can
   * increase very fast and lowered at the end. As glslang uses max id bound in their internal
   * structures to allocate arrays out of bound errors can occur.
   *
   * Increasing the max id bound to a larger number to increase the internal arrays of the
   * compiler to work around the compiler crash.
   *
   * NOTE: Test-files in #144614 and #143516 would surpass the default limit during compilation.
   * The final optimized SPIR-V is far less than the default so be fine to be used on platforms
   * with minimum spec.
   *
   * https://registry.khronos.org/SPIR-V/specs/1.0/SPIRV.html#_a_id_limits_a_universal_limits
   */
  options.SetMaxIdBound(0xffffff);

  /* Should always be called after setting the optimization level. Setting optimization level
   * resets all previous passes. */
  if (G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) {
    options.SetGenerateDebugInfo();
  }

  /* Removes line directive. */
  std::string sources = patch_line_directives(shader_module.combined_sources);

  shader_module.compilation_result = compiler.CompileGlslToSpv(
      sources, stage, full_name.c_str(), options);
  bool compilation_succeeded = shader_module.compilation_result.GetCompilationStatus() ==
                               shaderc_compilation_status_success;
  if (compilation_succeeded) {
    /* Copy the compiled SPIR-V code into spirv_binary so it can be post-processed.
     * The function VKShaderModule::finalize prefers spirv_binary when it is not empty. */
    const uint32_t *begin = shader_module.compilation_result.begin();
    const uint32_t *end = shader_module.compilation_result.end();
    shader_module.spirv_binary = Vector<uint32_t>(Span<uint32_t>(begin, end - begin));
    if ((G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) == 0) {
      spirv_inject_op_string(shader_module.spirv_binary, shader.name_get());
    }
    write_spirv_to_disk(shader_module);
  }
  return compilation_succeeded;
}

bool VKShaderCompiler::compile_module(VKShader &shader,
                                      shaderc_shader_kind stage,
                                      VKShaderModule &shader_module)
{
  shaderc::Compiler compiler;
  return compile_ex(compiler, shader, stage, shader_module);
}

/** \} */

}  // namespace blender::gpu
