# vehicle_sil_config: the boundary of the library. Its unit tests are in tests/launch.
if(NOT TARGET vehicle_sil::config)
  return()
endif()

# The configuration and its check are pure: they include no SIL Kit header and, of the core headers,
# only those of vehicle_sil::sim_base, directly or through the headers they include. The library
# does not link core, so the link cannot show this: a header of core that needs no SIL Kit is found
# and compiles.
add_test(NAME config.no_silkit_boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/config
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
# ... and no header of the processes (child_process.hpp, process.hpp: the library links
# vehicle_sil::process for process/paths.hpp, so the link cannot show it) and no header of the
# layers that depend on it (the launcher's library and the scenario).
add_test(NAME config.includes_no_runtime
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DDIR=libs/config
                 -DFORBIDDEN_PREFIXES=vehicle_sil/launch/,vehicle_sil/scenario/
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
# The check itself must reject a directory that reaches them, and name the chain.
add_test(NAME config.include_check_rejects_a_forbidden_include
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/include_fixtures -DDIR=libs/config
                 -DFORBIDDEN_PREFIXES=vehicle_sil/launch/,vehicle_sil/scenario/
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
set_tests_properties(config.no_silkit_boundary config.includes_no_runtime
                     config.include_check_rejects_a_forbidden_include PROPERTIES LABELS "unit;config" TIMEOUT 60)
set_tests_properties(config.include_check_rejects_a_forbidden_include PROPERTIES
                     PASS_REGULAR_EXPRESSION "uses_launch\\.cpp: vehicle_sil/launch/anything\\.hpp.*uses_launch\\.cpp: vehicle_sil/scenario/anything\\.hpp.*uses_launch\\.cpp: vehicle_sil/process/process\\.hpp"
                     FAIL_REGULAR_EXPRESSION "clean\\.hpp:")
