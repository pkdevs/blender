# SPDX-FileCopyrightText: 2026 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

add_library(extern_nanoflann INTERFACE)
target_include_directories(extern_nanoflann SYSTEM INTERFACE "include")
add_library(bf::extern::nanoflann ALIAS extern_nanoflann)
