# SPDX-FileCopyrightText: 2011-2026 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

set(INC
  ../../..
)

set(INC_SYS

)

set(SRC_KERNEL_DEVICE_CUDA
  kernel.cu
)

set(SRC_KERNEL_DEVICE_CUDA_HEADERS
  compat.h
  config.h
  globals.h
)

set(LIB

)

function(cuda_get_version nvcc_executable out_version)
  execute_process(COMMAND "${nvcc_executable}" "--version" OUTPUT_VARIABLE NVCC_OUT)
  string(REGEX REPLACE ".*release ([0-9]+)\\.([0-9]+).*" "\\1" version_major "${NVCC_OUT}")
  string(REGEX REPLACE ".*release ([0-9]+)\\.([0-9]+).*" "\\2" version_minor "${NVCC_OUT}")
  set(${out_version} "${version_major}.${version_minor}" PARENT_SCOPE)
endfunction()

function(cuda_get_arch_supported nvcc_executable arch out_is_supported)
  if("${arch}" MATCHES "^compute_[0-9]+$")
    execute_process(COMMAND "${nvcc_executable}" "--list-gpu-arch" OUTPUT_VARIABLE NVCC_OUT)
  elseif("${arch}" MATCHES "^sm_[0-9]+$")
    execute_process(COMMAND "${nvcc_executable}" "--list-gpu-code" OUTPUT_VARIABLE NVCC_OUT)
  else()
    message(WARNING "Unknown CUDA architecture ${arch}")
    # Leave the actual check to the toolkit.
    set(${out_is_supported} TRUE PARENT_SCOPE)
    return()
  endif()

  # Normalize the new lines for easier regex.
  string(REPLACE "\r\n" "\n" NVCC_OUT "${NVCC_OUT}")

  if("${NVCC_OUT}" MATCHES "(^|\n)${arch}\n")
    set(${out_is_supported} TRUE PARENT_SCOPE)
    return()
  endif()

  set(${out_is_supported} FALSE PARENT_SCOPE)
endfunction()

function(cuda_get_toolkit_for_arch arch out_root_dir out_nvcc_executable out_version)
  set(${out_root_dir}        PARENT_SCOPE)
  set(${out_nvcc_executable} PARENT_SCOPE)
  set(${out_version}         PARENT_SCOPE)

  set(_preferred_toolkits 13 12)
  if(arch MATCHES "^compute_[0-9]+$")
    list(REVERSE _preferred_toolkits)
  endif()

  foreach(major_toolkit_version ${_preferred_toolkits})
    if(DEFINED "CUDA${major_toolkit_version}_TOOLKIT_ROOT_DIR")
      if(NOT EXISTS "${CUDA${major_toolkit_version}_TOOLKIT_ROOT_DIR}")
        message(WARNING "CUDA${major_toolkit_version}_TOOLKIT_ROOT_DIR does not exist: ${CUDA${major_toolkit_version}_TOOLKIT_ROOT_DIR}")
        continue()
      endif()
      if(NOT EXISTS "${CUDA${major_toolkit_version}_NVCC_EXECUTABLE}")
        message(WARNING "CUDA${major_toolkit_version}_NVCC_EXECUTABLE does not exist: ${CUDA${major_toolkit_version}_NVCC_EXECUTABLE}")
        continue()
      endif()

      set(_root_dir        "${CUDA${major_toolkit_version}_TOOLKIT_ROOT_DIR}")
      set(_nvcc_executable "${CUDA${major_toolkit_version}_NVCC_EXECUTABLE}")
      cuda_get_version("${_nvcc_executable}" _version)
    else()
      continue()
    endif()

    cuda_get_arch_supported("${_nvcc_executable}" "${arch}" _is_supported)
    if(NOT ${_is_supported})
      continue()
    endif()

    set(${out_root_dir}        "${_root_dir}"         PARENT_SCOPE)
    set(${out_nvcc_executable} "${_nvcc_executable}"  PARENT_SCOPE)
    set(${out_version}         "${_version}"          PARENT_SCOPE)
    return()
  endforeach()

  # No suitable explicit toolkit found. Fallback to the default.

  cuda_get_arch_supported("${CUDA_NVCC_EXECUTABLE}" "${arch}" _is_supported)
  if(NOT _is_supported)
    return()
  endif()

  cuda_get_version("${CUDA_NVCC_EXECUTABLE}" _version)
  set(${out_root_dir}        "${CUDA_TOOLKIT_ROOT_DIR}" PARENT_SCOPE)
  set(${out_nvcc_executable} "${CUDA_NVCC_EXECUTABLE}"  PARENT_SCOPE)
  set(${out_version}         "${_version}"              PARENT_SCOPE)
endfunction()

function(cuda_add_common_flags cuda_version arch in_flags out_flags)
  set(flags ${in_flags})

  if(CUDA_HOST_COMPILER)
    set(flags ${flags} -ccbin="${CUDA_HOST_COMPILER}")
  endif()

  if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(flags ${flags} --force-cl-env-setup)
  endif()

  set(flags ${flags}
    # Helps with compatibility when using recent clang host compiler.
    "-std=c++17"
    --use_fast_math
    -Wno-deprecated-gpu-targets)

  if(WITH_CYCLES_DEBUG)
    set(flags ${flags}
      -D WITH_CYCLES_DEBUG
      --ptxas-options="-v")
  endif()

  if(WITH_NANOVDB)
    set(flags ${flags} -D WITH_NANOVDB)
  endif()

  if("${cuda_version}" VERSION_GREATER_EQUAL 12.3 AND "${arch}" STREQUAL "sm_120")
    # Enable jump table generation for the SVM switch statement.
    set(flags ${flags} --jump-table-density 80)
  endif()

  if(NOT WITH_CYCLES_CUDA_BUILD_SERIAL AND "${cuda_version}" VERSION_GREATER_EQUAL 12.9)
    # Only use split compile with few binaries, to avoid excessive memory usage.
    # This is mainly helpful for quick local builds for one architecture.
    list(LENGTH CYCLES_CUDA_BINARIES_ARCH _num_binaries)
    if(_num_binaries LESS_EQUAL 2)
      set(flags ${flags} --split-compile=0)
    endif()
  endif()

  set(${out_flags} ${flags} PARENT_SCOPE)
endfunction()

if(WITH_CYCLES_CUDA_BINARIES)
  # 64 bit only
  set(CUDA_BITS 64)

  # warn for other versions
  if((CUDA_VERSION VERSION_EQUAL 10.1) OR
     (CUDA_VERSION VERSION_EQUAL 10.2) OR
     (CUDA_VERSION_MAJOR STREQUAL "11") OR
     (CUDA_VERSION_MAJOR STREQUAL "12") OR
     (CUDA_VERSION_MAJOR STREQUAL "13"))
  else()
    message(WARNING
      "CUDA version ${CUDA_VERSION_MAJOR}.${CUDA_VERSION_MINOR} detected, "
      "build may succeed but only CUDA 13, 12, 11, 10.2 and 10.1 have been tested")
  endif()

  # build for each arch
  set(cuda_sources kernel.cu
    ${SRC_KERNEL_DEVICE_CUDA_HEADERS}
    $<TARGET_PROPERTY:cycles_kernel,INTERFACE_SOURCES>
  )
  set(cuda_cubins "")

  # Modifies in parent scope:
  # - `cuda_cubins`: appended with compressed `cubin/ptx` output.
  function(CYCLES_CUDA_KERNEL_ADD arch prev_arch name flags sources experimental)
    if(${arch} MATCHES "compute_.*")
      set(format "ptx")
    else()
      set(format "cubin")
    endif()
    set(cuda_file ${name}_${arch}.${format})
    set(cuda_file_compressed ${cuda_file}.zst)

    set(kernel_sources ${sources})
    if(NOT ${prev_arch} STREQUAL "none")
      if(${prev_arch} MATCHES "compute_.*")
        set(kernel_sources ${kernel_sources} ${name}_${prev_arch}.ptx)
      else()
        set(kernel_sources ${kernel_sources} ${name}_${prev_arch}.cubin)
      endif()
    endif()

    set(cuda_kernel_src "/${name}.cu")

    set(cuda_flags ${flags}
      -D CCL_NAMESPACE_BEGIN=
      -D CCL_NAMESPACE_END=
      -D NVCC
      -D _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
      -m ${CUDA_BITS}
      -I ${CMAKE_CURRENT_SOURCE_DIR}/../../..
      -o ${CMAKE_CURRENT_BINARY_DIR}/${cuda_file})

    cuda_add_common_flags(${cuda_version} ${arch} "${cuda_flags}" cuda_flags)

    set(_cuda_nvcc_args
      -arch=${arch}
      ${CUDA_NVCC_FLAGS}
      --${format}
      ${CMAKE_CURRENT_SOURCE_DIR}${cuda_kernel_src}
      ${cuda_flags}
    )

    if(WITH_COMPILER_CCACHE AND CCACHE_PROGRAM)
      add_custom_command(
        OUTPUT ${cuda_file}
        COMMAND ${CCACHE_PROGRAM} ${cuda_nvcc_executable} ${_cuda_nvcc_args}
        DEPENDS ${kernel_sources})
    else()
      add_custom_command(
        OUTPUT ${cuda_file}
        COMMAND ${cuda_nvcc_executable} ${_cuda_nvcc_args}
        DEPENDS ${kernel_sources})
    endif()

    add_custom_command(
      OUTPUT ${cuda_file_compressed}
      COMMAND "$<TARGET_FILE:zstd_compress>" ${cuda_file} ${cuda_file_compressed}
      DEPENDS ${cuda_file})

    unset(_cuda_nvcc_args)
    delayed_install("${CMAKE_CURRENT_BINARY_DIR}" "${cuda_file_compressed}" ${CYCLES_INSTALL_PATH}/lib)
    list(APPEND cuda_cubins ${cuda_file_compressed})
    set(cuda_cubins "${cuda_cubins}" PARENT_SCOPE)
  endfunction()

  set(prev_arch "none")
  foreach(arch ${CYCLES_CUDA_BINARIES_ARCH})
    if("${arch}" MATCHES ".*_3.")
      message(STATUS "CUDA binaries for ${arch} are no longer supported, skipped.")
      continue()
    endif()
    cuda_get_toolkit_for_arch("${arch}" cuda_toolkit_root_dir cuda_nvcc_executable cuda_version)
    if(DEFINED cuda_nvcc_executable AND DEFINED cuda_toolkit_root_dir)
      message(STATUS "Cycles CUDA: Using CUDA toolkit ${cuda_toolkit_root_dir} (version ${cuda_version}) for ${arch}")

      # Compile regular kernel
      cycles_cuda_kernel_add(${arch} ${prev_arch} kernel "" "${cuda_sources}" FALSE)

      if(WITH_CYCLES_CUDA_BUILD_SERIAL)
        set(prev_arch ${arch})
      endif()

      unset(cuda_nvcc_executable)
      unset(cuda_toolkit_root_dir)
    else()
      if(WITH_STRICT_BUILD_OPTIONS)
        message(FATAL_ERROR "Unable to find CUDA toolkit for ${arch}")
      else()
        message(WARNING "Unable to find CUDA toolkit for ${arch}, skipping the architecture")
      endif()
    endif()
  endforeach()

  add_custom_target(cycles_kernel_cuda
    ALL
    DEPENDS ${cuda_cubins}
    SOURCES ${SRC_KERNEL_DEVICE_CUDA} ${SRC_KERNEL_DEVICE_CUDA_HEADERS}
  )
  cycles_set_solution_folder(cycles_kernel_cuda)

  source_group("device\\cuda" FILES ${SRC_KERNEL_DEVICE_CUDA} ${SRC_KERNEL_DEVICE_CUDA_HEADERS})

  add_dependencies(cycles_kernel cycles_kernel_cuda)
endif()

delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_DEVICE_CUDA}" ${CYCLES_INSTALL_PATH}/source/kernel/device/cuda)
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "${SRC_KERNEL_DEVICE_CUDA_HEADERS}" ${CYCLES_INSTALL_PATH}/source/kernel/device/cuda)
