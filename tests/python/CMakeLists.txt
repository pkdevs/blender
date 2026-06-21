# SPDX-FileCopyrightText: 2011-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Use '--write-blend=/tmp/test.blend' to view output

set(TEST_SRC_DIR ${CMAKE_SOURCE_DIR}/tests/files)
set(TEST_PYTHON_DIR ${CMAKE_SOURCE_DIR}/tests/python)
set(TEST_OUT_DIR ${CMAKE_BINARY_DIR}/tests)

# ugh, any better way to do this on testing only?
file(MAKE_DIRECTORY ${TEST_OUT_DIR})
file(MAKE_DIRECTORY ${TEST_OUT_DIR}/io_tests)
file(MAKE_DIRECTORY ${TEST_OUT_DIR}/blendfile_io)

# Check which tests can be enabled.
if(IS_DIRECTORY ${TEST_SRC_DIR}/render)
  set(TEST_SRC_DIR_EXISTS TRUE)

  if(OPENIMAGEIO_TOOL)
    set(TEST_OPENIMAGEIO_TOOL_EXISTS TRUE)
  else()
    set(TEST_OPENIMAGEIO_TOOL_EXISTS FALSE)
    message(STATUS "Tests: Disabling render tests, missing oiiotool")
  endif()
else()
  set(TEST_SRC_DIR_EXISTS FALSE)
  message(STATUS "Tests: Disabling most tests, no test data in ${TEST_SRC_DIR}")
endif()

if(WITH_SYSTEM_PYTHON_TESTS)
  if(NOT EXISTS "${TEST_SYSTEM_PYTHON_EXE}")
    message(STATUS
      "Tests: Disabling 'System Python', no valid system python path, set TEST_SYSTEM_PYTHON_EXE."
    )
    set(WITH_SYSTEM_PYTHON_TESTS OFF)
  endif()
endif()

# Run Blender command with parameters.
function(add_blender_test_impl testname envvars_list exe)
  add_test(
    NAME ${testname}
    COMMAND ${exe} ${ARGN}
  )
  blender_test_set_envvars("${testname}" "${envvars_list}")
endfunction()

function(add_blender_test testname)
  add_blender_test_impl(
    "${testname}"
    ""
    "${TEST_BLENDER_EXE}"
    ${TEST_BLENDER_EXE_PARAMS}
    "${TEST_BLENDER_EXE_PARAMS_NO_THUMB}"
    ${ARGN}
  )
endfunction()

function(add_blender_test_allow_error testname)
  # Remove `--debug-exit-on-error` since errors are printed on e.g. meshes with
  # invalid data or failed image loading, but sometimes we want to test those.
  set(EXE_PARAMS ${TEST_BLENDER_EXE_PARAMS})
  list(REMOVE_ITEM EXE_PARAMS --debug-exit-on-error)
  add_blender_test_impl(
    "${testname}"
    ""
    "${TEST_BLENDER_EXE}"
    ${EXE_PARAMS}
    "${TEST_BLENDER_EXE_PARAMS_NO_THUMB}"
    ${ARGN}
  )
endfunction()

if(WITH_UI_TESTS)
  set(_blender_headless_env_vars "BLENDER_BIN=${TEST_BLENDER_EXE}")
  if(WITH_UI_TESTS_HEADLESS)

    # Currently only WAYLAND is supported, support for others may be added later.
    # In this case none of the WESTON environment variables will be used.
    if(WITH_GHOST_WAYLAND)
      set(_weston_bin_in_libdir OFF)
      if(DEFINED LIBDIR)
        set(_weston_bin_default "${LIBDIR}/wayland_weston/bin/weston")
      else()
        set(_weston_bin_default "weston")
      endif()
      set(WESTON_BIN "${_weston_bin_default}" CACHE STRING "\
  The location of weston, leave blank for the default location."
      )
      mark_as_advanced(WESTON_BIN)
      if((DEFINED LIBDIR) AND ("${WESTON_BIN}" STREQUAL "${_weston_bin_default}"))
        set(_weston_bin_in_libdir ON)
      endif()

      list(APPEND _blender_headless_env_vars
        "WESTON_BIN=${WESTON_BIN}"
      )

      if(_weston_bin_in_libdir)
        list(APPEND _blender_headless_env_vars
          "WAYLAND_ROOT_DIR=${LIBDIR}/wayland"
          "WESTON_ROOT_DIR=${LIBDIR}/wayland_weston"
        )
      endif()
    endif()
  else()
    list(APPEND _blender_headless_env_vars
      "PASS_THROUGH=1"
    )
  endif()
  function(add_blender_test_ui_backend testname backend)
    # Remove `--background`
    set(EXE_PARAMS ${TEST_BLENDER_EXE_PARAMS})
    list(REMOVE_ITEM EXE_PARAMS --background)
    add_blender_test_impl(
      "${testname}"
      "${_blender_headless_env_vars}"
      "${TEST_PYTHON_EXE}"
      "${CMAKE_SOURCE_DIR}/tests/utils/blender_headless.py"
      # NOTE: attempting to maximize the window causes problems with a headless `weston`,
      # while this could be investigated, use windowed mode instead.
      # Use a window size that balances software GPU rendering with enough room to use the UI.
      --factory-startup
      # Workaround for https://projects.blender.org/blender/blender/issues/154770.
      --no-native-pixels
      # No need for window frame with tests.
      --no-window-frame
      -p 0 0 800 600
      --gpu-backend ${backend}
      # Log operator calls at a debug level so it is easier to debug runs on the buildbot that fail
      --log "operator" --log-level "debug"
      "${EXE_PARAMS}"
      "${TEST_BLENDER_EXE_PARAMS_NO_THUMB}"
      "${ARGN}"
    )
  endfunction()
  function(add_blender_test_ui testname)
    if(WITH_OPENGL_BACKEND)
      add_blender_test_ui_backend(
        "ui_opengl_${testname}"
        "opengl"
        "${ARGN}"
      )
    endif()
    if(WITH_METAL_BACKEND)
      add_blender_test_ui_backend(
        "ui_metal_${testname}"
        "metal"
        "${ARGN}"
      )
    endif()
    if(WITH_VULKAN_BACKEND AND WITH_GPU_RENDER_TESTS_VULKAN AND WITH_UI_TESTS_VULKAN)
      add_blender_test_ui_backend(
        "ui_vulkan_${testname}"
        "vulkan"
        "${ARGN}"
      )
    endif()
  endfunction()
endif()

# Run Python script outside Blender.
function(add_python_test testname testscript)
  if(NOT TEST_PYTHON_EXE)
    message(FATAL_ERROR "No Python configured for running tests, set TEST_PYTHON_EXE.")
  endif()

  add_test(
    NAME ${testname}
    COMMAND ${TEST_PYTHON_EXE} ${TEST_PYTHON_EXE_EXTRA_ARGS} ${testscript} ${ARGN}
    WORKING_DIRECTORY $<TARGET_FILE_DIR:blender>
  )
  blender_test_set_envvars("${testname}" "")
endfunction()

# Run Python render test.
function(add_render_test testname testscript)
  if(TEST_OPENIMAGEIO_TOOL_EXISTS)
    set(_args ${ARGN} --blender "${TEST_BLENDER_EXE}" --oiiotool "${OPENIMAGEIO_TOOL}")
    if(WITH_TESTS_BATCHED)
      list(APPEND _args --batch)
    endif()
    add_python_test(${testname} ${testscript} ${_args})
  endif()
endfunction()

# Run Python script outside Blender, using system default Python3 interpreter,
# NOT the one specified in `TEST_PYTHON_EXE`.
function(add_system_python_test testname testscript)
  if(NOT WITH_SYSTEM_PYTHON_TESTS)
    return()
  endif()

  add_test(
    NAME ${testname}
    COMMAND ${TEST_SYSTEM_PYTHON_EXE} ${testscript} ${ARGN}
    WORKING_DIRECTORY $<TARGET_FILE_DIR:blender>
  )
  blender_test_set_envvars("${testname}" "")
endfunction()

# RENDER TESTS
# ------------------------------------------------------------------------------

if((WITH_CYCLES OR WITH_GPU_RENDER_TESTS) AND TEST_SRC_DIR_EXISTS)
  set(render_tests
    attributes
    camera
    colorspace
    bsdf
    gsplat
    hair
    image_colorspace
    image_data_types
    image_mapping
    image_mipmap
    image_texture_limit
    integrator
    light
    light_group
    light_linking
    lightprobe
    mesh
    node_inlining
    pointcloud
    principled_bsdf
    raycast
    shader
  )

  set(workbench_tests
    workbench
    camera
    colorspace
    hair
    image_colorspace
    image_data_types
    image_mapping
    image_texture_limit
    mesh
    pointcloud
    texture
  )

  if(WITH_OPENSUBDIV)
    list(APPEND render_tests displacement)
  endif()

  if(WITH_FREESTYLE)
    list(APPEND render_tests render_layer)
    list(APPEND workbench_tests render_layer)
  endif()

  if(WITH_MOD_FLUID)
    list(APPEND render_tests motion_blur reports volume)
    list(APPEND workbench_tests volume)
  endif()

  if(WITH_OPENVDB AND WITH_MOD_FLUID)
    # Some tests in the OpenVDB folder make use of fluid simulations to generate smoke
    list(APPEND render_tests openvdb)
    list(APPEND workbench_tests openvdb)
    # Some tests in the instancing folder make use of OpenVDB volumes
    list(APPEND render_tests instancing)
    list(APPEND workbench_tests instancing)
  endif()

  if(WITH_OPENIMAGEDENOISE)
    list(APPEND render_tests denoise denoise_animation)
  endif()

  # Disabled until new OpenPGL version with deterministic results.
  # if(WITH_CYCLES_PATH_GUIDING)
  #   list(APPEND render_tests guiding)
  # endif()

  if(WITH_GPU_RENDER_TESTS)
    list(APPEND render_tests grease_pencil)
    list(APPEND workbench_tests grease_pencil)
  endif()

  set(gpu_render_tests ${render_tests})
  # Remove cycles specific tests.
  list(FILTER gpu_render_tests EXCLUDE REGEX light_group|shadow_catcher|denoise|guiding|reports)

  # Add EEVEE specific tests.
  set(eevee_tests ${gpu_render_tests};lighting_nodes)

  list(SORT render_tests)
  list(SORT gpu_render_tests)
  list(SORT eevee_tests)
  list(SORT workbench_tests)


  if(WITH_GPU_RENDER_TESTS)
    function(add_gpu_render_tests name script render_tests)
      if(WITH_OPENGL_BACKEND)
        foreach(render_test ${render_tests})
          add_render_test(
            ${name}_opengl_${render_test}
            ${CMAKE_CURRENT_LIST_DIR}/${script}
            --testdir "${TEST_SRC_DIR}/render/${render_test}"
            --outdir "${TEST_OUT_DIR}/${name}_opengl"
            --gpu-backend opengl
            ${ARGN}
          )
        endforeach()
      endif()

      if(WITH_METAL_BACKEND)
        foreach(render_test ${render_tests})
          add_render_test(
            ${name}_metal_${render_test}
            ${CMAKE_CURRENT_LIST_DIR}/${script}
            --testdir "${TEST_SRC_DIR}/render/${render_test}"
            --outdir "${TEST_OUT_DIR}/${name}_metal"
            --gpu-backend metal
            ${ARGN}
          )
        endforeach()
      endif()

      if(WITH_VULKAN_BACKEND
          AND (NOT (name STREQUAL "storm_hydra" OR name STREQUAL "storm_usd") OR WITH_STORM_TESTS_VULKAN))
        foreach(render_test ${render_tests})
          add_render_test(
            ${name}_vulkan_${render_test}
            ${CMAKE_CURRENT_LIST_DIR}/${script}
            --testdir "${TEST_SRC_DIR}/render/${render_test}"
            --outdir "${TEST_OUT_DIR}/${name}_vulkan"
            --gpu-backend vulkan
            ${ARGN}
          )
        endforeach()
      endif()
    endfunction()

    if(WITH_HYDRA)
      add_gpu_render_tests(storm_hydra "storm_render_tests.py" "${gpu_render_tests}" --export_method "HYDRA")
    endif()
    if(WITH_HYDRA)
      add_gpu_render_tests(storm_usd "storm_render_tests.py" "${gpu_render_tests}" --export_method "USD")
    endif()
  endif()
endif()
