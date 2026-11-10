include(VehicleSilTestHelpers)

# The tests of each area are listed in tests/<area>/CMakeLists.txt. This file holds only what
# the areas share and the options that add or skip whole areas.

# main() and the registry of the in-process test harness (unit/test_harness.hpp).
add_library(vehicle_sil_test_main OBJECT unit/test_main.cpp)
target_include_directories(vehicle_sil_test_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/unit)
vehicle_sil_target_defaults(vehicle_sil_test_main)

# Child processes, a registry process, log helpers and coordinated virtual-time runs for the
# process-level launchers.
add_library(vehicle_sil_test_process STATIC
  integration/child_process.cpp
  integration/launcher_support.cpp
  integration/simulation_run.cpp)
target_include_directories(vehicle_sil_test_process PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/integration)
target_link_libraries(vehicle_sil_test_process PUBLIC vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_test_process)

add_subdirectory(unit)
add_subdirectory(core)
add_subdirectory(control)
if(VEHICLE_SIL_BUILD_CONTROL_SERVER)
  add_subdirectory(control_server)
endif()
add_subdirectory(lin)
add_subdirectory(can)
add_subdirectory(testkit)
add_subdirectory(simulation)
add_subdirectory(peer)
# The J1939 stack, its parts, the typed messages and the boundary checks; uses no example.
add_subdirectory(j1939)
# Vector XL CAN bridge (against a fake XL driver; runs on every platform).
add_subdirectory(vector_xl)
if(VEHICLE_SIL_TEST_FETCHCONTENT)
  add_subdirectory(fetchcontent)
endif()

# The remaining tests run the example peers as processes.
if(NOT VEHICLE_SIL_BUILD_EXAMPLES)
  message(STATUS "vehicle-sil: VEHICLE_SIL_BUILD_EXAMPLES is OFF; process-level J1939 and virtual time tests are skipped")
  return()
endif()
add_subdirectory(integration)
add_subdirectory(virtual_time)
