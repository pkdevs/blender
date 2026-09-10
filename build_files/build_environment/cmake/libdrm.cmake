# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(LIBDRM_EXTRA_OPTIONS
  -Dintel=disabled
  -Dradeon=disabled
  -Damdgpu=disabled
  -Dnouveau=disabled
  -Dvmwgfx=disabled
  -Domap=disabled
  -Dexynos=disabled
  -Dfreedreno=disabled
  -Dtegra=disabled
  -Dvc4=disabled
  -Detnaviv=disabled
  -Dcairo-tests=disabled
  -Dman-pages=disabled
  -Dvalgrind=disabled
  -Dtests=false
  -Dinstall-test-programs=false
  -Dudev=false
)

ExternalProject_Add(external_libdrm
  URL file://${PACKAGE_DIR}/${LIBDRM_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${LIBDRM_HASH_TYPE}=${LIBDRM_HASH}
  PREFIX ${BUILD_DIR}/libdrm

  CONFIGURE_COMMAND ${CONFIGURE_ENV} &&
    ${MESON} setup
      --prefix ${LIBDIR}/libdrm
      --libdir lib
      --default-library static
      ${MESON_BUILD_TYPE}
      ${LIBDRM_EXTRA_OPTIONS}
      ${BUILD_DIR}/libdrm/src/external_libdrm-build
      ${BUILD_DIR}/libdrm/src/external_libdrm

  BUILD_COMMAND ninja
  INSTALL_COMMAND ninja install

  INSTALL_DIR ${LIBDIR}/libdrm
)

add_dependencies(
  external_libdrm
  external_python
  external_python_site_packages
)

harvest(external_libdrm libdrm/lib ffmpeg/lib "*.a")
