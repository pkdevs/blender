/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_blender_project.hh"
#include "BKE_gtest_base.hh"
#include "BKE_main.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

class BlenderProjectTest : public bke::BlenderGTestBase {};

TEST_F(BlenderProjectTest, init_blender_project_test)
{
  Main *bmain = BKE_main_new();
  BKE_blender_project_init("My Project", "/path/to/my/project");

  BKE_blender_project_read_callback(
    bmain,
    [](const BlenderProject *project){
      ASSERT_NE(project, nullptr);
      ASSERT_EQ(project->get_name(), "My Project");
      ASSERT_EQ(project->get_root_path(), "/path/to/my/project");
    }
  );

  BKE_main_free(bmain);
}

}  // namespace blender::bke::tests
