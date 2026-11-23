include(VehicleSilTestHelpers)

# The pages of the manual that the tests compare with what the product does (the examples, the outputs, the steps
# of the guides): the English pages of docs/. The Japanese pages of docs/ja are held to them by scripts/check-docs.py
# (the same code blocks). The tests that check a page for a property of its own (tests/schema, the GUI server's tests,
# gui/client/pack.mjs, scripts/package.py) read both languages.
set(VEHICLE_SIL_TESTED_DOCS "${PROJECT_SOURCE_DIR}/docs")

# The tests of each area are listed in tests/<area>/CMakeLists.txt. This file holds only what
# the areas share. A new area needs no line here: every directory below that has a CMakeLists.txt
# is added (see the end of this file).

# ThreadSanitizer runs have high per-thread CPU/memory overhead: limit heavy multi-ECU
# tests to 2 slots on TSan builds while letting normal builds use all cores.
set(_vehicle_sil_tsan_processors "")
if("thread" IN_LIST VEHICLE_SIL_SANITIZERS
   OR CMAKE_CXX_FLAGS MATCHES "fsanitize=thread"
   OR CMAKE_C_FLAGS MATCHES "fsanitize=thread")
  set(_vehicle_sil_tsan_processors PROCESSORS 2)
endif()

# Shared headers of tests/unit/ (test_harness.hpp, test_timeout_scale.hpp, ...) and the build's
# default timeout factor, attached only to test targets that use them.
add_library(vehicle_sil_test_support INTERFACE)
target_include_directories(vehicle_sil_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/unit)
target_compile_definitions(vehicle_sil_test_support INTERFACE VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT="${VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT}")

# main() and the registry of the in-process test harness (unit/test_harness.hpp).
add_library(vehicle_sil_test_main OBJECT unit/test_main.cpp)
target_link_libraries(vehicle_sil_test_main PUBLIC vehicle_sil_test_support)
vehicle_sil_target_defaults(vehicle_sil_test_main)

# A registry process, log helpers and coordinated virtual-time runs for the process-level
# launchers, on the child processes of vehicle_sil::process.
add_library(vehicle_sil_test_process STATIC
  integration/launcher_support.cpp
  integration/simulation_run.cpp
)
target_include_directories(vehicle_sil_test_process PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/integration)
target_link_libraries(vehicle_sil_test_process PUBLIC vehicle_sil::process vehicle_sil_cli vehicle_sil_test_support)
vehicle_sil_target_defaults(vehicle_sil_test_process)

if(NOT VEHICLE_SIL_BUILD_EXAMPLES)
  message(STATUS "vehicle-sil: VEHICLE_SIL_BUILD_EXAMPLES is OFF; process-level J1939 and virtual time tests are skipped")
endif()

# Every directory below with a CMakeLists.txt is an area, added in the order of the names (the
# order is the same on every platform and file system). An area that exists only under a condition
# says so at the top of its own CMakeLists.txt and returns (control_server, fetchcontent,
# integration, virtual_time; multi_host and parts of others skip themselves the same way).
# The order matters in one place only: eth_link sets VEHICLE_SIL_FAKE_PCAP_DIR, which
# ethernet_bridge reads, and the names keep eth_link first. An area may not test for a target of
# another area with if(TARGET ...), because that would depend on the order.
file(GLOB _vehicle_sil_test_areas
     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt)
list(SORT _vehicle_sil_test_areas COMPARE STRING CASE SENSITIVE)
foreach(_vehicle_sil_test_area IN LISTS _vehicle_sil_test_areas)
  get_filename_component(_vehicle_sil_test_area ${_vehicle_sil_test_area} DIRECTORY)
  add_subdirectory(${_vehicle_sil_test_area})
endforeach()
