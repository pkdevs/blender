/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "processor.hh"

using namespace blender::gpu::shader;

static std::vector<std::string> list_files(const std::string &dir)
{
  std::vector<std::string> files;
  for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::path(dir))) {
    if (entry.is_regular_file()) {
      std::string filename(entry.path().string());
      /* We only allow including header files or shader files. */
      if (filename.find(".hh") != std::string::npos ||
          filename.find(".msl") != std::string::npos ||
          filename.find(".glsl") != std::string::npos)
      {
        files.push_back(filename);
      }
    }
  }
  return files;
}

static metadata::Source scan_external_symbols(const std::vector<std::string> &file_list,
                                              std::vector<std::string> &visited_files,
                                              const std::string &file_buffer,
                                              const std::string &file_name)
{
  Language language = language_from_filename(file_name);
  SourceProcessor processor(file_buffer, file_name, language, file_list);

  metadata::Source include_data = processor.parse_include_and_symbols();

  bool errors = false;

  for (const auto &dep : include_data.dependencies) {
    std::string file;
    for (const auto &filename : file_list) {
      if (filename.find(dep) != std::string::npos) {
        file = filename;
      }
    }

    if (file.empty()) {
      std::cout << "Error: Included file not found " << dep << std::endl;
      errors = true;
    }
    else if (std::find(visited_files.begin(), visited_files.end(), file) == visited_files.end()) {
      visited_files.emplace_back(file);

      std::ifstream input_file(file);
      if (!input_file) {
        std::cerr << "Error: Could not open file " << file << std::endl;
        errors = true;
      }
      else {
        std::stringstream buffer;
        buffer << input_file.rdbuf();

        metadata::Source source = scan_external_symbols(
            file_list, visited_files, buffer.str(), file);

        /* Extend list. */
        include_data.symbol_table.insert(include_data.symbol_table.end(),
                                         source.symbol_table.begin(),
                                         source.symbol_table.end());
        include_data.template_definitions.insert(include_data.template_definitions.end(),
                                                 source.template_definitions.begin(),
                                                 source.template_definitions.end());
      }
    }
  }

  if (errors) {
    exit(1);
  }
  return include_data;
}

int main(int argc, char **argv)
{
  using namespace blender;
  if (argc < 6) {
    std::cerr << "Usage: shader_tool <data_file_from> <data_file_to> <metadata_file_to> "
                 "<infos_file_to> <dep_file_to> <include_dir1> <include_dir2> ..."
              << std::endl;
    exit(1);
  }

  const char *input_file_name = argv[1];
  const char *output_file_name = argv[2];
  const char *metadata_file_name = argv[3];
  const char *infos_file_name = argv[4];
  const char *dep_file_name = argv[5];

  /* Open the input file for reading */
  std::ifstream input_file(input_file_name);
  if (!input_file) {
    std::cerr << "Error: Could not open input file " << input_file_name << std::endl;
    exit(1);
  }

  /* We make the required directories here rather than having the build system
   * do the work for us, as having cmake do it leads to several thousand cmake
   * instances being launched, leading to significant overhead, see pr #141404
   * for details. */
  std::filesystem::path parent_dir = std::filesystem::path(output_file_name).parent_path();
  std::error_code ec;
  if (!std::filesystem::create_directories(parent_dir, ec)) {
    if (ec) {
      std::cerr << "Unable to create " << parent_dir << " : " << ec.message() << std::endl;
      exit(1);
    }
  }

  /* Open the output file for writing */
  std::ofstream output_file(output_file_name, std::ofstream::out | std::ofstream::binary);
  if (!output_file) {
    std::cerr << "Error: Could not open output file " << output_file_name << std::endl;
    input_file.close();
    exit(1);
  }

  /* Open the output file for writing */
  std::ofstream metadata_file(metadata_file_name, std::ofstream::out | std::ofstream::binary);
  if (!metadata_file) {
    std::cerr << "Error: Could not open output file " << metadata_file_name << std::endl;
    input_file.close();
    exit(1);
  }

  /* Open the output file for writing */
  std::ofstream infos_file(infos_file_name, std::ofstream::out | std::ofstream::binary);
  if (!infos_file) {
    std::cerr << "Error: Could not open output file " << infos_file_name << std::endl;
    input_file.close();
    exit(1);
  }

  /* Open the output file for writing */
  std::ofstream dep_file(dep_file_name, std::ofstream::out | std::ofstream::binary);
  if (!dep_file) {
    std::cerr << "Error: Could not open output file " << dep_file_name << std::endl;
    input_file.close();
    exit(1);
  }

  /* List of files available for include. */
  std::vector<std::string> file_list;
  for (int i = 6; i < argc; i++) {
    auto list = list_files(std::string(argv[i]));
    /* Extend list. */
    file_list.insert(file_list.end(), list.begin(), list.end());
  }

  std::stringstream buffer;
  buffer << input_file.rdbuf();

  std::string filename(input_file_name);
  const bool is_info = filename.ends_with("infos.hh") || filename.ends_with(".bsl.hh");

  using namespace gpu::shader;

  Language language = language_from_filename(filename);

  if (language == Language::GLSL) {
    /* All build-time GLSL files should be considered blender-GLSL. */
    language = Language::BLENDER_GLSL;
  }

  if (language == Language::BSL) {
    /* Until the new BSL compiler is fully working, use the legacy one. */
    if (!filename.ends_with("gpu_shader_2D_widget_base.bsl.hh") &&
        !filename.ends_with("gpu_shader_2D_update_mipmaps.bsl.hh") &&
        !filename.ends_with("eevee_bxdf_lut_lib.bsl.hh") &&
        !filename.ends_with("eevee_hiz_update.bsl.hh") &&
        !filename.ends_with("eevee_reverse_z_lib.bsl.hh") &&
        !filename.ends_with("eevee_shadow_page_defrag.bsl.hh") &&
        !filename.ends_with("eevee_surfel.bsl.hh") &&
        !filename.ends_with("eevee_surfel_cluster.bsl.hh") &&
        !filename.ends_with("eevee_surfel_list.bsl.hh") &&
        !filename.ends_with("eevee_surfel_ray.bsl.hh") &&
        !filename.ends_with("eevee_surfel_light.bsl.hh") &&
        !filename.ends_with("eevee_bxdf.bsl.hh") &&
        !filename.ends_with("eevee_bxdf_diffuse.bsl.hh") &&
        !filename.ends_with("eevee_bxdf_lut.bsl.hh") &&
        !filename.ends_with("eevee_bxdf_microfacet.bsl.hh") &&
        !filename.ends_with("eevee_bxdf_oren_nayar.bsl.hh") &&
        !filename.ends_with("eevee_bxdf_types.bsl.hh") &&
        !filename.ends_with("eevee_cryptomatte.bsl.hh") &&
        !filename.ends_with("eevee_cryptomatte_sort.bsl.hh") &&
        !filename.ends_with("eevee_camera_lib.bsl.hh") &&
        !filename.ends_with("eevee_closure.bsl.hh") &&
        !filename.ends_with("eevee_colorspace_lib.bsl.hh") &&
        !filename.ends_with("eevee_depth_of_field_accumulator.bsl.hh") &&
        !filename.ends_with("eevee_depth_of_field_bokeh_lut.bsl.hh") &&
        !filename.ends_with("eevee_depth_of_field_filter.bsl.hh") &&
        !filename.ends_with("eevee_depth_of_field_gather.bsl.hh") &&
        !filename.ends_with("eevee_depth_of_field_lib.bsl.hh") &&
        !filename.ends_with("eevee_depth_of_field_resolve.bsl.hh") &&
        !filename.ends_with("eevee_depth_of_field_scatter.bsl.hh") &&
        !filename.ends_with("eevee_depth_of_field_setup.bsl.hh") &&
        !filename.ends_with("eevee_depth_of_field_tiles.bsl.hh") &&
        !filename.ends_with("eevee_fast_gi.bsl.hh") && !filename.ends_with("eevee_film.bsl.hh") &&
        !filename.ends_with("eevee_hiz.bsl.hh") && !filename.ends_with("eevee_filter.bsl.hh") &&
        !filename.ends_with("eevee_lightprobe.bsl.hh") &&
        !filename.ends_with("eevee_lightprobe_display.bsl.hh") &&
        !filename.ends_with("eevee_lightprobe_plane.bsl.hh") &&
        !filename.ends_with("eevee_lightprobe_sphere.bsl.hh") &&
        // !filename.ends_with("eevee_lightprobe_sphere_bake.bsl.hh") &&
        !filename.ends_with("eevee_lightprobe_sphere_culling.bsl.hh") &&
        !filename.ends_with("eevee_lightprobe_volume.bsl.hh") &&
        !filename.ends_with("eevee_lightprobe_volume_bake.bsl.hh") &&
        !filename.ends_with("eevee_lightprobe_volume_load.bsl.hh") &&
        !filename.ends_with("eevee_lookdev_copy_world.bsl.hh") &&
        !filename.ends_with("eevee_lookdev_display.bsl.hh") &&
        !filename.ends_with("eevee_ltc_lib.bsl.hh") &&
        !filename.ends_with("eevee_ltc_lut_lib.bsl.hh") &&
        !filename.ends_with("eevee_motion_blur.bsl.hh") &&
        !filename.ends_with("eevee_occupancy_convert.bsl.hh") &&
        !filename.ends_with("eevee_occupancy_lib.bsl.hh") &&
        !filename.ends_with("eevee_octahedron_lib.bsl.hh") &&
        !filename.ends_with("eevee_ray_denoise.bsl.hh") &&
        !filename.ends_with("eevee_ray_generate.bsl.hh") &&
        !filename.ends_with("eevee_ray_tile.bsl.hh") &&
        !filename.ends_with("eevee_ray_trace.bsl.hh") &&
        !filename.ends_with("eevee_ray_trace_screen_lib.bsl.hh") &&
        !filename.ends_with("eevee_ray_types_lib.bsl.hh") &&
        !filename.ends_with("eevee_renderpass.bsl.hh") &&
        !filename.ends_with("eevee_renderpass_clear.bsl.hh") &&
        !filename.ends_with("eevee_sampling_lib.bsl.hh") &&
        !filename.ends_with("eevee_spherical_harmonics.bsl.hh") &&
        !filename.ends_with("eevee_subsurface.bsl.hh") &&
        !filename.ends_with("eevee_subsurface_lib.bsl.hh") &&
        !filename.ends_with("eevee_thickness_lib.bsl.hh") &&
        !filename.ends_with("eevee_transparency.bsl.hh") &&
        !filename.ends_with("eevee_uniform.bsl.hh") &&
        !filename.ends_with("eevee_utility_tx.bsl.hh") &&
        !filename.ends_with("eevee_velocity.bsl.hh") &&
        !filename.ends_with("eevee_volume_lib.bsl.hh") &&
        // !filename.ends_with("eevee_test_fast_gi.bsl.hh") &&
        // !filename.ends_with("eevee_test_gbuffer_closure.bsl.hh") &&
        // !filename.ends_with("eevee_test_gbuffer_normal.bsl.hh") &&
        // !filename.ends_with("eevee_test_occupancy.bsl.hh") &&
        // !filename.ends_with("eevee_test_shadow.bsl.hh") &&
        // !filename.ends_with("eevee_volume.bsl.hh") &&
        // !filename.ends_with("eevee_light_shadow_setup.bsl.hh") &&
        // !filename.ends_with("eevee_debug.bsl.hh") &&
        // !filename.ends_with("eevee_deferred_combine.bsl.hh") &&
        // !filename.ends_with("eevee_deferred_eval.bsl.hh") &&
        // !filename.ends_with("eevee_deferred_thickness_amend.bsl.hh") &&
        // !filename.ends_with("eevee_deferred_tile_classify.bsl.hh") &&
        // !filename.ends_with("eevee_forward_lib.bsl.hh") &&
        // !filename.ends_with("eevee_forward_resolve.bsl.hh") &&
        !filename.ends_with("eevee_gbuffer_read.bsl.hh") &&
        !filename.ends_with("eevee_gbuffer_types.bsl.hh") &&
        !filename.ends_with("eevee_gbuffer_write.bsl.hh") &&
        // !filename.ends_with("eevee_geom_curves.bsl.hh") &&
        // !filename.ends_with("eevee_geom_interp.bsl.hh") &&
        // !filename.ends_with("eevee_geom_mesh.bsl.hh") &&
        // !filename.ends_with("eevee_geom_pointcloud.bsl.hh") &&
        // !filename.ends_with("eevee_geom_types_lib.bsl.hh") &&
        // !filename.ends_with("eevee_geom_volume.bsl.hh") &&
        // !filename.ends_with("eevee_geom_world.bsl.hh") &&
        // !filename.ends_with("eevee_light_culling.bsl.hh") && /* ::intersect error */
        // !filename.ends_with("eevee_light_data.bsl.hh") &&
        // !filename.ends_with("eevee_light_eval.bsl.hh") &&
        // !filename.ends_with("eevee_light_iter.bsl.hh") &&
        // !filename.ends_with("eevee_light_lib.bsl.hh") &&
        // !filename.ends_with("eevee_light_shape_display.bsl.hh") &&
        // !filename.ends_with("eevee_material_variants.bsl.hh") &&
        // !filename.ends_with("eevee_nodetree_lib.bsl.hh") &&
        // !filename.ends_with("eevee_pipeline.bsl.hh") &&
        // !filename.ends_with("eevee_shadow.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_page_allocate.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_page_clear.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_page_free.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_page_mask.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_page_ops.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_tag_update.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_tag_usage.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_tag_usage_transparent.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_tilemap_amend.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_tilemap_bounds.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_tilemap_finalize.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_tilemap_init.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_tilemap_lib.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_tracing.bsl.hh") &&
        // !filename.ends_with("eevee_shadow_visibility.bsl.hh") &&
        // !filename.ends_with("eevee_surf_capture.bsl.hh") &&
        // !filename.ends_with("eevee_surf_deferred.bsl.hh") &&
        // !filename.ends_with("eevee_surf_depth.bsl.hh") &&
        // !filename.ends_with("eevee_surf_forward.bsl.hh") &&
        // !filename.ends_with("eevee_surf_hybrid.bsl.hh") &&
        // !filename.ends_with("eevee_surf_occupancy.bsl.hh") &&
        // !filename.ends_with("eevee_surf_shadow.bsl.hh") &&
        // !filename.ends_with("eevee_surf_volume.bsl.hh") &&
        // !filename.ends_with("eevee_surf_world.bsl.hh") &&
        // !filename.ends_with("eevee_surf_common.bsl.hh") &&
        true)
    {
      language = Language::BLENDER_GLSL;
    }
  }

  metadata::Source external_symbols;
  std::vector<std::string> visited_files{input_file_name};
  if (language == Language::BLENDER_GLSL) {
    external_symbols = scan_external_symbols(file_list, visited_files, buffer.str(), filename);
  }

  /* Escape path according to the depfile syntax. */
  auto escape_path = [](std::string filepath) {
    size_t pos = 0;
    while ((pos = filepath.find(' ', pos)) != std::string::npos) {
      filepath.replace(pos, 1, "\\ ");
      pos += 2;
    }
    return filepath;
  };

  dep_file << output_file_name << " : ";
  for (const auto &file : visited_files) {
    dep_file << escape_path(file) << " ";
  }
  dep_file << "\n";

  SourceProcessor processor(buffer.str(), input_file_name, language, file_list);

  auto [result, metadata, error] = processor.convert(external_symbols);

  output_file << result;

  /* TODO(fclem): Don't use regex for that. */
  size_t last_slash = filename.find_last_of('/');
  std::string name = (last_slash == std::string::npos) ? filename :
                                                         filename.substr(last_slash + 1);
  std::string metadata_function_name = "metadata_" + name + "_tmp";
  std::ranges::replace(metadata_function_name, '.', '_');

  metadata_file << metadata.serialize(metadata_function_name);
  if (is_info) {
    infos_file << metadata.serialize_infos();
  }

  input_file.close();
  output_file.close();
  metadata_file.close();
  infos_file.close();
  dep_file.close();

  if (error) {
    std::cerr << error.value().full_report << std::endl;
  }

  return error.has_value() ? 1 : 0;
}
