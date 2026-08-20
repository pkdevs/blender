# Fetch the tool's path within the NATIVE build
function(blender_get_native_tool_path target output_path_var)
  # Check if we're in a multi-config generator
  if(CMAKE_CONFIGURATION_TYPES)
    if(NOT "Release" IN_LIST CMAKE_CONFIGURATION_TYPES)
      message(FATAL_ERROR "Release configuration not present in CMAKE_CONFIGURATION_TYPES, got ${CMAKE_CONFIGURATION_TYPES}")
    endif()
    set(output_path "${blender_NATIVE_BUILD}/bin/Release/${target}${CMAKE_HOST_EXECUTABLE_SUFFIX}")
  else()
    set(output_path "${blender_NATIVE_BUILD}/bin/${target}${CMAKE_HOST_EXECUTABLE_SUFFIX}")
  endif()
  set(${output_path_var} ${output_path} PARENT_SCOPE)
endfunction()

# Build the tool in the NATIVE build
function(blender_build_native_tool target output_path_var)
  cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
  blender_get_native_tool_path(${target} output_path)

  set(build_cmd ${CMAKE_COMMAND} --build ${blender_NATIVE_BUILD} --target ${target} --config Release)
  add_custom_command(OUTPUT "${output_path}"
    COMMAND ${build_cmd}
    DEPENDS CONFIGURE_blender_NATIVE ${ARG_DEPENDS}
    WORKING_DIRECTORY "${blender_NATIVE_BUILD}"
    COMMENT "Building native `${target}` executable for cross-compilation..."
    USES_TERMINAL VERBATIM)
  add_custom_target(build_host_${target} DEPENDS "${output_path}")
  set(${output_path_var} ${output_path} PARENT_SCOPE)
endfunction()

# Make the tool available for use and setup a native build if necessary
function(blender_setup_host_tool target setting_var)
  set(WITH_HOST_TOOL_DIR "" CACHE PATH "Directory containing host-architecture executables for cross-compilation")
  # host tool directory made available by the user
  if(WITH_HOST_TOOL_DIR)
    get_filename_component(TOOL_DIR_ABS "${WITH_HOST_TOOL_DIR}" REALPATH BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(EXISTS "${TOOL_DIR_ABS}/${target}${CMAKE_HOST_EXECUTABLE_SUFFIX}")
      set(${setting_var}_DEFAULT "${TOOL_DIR_ABS}/${target}${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    endif()
  endif()

  set(${setting_var} ${${setting_var}_DEFAULT} CACHE FILEPATH "Path to `${target}` executable for cross-compilation")
  # host tool was passed by the user
  if(${setting_var})
    add_executable(host_${target} IMPORTED GLOBAL)
    set_target_properties(host_${target} PROPERTIES
      IMPORTED_LOCATION ${${setting_var}}
      IMPORTED_LOCATION_DEBUG ${${setting_var}}
      IMPORTED_LOCATION_RELEASE ${${setting_var}}
      IMPORTED_LOCATION_RELWITHDEBINFO ${${setting_var}}
      IMPORTED_LOCATION_MINSIZEREL ${${setting_var}})
  # host tool is being built as an extra step
  elseif(CMAKE_CROSSCOMPILING)
    blender_build_native_tool(${target} exe_name)
    add_executable(host_${target} IMPORTED GLOBAL)
    add_dependencies(host_${target} build_host_${target})
    set_target_properties(host_${target} PROPERTIES
      IMPORTED_LOCATION ${exe_name}
      IMPORTED_LOCATION_DEBUG ${exe_name}
      IMPORTED_LOCATION_RELEASE ${exe_name}
      IMPORTED_LOCATION_RELWITHDEBINFO ${exe_name}
      IMPORTED_LOCATION_MINSIZEREL ${exe_name})
  # using normally built tool
  else()
    add_executable(host_${target} ALIAS ${target})
  endif()
endfunction()

# create a cross build of blender, this will configure a headless blender build
function(blender_create_cross_target target_name buildtype)
  if(NOT DEFINED blender_${target_name}_BUILD)
    set(blender_${target_name}_BUILD "${CMAKE_CURRENT_BINARY_DIR}/${target_name}")
    set(blender_${target_name}_BUILD ${blender_${target_name}_BUILD} CACHE PATH "Build directory for ${target_name} cross-compilation" FORCE)
  endif()

  # Use a stamp file instead of the directory itself as OUTPUT to avoid
  # issues with the clean target trying to delete a non-empty directory
  set(create_stamp "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_create.stamp")

  add_custom_command(OUTPUT ${create_stamp}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${blender_${target_name}_BUILD}
    COMMAND ${CMAKE_COMMAND} -E touch ${create_stamp}
    COMMENT "Creating ${blender_${target_name}_BUILD}..."
  )

  add_custom_target(CREATE_${target_name}
    DEPENDS ${create_stamp}
  )

  set(CROSS_TOOLCHAIN_FLAGS_blender_${target_name} "" CACHE STRING "Toolchain configuration for blender ${target_name}")

  # Generate the .cmake.in file at configure time (not build time)
  # This is required because configure_file needs the input file to exist during CMake configuration
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/blender_${target_name}_BUILD_command.cmake.in
    "@blender_${target_name}_BUILD_command@")

  # Set the command we use to generate the subbuild. A lot of these are inherited to create a matching makesrna host executable.
  set(blender_${target_name}_BUILD_command "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}"
    -DCMAKE_BUILD_TYPE=${buildtype}
    "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}"
    "-DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}"
    -C "${CMAKE_CURRENT_SOURCE_DIR}/build_files/cmake/config/blender_lite.cmake"
    "${CROSS_TOOLCHAIN_FLAGS_blender_${target_name}}" -DWITH_HEADLESS=ON -DWITH_CYCLES=${WITH_CYCLES}
    -DWITH_INTERNATIONAL=${WITH_INTERNATIONAL} -DWITH_USD=${WITH_USD} -DWITH_TBB=${WITH_TBB}
    -DWITH_EXPERIMENTAL_FEATURES=${WITH_EXPERIMENTAL_FEATURES} -DWITH_IMAGE_OPENEXR=${WITH_IMAGE_OPENEXR}
    -DWITH_IMAGE_OPENJPEG=${WITH_IMAGE_OPENJPEG} -DWITH_IMAGE_CINEON=${WITH_IMAGE_CINEON} -DWITH_IMAGE_WEBP=${WITH_IMAGE_WEBP}
    -DWITH_FFMPEG=${WITH_FFMPEG} -DWITH_FFTW3=${WITH_FFTW3} -DWITH_MOD_FLUID=${WITH_MOD_FLUID} -DWITH_MOD_OCEANSIM=${WITH_MOD_OCEANSIM}
    -DWITH_SDL=${WITH_SDL} -DWITH_OPENAL=${WITH_OPENAL} -DWITH_COREAUDIO=${WITH_COREAUDIO} -DWITH_JACK=${WITH_JACK}
    -DWITH_PULSEAUDIO=${WITH_PULSEAUDIO} -DWITH_WASAPI=${WITH_WASAPI} -DWITH_ALEMBIC=${WITH_ALEMBIC}
    -DWITH_BULLET=${WITH_BULLET} -DWITH_FREESTYLE=${WITH_FREESTYLE} -DWITH_OPENSUBDIV=${WITH_OPENSUBDIV} -DWITH_INPUT_NDOF=${WITH_INPUT_NDOF}
    -DWITH_XR_OPENXR=${WITH_XR_OPENXR} -DWITH_OPENGL_BACKEND=${WITH_OPENGL_BACKEND} -DWITH_METAL_BACKEND=${WITH_METAL_BACKEND}
    -DWITH_VULKAN_BACKEND=${WITH_VULKAN_BACKEND} -DWITH_GMP=${WITH_GMP}
    -S "${CMAKE_CURRENT_SOURCE_DIR}" -B "${blender_${target_name}_BUILD}")

  # Put the cross build command in a separate file. If any of the arguments change this will trigger a reconfigure of the cross build
  configure_file(
    ${CMAKE_CURRENT_BINARY_DIR}/blender_${target_name}_BUILD_command.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/blender_${target_name}_BUILD_command.cmake.tmp
    @ONLY)

  # copy if the command has changed, this will trigger a reconfigure of the cross build
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/blender_${target_name}_BUILD_command.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_BINARY_DIR}/blender_${target_name}_BUILD_command.cmake.tmp ${CMAKE_CURRENT_BINARY_DIR}/blender_${target_name}_BUILD_command.cmake
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/blender_${target_name}_BUILD_command.cmake.tmp
    COMMENT "Generating build command for ${target_name} cross-compilation..."
  )

  add_custom_command(OUTPUT ${blender_${target_name}_BUILD}/CMakeCache.txt
    COMMAND ${blender_${target_name}_BUILD_command}
    WORKING_DIRECTORY ${blender_${target_name}_BUILD}
    DEPENDS CREATE_${target_name} "${CMAKE_CURRENT_BINARY_DIR}/blender_${target_name}_BUILD_command.cmake"
    COMMENT "Configuring ${target_name} build in ${blender_${target_name}_BUILD}..."
  )

  add_custom_target(CONFIGURE_blender_${target_name}
    DEPENDS ${blender_${target_name}_BUILD}/CMakeCache.txt
  )
endfunction()


