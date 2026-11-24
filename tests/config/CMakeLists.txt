# vehicle_sil_config: the boundary of the library, and its unit tests (the test names keep the prefix
# launch.: the executable links the configuration alone, without the launcher's library, core and SIL Kit).
if(NOT TARGET vehicle_sil::config)
  return()
endif()

vehicle_sil_add_harness_tests(vehicle_sil_config_tests
  REGISTRATION launch.registration.vehicle_sil_config_tests
  PREFIX launch LABELS unit launch config TIMEOUT 60 BATCHABLE
  SOURCES
    test_check.cpp
    test_compare_commands.cpp
    test_config.cpp
    test_connect.cpp
    test_j1939_nodes_match.cpp
  LINK vehicle_sil::config
       vehicle_sil::process # the paths of the files and the executables (test_check.cpp, test_connect.cpp)
       vehicle_sil::j1939 # the network that applies j1939_nodes too (test_j1939_nodes_match.cpp)
       vehicle_sil::can) # the bus that the network is on (test_j1939_nodes_match.cpp)
target_include_directories(vehicle_sil_config_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The configuration and its check are pure: they include no SIL Kit header and, of the core headers,
# only those of vehicle_sil::sim_base, directly or through the headers they include. The library
# does not link core, so the link cannot show this: a header of core that needs no SIL Kit is found
# and compiles.
add_test(NAME config.no_silkit_boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/config
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
# ... and no header of the processes (child_process.hpp, process.hpp: the library links
# vehicle_sil::process for process/paths.hpp, so the link cannot show it) and no header of the
# layers that depend on it (the launcher's library, the scenario and the runner).
add_test(NAME config.includes_no_runtime
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DDIR=libs/config
                 -DFORBIDDEN_PREFIXES=vehicle_sil/launch/,vehicle_sil/scenario/,vehicle_sil/runner/
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
# The check itself must reject a directory that reaches them, and name the chain.
add_test(NAME config.include_check_rejects_a_forbidden_include
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/include_fixtures -DDIR=libs/config
                 -DFORBIDDEN_PREFIXES=vehicle_sil/launch/,vehicle_sil/scenario/,vehicle_sil/runner/
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
set_tests_properties(config.no_silkit_boundary config.includes_no_runtime
                     config.include_check_rejects_a_forbidden_include PROPERTIES LABELS "unit;config" TIMEOUT 60)
set_tests_properties(config.include_check_rejects_a_forbidden_include PROPERTIES
                     PASS_REGULAR_EXPRESSION "uses_launch\\.cpp: vehicle_sil/launch/anything\\.hpp.*uses_launch\\.cpp: vehicle_sil/scenario/anything\\.hpp.*uses_launch\\.cpp: vehicle_sil/runner/anything\\.hpp.*uses_launch\\.cpp: vehicle_sil/process/process\\.hpp"
                     FAIL_REGULAR_EXPRESSION "clean\\.hpp:")
