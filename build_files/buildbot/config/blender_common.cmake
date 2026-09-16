# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

include("${CMAKE_CURRENT_LIST_DIR}/blender_version.cmake")

# Preprocess variables so that they match what the configuration expects.

string(REGEX MATCH "^[0-9]+\\.[0-9]+" CUDA12_FOLDER_VERSION "${CUDA12_VERSION}")
string(REGEX MATCH "^[0-9]+\\.[0-9]+" CUDA13_FOLDER_VERSION "${CUDA13_VERSION}")

# Utility functions used by the platform-specific buildbot configuration.

function(buildbot_assert_path_exists path message)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "${message}")
  endif()
endfunction()
