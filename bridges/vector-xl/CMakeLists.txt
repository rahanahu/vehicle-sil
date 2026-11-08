# Vector XL CAN bridge.
#
#   vehicle_sil::vector_xl_bridge_core  platform independent: CanBridge, ExternalPortAdapter,
#                                       channel selection, CLI parsing. Built everywhere so it is
#                                       tested on Linux too.
#   vehicle_sil_vector_xl_logic         platform independent object library: the XL backend
#                                       logic (XlDriver, XlCanPort) written against the internal
#                                       driver interface src/xl_backend.hpp. Whoever links it
#                                       also links one implementation of that interface.
#   vehicle_sil::vector_xl              the logic plus src/xl_backend_vector.cpp, the
#                                       implementation that calls Vector's XL Driver Library.
#   vehicle_sil_vector_xl_bridge        the bridge executable.
#
# The last two are Windows only and need Vector's XL Driver Library (vxlapi.h and
# vxlapi64.lib/vxlapi.lib), which is not redistributed with vehicle-sil. Point
# VEHICLE_SIL_VECTOR_XL_DIR (CMake cache or environment variable) at the library's folder.
# Without it they are skipped; Linux builds never need it. The tests link the logic with a
# fake implementation (tests/vector_xl/fake_xl).

add_library(vehicle_sil_vector_xl_bridge_core STATIC
  src/can_bridge.cpp
  src/channel_selection.cpp
  src/bridge_options.cpp
  src/external_port_adapter.cpp)
add_library(vehicle_sil::vector_xl_bridge_core ALIAS vehicle_sil_vector_xl_bridge_core)
set_target_properties(vehicle_sil_vector_xl_bridge_core PROPERTIES EXPORT_NAME vector_xl_bridge_core)
target_include_directories(vehicle_sil_vector_xl_bridge_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_vector_xl_bridge_core PUBLIC SilKit::SilKit vehicle_sil::can vehicle_sil::can_silkit)
vehicle_sil_target_defaults(vehicle_sil_vector_xl_bridge_core)

# An object library, so that each library that adds an implementation of xl_backend.hpp holds
# the logic's objects itself and the link order between the two does not matter.
add_library(vehicle_sil_vector_xl_logic OBJECT src/xl_driver.cpp)
target_include_directories(vehicle_sil_vector_xl_logic
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vehicle_sil_vector_xl_logic PUBLIC vehicle_sil::vector_xl_bridge_core)
vehicle_sil_target_defaults(vehicle_sil_vector_xl_logic)

if(NOT WIN32)
  return()
endif()

set(VEHICLE_SIL_VECTOR_XL_DIR "$ENV{VEHICLE_SIL_VECTOR_XL_DIR}"
    CACHE PATH "Vector XL Driver Library folder (with vxlapi.h and vxlapi64.lib, usually in bin/)")
option(VEHICLE_SIL_REQUIRE_VECTOR_XL "Fail the configuration if the XL Driver Library is not found" OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_vehicle_sil_vxlapi_name vxlapi64)
else()
  set(_vehicle_sil_vxlapi_name vxlapi)
endif()
find_path(VEHICLE_SIL_VXLAPI_INCLUDE_DIR vxlapi.h
  HINTS "${VEHICLE_SIL_VECTOR_XL_DIR}" PATH_SUFFIXES bin include NO_CMAKE_FIND_ROOT_PATH)
find_library(VEHICLE_SIL_VXLAPI_LIBRARY NAMES ${_vehicle_sil_vxlapi_name}
  HINTS "${VEHICLE_SIL_VECTOR_XL_DIR}" PATH_SUFFIXES bin lib NO_CMAKE_FIND_ROOT_PATH)
find_file(VEHICLE_SIL_VXLAPI_DLL NAMES ${_vehicle_sil_vxlapi_name}.dll
  HINTS "${VEHICLE_SIL_VECTOR_XL_DIR}" PATH_SUFFIXES bin NO_CMAKE_FIND_ROOT_PATH)

if(NOT VEHICLE_SIL_VXLAPI_INCLUDE_DIR OR NOT VEHICLE_SIL_VXLAPI_LIBRARY)
  set(_vehicle_sil_msg
      "vehicle-sil: Vector XL Driver Library not found (vxlapi.h / ${_vehicle_sil_vxlapi_name}); "
      "the Vector XL bridge is not built. Set VEHICLE_SIL_VECTOR_XL_DIR to the library folder.")
  if(VEHICLE_SIL_REQUIRE_VECTOR_XL)
    message(FATAL_ERROR ${_vehicle_sil_msg})
  endif()
  message(STATUS ${_vehicle_sil_msg})
  return()
endif()
message(STATUS "vehicle-sil: Vector XL Driver Library: ${VEHICLE_SIL_VXLAPI_LIBRARY}")

add_library(vehicle_sil_vector_xl STATIC src/xl_backend_vector.cpp)
add_library(vehicle_sil::vector_xl ALIAS vehicle_sil_vector_xl)
set_target_properties(vehicle_sil_vector_xl PROPERTIES EXPORT_NAME vector_xl)
target_include_directories(vehicle_sil_vector_xl
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(vehicle_sil_vector_xl SYSTEM PRIVATE "${VEHICLE_SIL_VXLAPI_INCLUDE_DIR}")
target_link_libraries(vehicle_sil_vector_xl
  PUBLIC vehicle_sil::vector_xl_bridge_core
  PRIVATE vehicle_sil_vector_xl_logic "${VEHICLE_SIL_VXLAPI_LIBRARY}")
vehicle_sil_target_defaults(vehicle_sil_vector_xl)

add_executable(vehicle_sil_vector_xl_bridge app/main.cpp)
target_link_libraries(vehicle_sil_vector_xl_bridge PRIVATE vehicle_sil::vector_xl vehicle_sil::core vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_vector_xl_bridge)
vehicle_sil_copy_runtime_dlls(vehicle_sil_vector_xl_bridge)
if(VEHICLE_SIL_VXLAPI_DLL)
  add_custom_command(
    TARGET vehicle_sil_vector_xl_bridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VEHICLE_SIL_VXLAPI_DLL}" $<TARGET_FILE_DIR:vehicle_sil_vector_xl_bridge>
    VERBATIM)
endif()
