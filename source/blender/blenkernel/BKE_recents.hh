/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Recents API for persisted UI state.
 *
 * This namespace provides a small, convenient API to read and write
 * small pieces of persisted UI state (window geometry, panel flags,
 * last-used filters, etc.) organized in named "sections".
 *
 * Data is stored as TOML and split into:
 * - `recents_current` (user-modifiable values, saved to disk)
 * - `recents_default` (builtin defaults embedded in the binary)
 *
 * Lookup order (when reading a key)
 * 1. User value in `recents_current`
 * 2. Per-key default in `recents_default`
 * 3. Section-level `_default` in `recents_current` (user override)
 * 4. Section-level `_default` in `recents_default` (builtin section default)
 * 5. Final fallback: value-initialized `T{}`
 *
 * Threading / initialization
 *   Call `recents::init()` or `recents::init_async()` early in startup to
 *   populate runtime data from the built-in defaults and the on-disk file.
 *   The module will lazily ensure initialization when needed, so explicit
 *   init is optional but recommended to avoid synchronous waits.
 *
 * Typical usage
 *   // Read
 *   std::vector<float> bounds =
 *       recents::section("temp.window.dimensions")["PREFERENCES"];
 *
 *   // Write
 *   recents::section("temp.window.dimensions")["PREFERENCES"] = bounds;
 *
 *   // Explicit get/set
 *   auto s = recents::section("panels");
 *   bool is_open = s["open"];
 *   s["width"] = 12;
 *
 */

#pragma once

#include <string>

#include "BLI_string_ref.hh"

namespace blender::recents {

class Section {
  std::string section_;

 public:
  explicit Section(std::string section) : section_(std::move(section)) {}

  template<typename T> T get(const std::string &item_key) const;
  template<typename T> void set(const std::string &item_key, const T &value);

  void remove(const std::string &item_key);
  void remove_section();

  struct Proxy {
    Section *section_ptr_;
    std::string key_;

    Proxy(Section *section_p, std::string key) : section_ptr_(section_p), key_(std::move(key)) {}

    template<typename T> operator T() const
    {
      return section_ptr_->get<T>(key_);
    }

    template<typename T> Proxy &operator=(const T &value)
    {
      section_ptr_->set(key_, value);
      return *this;
    }
  };

  Proxy operator[](const std::string &item_key)
  {
    return Proxy(this, item_key);
  }
};

void init();
void init_async();
void ensure_init();
bool save();

Section section(StringRef section_name);

}  // namespace blender::recents
