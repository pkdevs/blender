/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string>

#include "BLI_vector.hh"

namespace blender::dna {

struct ParsedMember {
  /** Single-identifier type name, e.g. `int`, `float`, `Material`. */
  std::string type_name;
  /** Canonical full member name, e.g. `*var`, `arr[4]`, `(*func)()`. */
  std::string member_name;
};

struct ParsedStruct {
  std::string type_name;
  Vector<ParsedMember> members;
};

/** Extract structs and their members from a DNA header. */
bool parse_dna_header(const std::string &filepath, Vector<ParsedStruct> &r_structs);

}  // namespace blender::dna
