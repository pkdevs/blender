# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(LIBVA_PKG_ENV "PKG_CONFIG_PATH=${LIBDIR}/libdrm/lib/pkgconfig:$PKG_CONFIG_PATH")

set(LIBVA_EXTRA_OPTIONS
  -Ddriverdir=/usr/lib/x86_64-linux-gnu/dri:/usr/lib64/dri:/usr/lib/dri
  -Dwith_x11=no
  -Dwith_glx=no
  -Dwith_wayland=no
  -Dwith_win32=no
  -Denable_docs=false
)

ExternalProject_Add(external_libva
  URL file://${PACKAGE_DIR}/${LIBVA_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${LIBVA_HASH_TYPE}=${LIBVA_HASH}
  PREFIX ${BUILD_DIR}/libva

  CONFIGURE_COMMAND ${CONFIGURE_ENV} &&
    ${CMAKE_COMMAND} -E env ${LIBVA_PKG_ENV}
    ${MESON} setup
      --prefix ${LIBDIR}/libva
      --libdir lib
      ${MESON_BUILD_TYPE}
      ${LIBVA_EXTRA_OPTIONS}
      ${BUILD_DIR}/libva/src/external_libva-build
      ${BUILD_DIR}/libva/src/external_libva

  BUILD_COMMAND ninja
  INSTALL_COMMAND ninja install

  INSTALL_DIR ${LIBDIR}/libva
)

add_dependencies(
  external_libva
  external_libdrm
  external_python
  external_python_site_packages
)

harvest_rpath_lib(external_libva libva/lib ffmpeg/lib "*${SHAREDLIBEXT}*")
