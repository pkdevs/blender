/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_ustring.hh"

#include "BLI_concurrent_set.hh"

namespace blender {

using StringTable = ConcurrentSet<blender::detail::UStringEntry>;
static StringTable &global_string_table()
{
  static StringTable table = []() {
    StringTable table;
    /* Slightly larger than the number used for a default startup. */
    table.reserve(16384);
    return table;
  }();
  return table;
}

namespace detail {

const blender::detail::UStringEntry *ustring_ensure_entry(const StringRef str)
{
  const uint64_t hash = get_default_hash(str);
  StringTable &table = global_string_table();
  if (const blender::detail::UStringEntry *entry = table.lookup_ptr(
          detail::StrWithHash{str, hash}))
  {
    return entry;
  }
  return &table.add_get(blender::detail::UStringEntry{std::string(str), hash});
}

const blender::detail::UStringEntry *ustring_ensure_entry(const char *str)
{
  return ustring_ensure_entry(StringRef(str));
}

}  // namespace detail

}  // namespace blender
