# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: BSD-3-Clause

# - Find ThorVG library
# Find the native ThorVG includes and libraries
# This module defines
#  ThorVG_INCLUDE_DIRS, where to find ThorVG headers, Set when
#                        ThorVG_INCLUDE_DIR is found.
#  ThorVG_LIBRARIES, libraries to link against to use ThorVG.
#  ThorVG_ROOT_DIR, The base directory to search for ThorVG.
#                    This can also be an environment variable.
#  ThorVG_FOUND, If false, do not try to use ThorVG.
#

# If `ThorVG_ROOT_DIR` was defined in the environment, use it.
if(DEFINED ThorVG_ROOT_DIR)
  # Pass.
elseif(DEFINED ENV{ThorVG_ROOT_DIR})
  set(ThorVG_ROOT_DIR $ENV{ThorVG_ROOT_DIR})
else()
  set(ThorVG_ROOT_DIR "")
endif()

set(_ThorVG_SEARCH_DIRS
  ${ThorVG_ROOT_DIR}
  /opt/lib/ThorVG
)

find_path(ThorVG_INCLUDE_DIR
  NAMES
    thorvg.h
  HINTS
    ${_ThorVG_SEARCH_DIRS}
  PATH_SUFFIXES
    include include/thorvg-1
)

find_library(ThorVG_LIBRARY
  NAMES
    thorvg-1
    thorvg
  HINTS
    ${_ThorVG_SEARCH_DIRS}
  PATH_SUFFIXES
    lib64 lib lib/static
)

# handle the QUIETLY and REQUIRED arguments and set ThorVG_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ThorVG DEFAULT_MSG ThorVG_LIBRARY ThorVG_INCLUDE_DIR)

if(ThorVG_FOUND)
  set(ThorVG_LIBRARIES ${ThorVG_LIBRARY})
  set(ThorVG_INCLUDE_DIRS ${ThorVG_INCLUDE_DIR})
endif()

mark_as_advanced(
  ThorVG_INCLUDE_DIR
  ThorVG_LIBRARY
)

unset(_ThorVG_SEARCH_DIRS)