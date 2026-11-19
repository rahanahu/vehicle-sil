# vehicle_sil_runner: the boundary of the library, and its unit tests (the test names keep the prefix
# launch.: the executable links the runner, without the launcher's library).
if(NOT TARGET vehicle_sil::runner)
  return()
endif()

vehicle_sil_add_harness_tests(vehicle_sil_runner_tests
  REGISTRATION launch.registration.vehicle_sil_runner_tests
  PREFIX launch LABELS unit launch runner TIMEOUT 60
  SOURCES
    test_control_rules.cpp
    test_run.cpp
    test_run_controls.cpp
    test_run_outside.cpp
    test_run_plan.cpp
    test_stop_requests.cpp
    test_traffic.cpp
  LINK vehicle_sil::runner vehicle_sil::core # core: the time commands with a controller (test_control_rules.cpp)
       vehicle_sil::control # the hub that the run's samples come from (test_traffic.cpp)
       vehicle_sil::control_silkit # the hub's transport of a real connection (test_run.cpp)
       vehicle_sil::process # the participants' processes that a run ends (test_run.cpp)
       # the statistics lines as the bridges print them, which a run reads (test_run_outside.cpp)
       vehicle_sil::ethernet_bridge_core vehicle_sil::can_bridge_core)
# libs/runner/src: the run's private headers that the tests read (run_pacing.hpp, run_notes.hpp,
# run_connection.hpp).
target_include_directories(vehicle_sil_runner_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/config
                           ${PROJECT_SOURCE_DIR}/libs/runner/src)

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

# vehicle_sil::runner alone is enough to link what its public headers declare: no definition of them
# is in the launcher's library, which depends on it (an include check does not see that). The
# program is built, not run; the test builds it again, so that a missing definition fails here.
add_executable(vehicle_sil_runner_link_only link_only_runner.cpp)
target_link_libraries(vehicle_sil_runner_link_only PRIVATE vehicle_sil::runner)
vehicle_sil_target_defaults(vehicle_sil_runner_link_only)
add_test(NAME runner.links_without_the_launcher
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target vehicle_sil_runner_link_only)
set_tests_properties(runner.links_without_the_launcher PROPERTIES LABELS "unit;runner" TIMEOUT 600)

# The files of the runner that hold the plain data and rules of a run (no SIL Kit, process, run,
# events, time-command or stop-request header, directly or through the headers they include): the
# commands, the traffic counts and the rules of the control. They were checked as part of the
# launcher's pure files before the launcher's library was split.
add_test(NAME runner.pure_files_include_no_runtime
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
                 -DFILES=libs/runner/include/vehicle_sil/runner/traffic.hpp,libs/runner/include/vehicle_sil/runner/command.hpp,libs/runner/src/control_rules.cpp,libs/runner/include/vehicle_sil/runner/control_rules.hpp
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
set_tests_properties(runner.pure_files_include_no_runtime PROPERTIES LABELS "unit;runner" TIMEOUT 60)
