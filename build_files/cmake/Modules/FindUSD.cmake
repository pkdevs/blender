
#

message(STATUS "FindUSD")

find_package(pxr REQUIRED)
if (pxr_FOUND)
  set(USD_FOUND TRUE)
  get_filename_component(USD_LIBRARY_DIR ${PXR_usd_LIBRARY} DIRECTORY)
  set(USD_INCLUDE_DIR ${PXR_INCLUDE_DIRS})
  set(USD_LIBRARIES ${PXR_LIBRARIES})
  if(EXISTS ${USD_INCLUDE_DIR}/pxr/base/tf/pyModule.h)
    set(USD_PYTHON_SUPPORT ON)
  endif()
else()
  set(USD_FOUND FALSE)
endif()
message(STATUS "pxr_FOUND: ${pxr_FOUND}")
message(STATUS "USD_FOUND: ${USD_FOUND}")
message(STATUS "USD_LIBRARIES: ${USD_LIBRARIES}")
message(STATUS "USD_PYTHON_SUPPORT: ${USD_PYTHON_SUPPORT}")

mark_as_advanced(
  USD_INCLUDE_DIR
  USD_LIBRARY_DIR
)
