# vehicle_sil_runner: the boundary of the library. Its unit tests are in tests/launch.
if(NOT TARGET vehicle_sil::runner)
  return()
endif()

# The public headers of the runner (vehicle_sil/runner/, detail/ among them) include no SIL Kit
# header and no header of core's runtime, directly or through the headers they include: the SIL Kit
# parts of a run (the registry, the controller, the hub's transport) stay behind RunnerImpl
# (src/runner_impl.hpp), and the library that links the runner needs no SIL Kit to read them. The
# runner links core, so the link cannot show this.
add_test(NAME runner.public_headers_need_no_silkit
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/runner/include
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
# The runner includes no header of the launcher's library, which depends on it (ONLY_PREFIXES: the runner
# itself uses SIL Kit, the process headers and the run_*.hpp that the check forbids to the pure layers).
add_test(NAME runner.includes_no_launcher
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DDIR=libs/runner
                 -DFORBIDDEN_PREFIXES=vehicle_sil/launch/ -DONLY_PREFIXES=ON
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
# The check itself must reject a directory that reaches it, and name the chain.
add_test(NAME runner.include_check_rejects_a_forbidden_include
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/include_fixtures -DDIR=libs/runner
                 -DFORBIDDEN_PREFIXES=vehicle_sil/launch/ -DONLY_PREFIXES=ON
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
set_tests_properties(runner.public_headers_need_no_silkit runner.includes_no_launcher
                     runner.include_check_rejects_a_forbidden_include PROPERTIES LABELS "unit;runner" TIMEOUT 60)
set_tests_properties(runner.include_check_rejects_a_forbidden_include PROPERTIES
                     PASS_REGULAR_EXPRESSION "uses_launch\\.cpp: vehicle_sil/launch/anything\\.hpp"
                     FAIL_REGULAR_EXPRESSION "clean\\.hpp:")
