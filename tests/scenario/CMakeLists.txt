# vehicle_sil_scenario: the boundary of the library. Its unit tests are in tests/launch.
if(NOT TARGET vehicle_sil::scenario)
  return()
endif()

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
