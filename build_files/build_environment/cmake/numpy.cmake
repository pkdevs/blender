# SPDX-FileCopyrightText: 2017-2022 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

if(MSVC)
  message(STATUS "BIN >${PYTHON_BINARY}<")
  if(BUILD_MODE STREQUAL Debug)
    set(NUMPY_DIR_POSTFIX -pydebug)
    set(NUMPY_ARCHIVE_POSTFIX d)
    set(NUMPY_BUILD_OPTION --debug)
  else()
    set(NUMPY_DIR_POSTFIX)
    set(NUMPY_ARCHIVE_POSTFIX)
    set(NUMPY_BUILD_OPTION)
  endif()
endif()

set(NUMPY_POSTFIX)

if(WITH_APPLE_CROSSPLATFORM)
  # Code generators run on the Mac; extension modules use the target SDK and Python headers.
  configure_file(${CMAKE_CURRENT_LIST_DIR}/numpy_apple_cross.ini.in
    ${BUILD_DIR}/numpy-cross.ini @ONLY)
  configure_file(${CMAKE_CURRENT_LIST_DIR}/numpy_apple_native.ini.in
    ${BUILD_DIR}/numpy-native.ini @ONLY)
  set(NUMPY_MESON ${PYTHON_BINARY} <SOURCE_DIR>/vendored-meson/meson/meson.py)
  ExternalProject_Add(external_numpy
    URL file://${PACKAGE_DIR}/${NUMPY_FILE}
    DOWNLOAD_DIR ${DOWNLOAD_DIR}
    URL_HASH ${NUMPY_HASH_TYPE}=${NUMPY_HASH}
    PREFIX ${BUILD_DIR}/numpy
    CONFIGURE_COMMAND ${NUMPY_MESON} setup <BINARY_DIR> <SOURCE_DIR>
      --cross-file=${BUILD_DIR}/numpy-cross.ini
      --native-file=${BUILD_DIR}/numpy-native.ini
      --prefix=${LIBDIR}/python
      --buildtype=release
      -Dblas=none -Dlapack=none
      -Dpython.platlibdir=lib/python${PYTHON_SHORT_VERSION}/site-packages
      -Dpython.purelibdir=lib/python${PYTHON_SHORT_VERSION}/site-packages
    BUILD_COMMAND ${NUMPY_MESON} compile -C <BINARY_DIR> -j ${MAKE_THREADS}
    INSTALL_COMMAND ${NUMPY_MESON} install -C <BINARY_DIR> --no-rebuild
  )
  add_dependencies(external_numpy external_python)
  harvest(external_numpy python/lib python/lib "*")
  return()
endif()

ExternalProject_Add(external_numpy
  URL file://${PACKAGE_DIR}/${NUMPY_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${NUMPY_HASH_TYPE}=${NUMPY_HASH}
  PREFIX ${BUILD_DIR}/numpy
  PATCH_COMMAND ${NUMPY_PATCH}
  CONFIGURE_COMMAND ""
  LOG_BUILD 1
  BUILD_IN_SOURCE 1

  BUILD_COMMAND
    ${PYTHON_BINARY} setup.py
      build ${NUMPY_BUILD_OPTION} -j${PYTHON_MAKE_THREADS}
      install
      --old-and-unmanageable

  INSTALL_COMMAND ""
)

add_dependencies(
  external_numpy
  external_python
  external_python_site_packages
  external_cython
)
