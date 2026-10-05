# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Keep framebuffer-fetch emulation in the replacement Metal shader generator."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class MetalSubpassFallbackTest(unittest.TestCase):
    def test_native_and_texture_backed_subpass_inputs(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        root = Path(__file__).resolve().parents[2]
        source = (root / "source/blender/gpu/metal/mtl_shader_generate.cc").read_text()
        start = source.index("struct Separator {")
        streams = source[start:source.index("static bool is_builtin_type", start)]
        start = source.index("static void generate_subpass_inputs(")
        function = source[start:source.index("\n}\n", start) + 3]
        harness = r'''
#include <cassert>
#include <sstream>
#include <string>
#include <vector>
#define ELEM(a, b, c, d) ((a) == (b) || (a) == (c) || (a) == (d))
using StringRefNull = std::string;
enum class ShaderStage { FRAGMENT };
enum BuiltinBits { LAYER = 1 };
enum class ImageType { Float2D, Int2D, Uint2D, Float2DArray, Int2DArray, Uint2DArray };
struct Type { int components; };
static std::ostream &operator<<(std::ostream &out, Type type) {
  out << "float";
  if (type.components > 1) out << type.components;
  return out;
}
static int to_component_count(Type type) { return type.components; }
template<typename T> struct Vector : std::vector<T> {
  bool is_empty() const { return this->empty(); }
};
struct ShaderCreateInfo {
  struct SubpassIn { int index; Type type; ImageType img_type; std::string name; int raster_order_group; };
  Vector<SubpassIn> subpass_inputs_;
  int builtins_ = 0;
};
struct Capabilities { bool supports_native_tile_inputs = false; };
struct MTLBackend {
  static Capabilities &get_capabilities() { static Capabilities value; return value; }
};
static std::string wrap_type(const std::string &name, ShaderStage) { return name; }
''' + streams + function + r'''
int main() {
  for (bool native : {false, true}) {
    MTLBackend::get_capabilities().supports_native_tile_inputs = native;
    for (int components = 1; components <= 4; ++components) {
      for (bool array : {false, true}) {
        for (bool layered : {false, true}) {
          ShaderCreateInfo info;
          info.builtins_ = layered ? LAYER : 0;
          info.subpass_inputs_.push_back({2, {components},
              array ? ImageType::Float2DArray : ImageType::Float2D, "input_color", 0});
          GeneratedStreams streams;
          generate_subpass_inputs(streams, info);
          const std::string members = streams.wrapper_class_members.str();
          const std::string start = streams.entry_point_start.str();
          if (native) {
            assert(members.find("[[color(2)]]") != std::string::npos);
            assert(members.find("[[raster_order_group(0)]]") != std::string::npos);
            assert(start.empty());
          } else {
            assert(members.find("[[color(") == std::string::npos);
            const std::string coords = "uint2(mtl_vert_out.gl_FragCoord.xy)" +
                (array ? std::string(", ") + (layered ? "mtl_vert_out.gpu_Layer" : "0") : "");
            const std::string read = "mtl_subpass_image_2.read(" + coords + ")." +
                std::string("xyzw").substr(0, components);
            assert(start.find(read) != std::string::npos);
            assert(streams.wrapper_instance_init.str().find("mtl_subpass_image_2_value") !=
                   std::string::npos);
          }
        }
      }
    }
    GeneratedStreams streams;
    generate_subpass_inputs(streams, {});
    assert(streams.wrapper_class_members.str().empty());
  }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            code = Path(directory) / "subpass.cc"
            executable = Path(directory) / "subpass"
            code.write_text(harness)
            result = subprocess.run([compiler, "-std=c++20", str(code), "-o", str(executable)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
