/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <string>

namespace blender {
struct Main;

bool BKE_blender_project_config_file_write(Main *bmain);
bool BKE_blender_project_init_from_root_path(Main *bmain, std::string project_root_path);

}  // namespace blender
