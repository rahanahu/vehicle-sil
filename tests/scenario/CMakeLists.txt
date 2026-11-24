# vehicle_sil_scenario: the boundary of the library, and its unit tests (the test names keep the prefix
# launch.: the executable links the scenario and the configuration, without the launcher's library, core and SIL Kit).
if(NOT TARGET vehicle_sil::scenario)
  return()
endif()

vehicle_sil_add_harness_tests(vehicle_sil_scenario_tests
  REGISTRATION launch.registration.vehicle_sil_scenario_tests
  PREFIX launch LABELS unit launch scenario TIMEOUT 60 BATCHABLE
  SOURCES
    test_scenario.cpp
    test_scenario_line.cpp
  LINK vehicle_sil::scenario vehicle_sil::config
       vehicle_sil::process # the files of the scenario's tests (test_scenario.cpp)
       vehicle_sil::signal) # the shapes of a scenario (test_scenario_line.cpp: scenario_shapes.hpp)
target_include_directories(vehicle_sil_scenario_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                           ${PROJECT_SOURCE_DIR}/tests/config)

# The scenario is pure: it includes no SIL Kit header and, of the core headers, only those of
# vehicle_sil::sim_base, directly or through the headers it includes. The library does not link
# core, so the link cannot show this: a header of core that needs no SIL Kit is found and compiles.
add_test(NAME scenario.no_silkit_boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/scenario
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
# ... and no header of the processes (child_process.hpp, process.hpp: vehicle_sil::config links
# vehicle_sil::process, so the link cannot show it) and no header of the launcher's library and the runner, which
# depend on it.
add_test(NAME scenario.includes_no_runtime
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DDIR=libs/scenario
                 -DFORBIDDEN_PREFIXES=vehicle_sil/launch/,vehicle_sil/runner/
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
# The check itself must reject a directory that reaches them, and name the chain.
add_test(NAME scenario.include_check_rejects_a_forbidden_include
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/include_fixtures -DDIR=libs/scenario
                 -DFORBIDDEN_PREFIXES=vehicle_sil/launch/,vehicle_sil/runner/
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
set_tests_properties(scenario.no_silkit_boundary scenario.includes_no_runtime
                     scenario.include_check_rejects_a_forbidden_include PROPERTIES LABELS "unit;scenario" TIMEOUT 60)
set_tests_properties(scenario.include_check_rejects_a_forbidden_include PROPERTIES
                     PASS_REGULAR_EXPRESSION "uses_launch\\.cpp: vehicle_sil/launch/anything\\.hpp.*uses_launch\\.cpp: vehicle_sil/runner/anything\\.hpp.*uses_launch\\.cpp: vehicle_sil/process/process\\.hpp"
                     FAIL_REGULAR_EXPRESSION "clean\\.hpp:")
