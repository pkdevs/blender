/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <mutex>
#include <shared_mutex>

#include "BKE_blender_project.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"

#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.hh"

namespace blender {

/**
 * Get a reference to the global Blender project.
 *
 * As a general rule, the project's mutex should be held while accessing this to
 * prevent data races. The public APIs `BKE_blender_project_read_callback()` and
 * `BKE_blender_project_write_callback()` enforce this (if not abused) and should be
 * used where possible.
 *
 * \see get_project_mutex()
 *
 * \see BKE_blender_project_read_callback()
 *
 * \see BKE_blender_project_write_callback()
 */
static std::optional<bke::BlenderProject> &get_project()
{
  /* Construct on First Use idiom. */
  static std::optional<bke::BlenderProject> project;

  return project;
}

/**
 * Get a reference to the global Blender project's mutex.
 *
 * \see get_project()
 */
static std::shared_mutex &get_project_mutex()
{
  /* Construct on First Use idiom. */
  static std::shared_mutex project_mutex;

  return project_mutex;
}

namespace bke {

void BlenderProject::set_name(StringRef name)
{
  BLI_assert(!name.is_empty());

  this->name_ = name;

  this->is_dirty = true;
}

void BlenderProject::set_root_path(StringRef root_path)
{
  BLI_assert(!root_path.is_empty());

  this->root_path_ = root_path;

  this->is_dirty = true;
}

StringRefNull BlenderProject::get_name() const
{
  return StringRefNull(this->name_);
}

StringRefNull BlenderProject::get_root_path() const
{
  return StringRefNull(this->root_path_);
}

void with_blender_project_read_lock(FunctionRef<void()> lambda)
{
  std::shared_lock<std::shared_mutex> lock(get_project_mutex());
  lambda();
}

void with_blender_project_write_lock(FunctionRef<void()> lambda)
{
  std::unique_lock<std::shared_mutex> lock(get_project_mutex());
  lambda();
}

size_t abbreviate_variable_name(char *dst, size_t dst_max_size, StringRef name)
{
  if (name.is_empty()) {
    return 0;
  }

  auto is_alphabetic = [](char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  };

  /* The strategy for abbreviating variable names is to include each alphabetic
   * character that follows a non-alphabetic character. Additionally, the first
   * character is always unconditionally included, both in case it's alphabetic
   * and to ensure no empty abbreviations in cases like all-underscore names.
   *
   * This approach is simple, but effective for ascii snake-case variable names.
   *
   * In the future we could expand this to also handle camel-case names if
   * desired. But all built-in variables are snake-case, and hopefully most
   * user-defined variables will follow that example. */
  const size_t first_character_length = BLI_str_utf8_size_safe(name.data());
  std::memcpy(dst, name.data(), first_character_length);

  size_t dst_i = first_character_length;
  size_t name_i = first_character_length;
  while (dst_i < dst_max_size && name_i < name.size()) {
    if (!is_alphabetic(name[name_i - 1]) && is_alphabetic(name[name_i])) {
      dst[dst_i] = name[name_i];
      dst_i++;
    }
    name_i++;
  }

  return dst_i;
}

}  // namespace bke

bke::BlenderProject *BKE_blender_project_get(const Main *bmain)
{
  if (bmain == nullptr || !bmain->is_part_of_project) {
    return nullptr;
  }

  std::optional<bke::BlenderProject> &project = get_project();
  if (!project.has_value()) {
    return nullptr;
  }

  return &*project;
}

bool BKE_blender_project_init(blender::StringRef name, blender::StringRef root_path)
{
  if (name.is_empty() || root_path.is_empty()) {
    return false;
  }

  BKE_blender_project_clear();

  bke::with_blender_project_write_lock([&] {
    std::optional<bke::BlenderProject> &project = get_project();

    project = blender::bke::BlenderProject();

    project->set_name(name);
    project->set_root_path(root_path);
  });

  return true;
}

void BKE_blender_project_clear()
{
  /* At the moment this function is quite anemic, and doesn't really justify
   * being a separate function. However, as future milestones like
   * project-specific addons and asset libraries are added, this will collect in
   * one place the code for ensuring those things are properly unloaded when the
   * active project is cleared. */

  bke::with_blender_project_write_lock([&] {
    std::optional<bke::BlenderProject> &project = get_project();

    project = std::nullopt;
  });
}

}  // namespace blender
