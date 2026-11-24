# vehicle_sil_launch: reading the simulation configuration, the rules of the check (pure
# functions: no process), the output for people and as JSON.
if(NOT TARGET vehicle_sil::launch)
  return()
endif()
vehicle_sil_add_harness_tests(vehicle_sil_launch_tests
  PREFIX launch LABELS unit launch TIMEOUT 60
  SOURCES
    test_batch.cpp
    test_check_parallel.cpp
    test_check_reports.cpp
    test_config_executables.cpp
    test_connect_tool.cpp
    test_connections.cpp
    test_events.cpp
    test_execution_runtime.cpp
    test_request.cpp
    test_request_file.cpp
    test_run.cpp
    test_run_controls.cpp
    test_run_sink.cpp
    test_scenario_samples.cpp
    test_scenario_timeline.cpp
    test_serve.cpp
    test_sha256.cpp
    test_shared_values.cpp
    test_traffic.cpp
  LINK vehicle_sil::launch vehicle_sil::core # core: the time commands with a controller (test_events.cpp)
       vehicle_sil::control # the hub that the run's samples come from (test_traffic.cpp)
       vehicle_sil::control_silkit # the hub's transport of a real connection (test_run.cpp)
       vehicle_sil_cli # the other tools' exit codes and durations (test_shared_values.cpp)
       vehicle_sil::signal # the shapes of a scenario's timeline (test_scenario_timeline.cpp)
       vehicle_sil::j1939) # the networks that the check's reports show (test_check_reports.cpp)
# The tests of the units below the launcher's library are in tests/config, tests/scenario and tests/runner, with
# the same test names (launch.<test>): the helpers that both sides use are headers there
# (launch_test_util.hpp, check_setup.hpp, scenario_model.hpp, run_support.hpp and the other *_support.hpp).
# libs/runner/src: the run's private headers that the tests read (run_pacing.hpp, run_notes.hpp,
# run_connection.hpp). libs/launch/src: the outputs' private header (format/check_format.hpp).
target_include_directories(vehicle_sil_launch_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/config
                           ${PROJECT_SOURCE_DIR}/tests/scenario ${PROJECT_SOURCE_DIR}/tests/runner
                           ${PROJECT_SOURCE_DIR}/libs/launch/src ${PROJECT_SOURCE_DIR}/libs/runner/src)
add_dependencies(vehicle_sil_launch_tests vehicle_sil_process_helper)
target_compile_definitions(vehicle_sil_launch_tests PRIVATE VEHICLE_SIL_LAUNCH_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
                           VEHICLE_SIL_PROCESS_HELPER="$<TARGET_FILE:vehicle_sil_process_helper>"
                           # test_shared_values.cpp reads the GUI's sources.
                           VEHICLE_SIL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# The sources of the configuration, the launcher (its library and its tool) read the networks of a description in
# one place (seen_networks(), which applies a participant's network_names and j1939_nodes; the J1939
# nodes are members of the networks, so this keeps their values in the code from the rules too), and where --describe's
# JSON is read and written back; the check itself must reject a read anywhere else (also one
# through a pointer to the member), and pass the networks of the file.
add_test(NAME launch.described_networks_are_read_in_one_place
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DDIR=libs/config,libs/launch,libs/runner,libs/scenario,tools/launch
                 -DFILES=libs/config/src/check_networks.cpp,libs/config/src/describe_read.cpp,libs/launch/src/format/reported_networks.cpp
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckDescribedNetworks.cmake)
add_test(NAME launch.described_networks_check_rejects_a_direct_read
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/network_fixtures/direct -DDIR=launch
                 -DFILES=launch/src/check_networks.cpp,launch/src/describe_read.cpp
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckDescribedNetworks.cmake)
set_tests_properties(launch.described_networks_are_read_in_one_place launch.described_networks_check_rejects_a_direct_read
                     PROPERTIES LABELS "unit;launch" TIMEOUT 60)
set_tests_properties(launch.described_networks_check_rejects_a_direct_read PROPERTIES
                     PASS_REGULAR_EXPRESSION "launch/src/rules\\.cpp: if \\(\\(result\\.ecu->description\\.\\*\\(&EcuDescription::networks\\)\\)\\.empty\\(\\)\\)[^:]*launch/src/rules\\.cpp: for \\(const auto& network : result\\.ecu->description\\.networks\\)"
                     FAIL_REGULAR_EXPRESSION "check_networks\\.cpp: |config\\.networks")

# The five parts of libs/launch/src (usecases/, request/, probe/, format/, endpoints/) and the public
# headers that belong to them keep to the include direction below, directly or through the headers
# they include (ONLY_PREFIXES: the check's built-in list of forbidden headers does not apply, because a part
# may use events.hpp or a process header; only the prefixes named here count). usecases/ calls the other parts in
# order and has no limit. The private headers between the parts are included as "format/..." and the like,
# so a prefix ending in "/" names a part.
#  - request/ reads the request and checks the shape of its fields: configuration values only. It
#    reaches no run, scenario, process or SIL Kit header, no other part, no use case's header.
#  - probe/ looks at the computer and returns values: no run, no SIL Kit, no output, no endpoint, no
#    use case.
#  - format/ writes the outputs: it does not look at the computer (probe.hpp), reaches no endpoint, no
#    SIL Kit header and no other use case.
#  - endpoints/ translate stdin commands and the Control Server's calls: they do not look at the
#    computer and do not write the outputs of check, run and timeline (*_format.hpp). They may read the
#    configuration's value types and its YAML reader (command_input.cpp), and events.hpp.
# To let a part include more, take the prefix off the list for that part here and say so in
# docs/architecture.md ("Layers in libs/launch"); a file that breaks the direction is moved or split,
# not listed as an exception.
set(_launch_silkit_prefixes silkit/ vehicle_sil/control_silkit/ vehicle_sil/can_silkit/ vehicle_sil/eth_silkit/
                            vehicle_sil/lin_silkit/ vehicle_sil/core/participant_)
set(_launch_probe_headers vehicle_sil/launch/probe)
set(_launch_output_headers vehicle_sil/launch/events vehicle_sil/launch/run_sink vehicle_sil/launch/timeline_format
                           vehicle_sil/launch/error_result)
set(_launch_endpoint_headers vehicle_sil/launch/command_input)
set(_launch_usecase_headers vehicle_sil/launch/run. vehicle_sil/launch/serve. vehicle_sil/launch/execution_runtime.
                            vehicle_sil/launch/connections vehicle_sil/launch/test vehicle_sil/launch/checked)
set(_launch_include_check ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DONLY_PREFIXES=ON)
set(_launch_include_script -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)

# name, the directory of the part under libs/launch/src, the part's public headers (a list), then
# the prefixes the part must not reach.
function(_launch_part_test name dir headers)
  set(_files ${headers})
  list(TRANSFORM _files PREPEND libs/launch/include/vehicle_sil/launch/)
  list(JOIN _files "," _files)
  list(JOIN ARGN "," _prefixes)
  add_test(NAME launch.${name}
           COMMAND ${_launch_include_check} -DDIR=libs/launch/src/${dir} -DFILES=${_files}
                   -DFORBIDDEN_PREFIXES=${_prefixes} ${_launch_include_script})
  set_tests_properties(launch.${name} PROPERTIES LABELS "unit;launch" TIMEOUT 60)
endfunction()
_launch_part_test(request_boundary request "request.hpp"
    vehicle_sil/runner/ vehicle_sil/scenario/ vehicle_sil/process/ ${_launch_silkit_prefixes}
    ${_launch_probe_headers} ${_launch_output_headers} ${_launch_endpoint_headers} ${_launch_usecase_headers}
    usecases/ probe/ format/ endpoints/)
_launch_part_test(probe_boundary probe "probe.hpp"
    vehicle_sil/runner/ ${_launch_silkit_prefixes}
    ${_launch_output_headers} ${_launch_endpoint_headers} ${_launch_usecase_headers}
    usecases/ format/ endpoints/)
_launch_part_test(format_boundary format "run_sink.hpp;timeline_format.hpp;error_result.hpp"
    ${_launch_silkit_prefixes} vehicle_sil/control_grpc/ ${_launch_probe_headers} ${_launch_endpoint_headers}
    probe/ endpoints/ usecases/)
# usecases/ has no limit on the other parts, but it reaches no SIL Kit header: the SIL Kit parts of a run are
# behind the runner, and the Control Server's file (endpoints/) is the one place in libs/launch that includes them.
_launch_part_test(usecases_boundary usecases "run.hpp;serve.hpp;execution_runtime.hpp;test.hpp;connections.hpp;checked.hpp" ${_launch_silkit_prefixes})
_launch_part_test(endpoints_boundary endpoints "command_input.hpp"
    ${_launch_probe_headers} vehicle_sil/launch/timeline_format probe/ format/)
# The check itself must reject a file that reaches a forbidden header through a header beside it
# and through a private header of another part (found in the directory above: "request/shared.hpp"
# included from probe/), and name the chain.
add_test(NAME launch.parts_include_check_rejects_a_forbidden_include
         COMMAND ${_launch_include_check} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/include_fixtures/direct -DDIR=launch/src
                 -DFORBIDDEN_PREFIXES=silkit/,vehicle_sil/launch/run. ${_launch_include_script})
set_tests_properties(launch.parts_include_check_rejects_a_forbidden_include PROPERTIES LABELS "unit;launch" TIMEOUT 60
                     PASS_REGULAR_EXPRESSION "chain\\.cpp: request/shared\\.hpp -> silkit/SilKit\\.hpp.*pure\\.cpp: helper\\.hpp -> silkit/SilKit\\.hpp"
                     FAIL_REGULAR_EXPRESSION "clean\\.hpp:")
# The files that read the configuration do not reach the scenario
# (vehicle_sil/scenario/): the scenario depends on the configuration, not the other way
# round. request/ is guarded by launch.request_boundary; connections.hpp is the one use-case header
# that reads the configuration's values.
add_test(NAME launch.config_files_do_not_reach_the_scenario
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
                 -DFILES=libs/launch/include/vehicle_sil/launch/connections.hpp
                 -DFORBIDDEN_PREFIXES=vehicle_sil/scenario/
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
set_tests_properties(launch.config_files_do_not_reach_the_scenario PROPERTIES LABELS "unit;launch" TIMEOUT 60)
add_test(NAME launch.config_files_check_rejects_a_scenario_include
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/include_fixtures/scenario
                 -DFILES=launch/src/config.cpp -DFORBIDDEN_PREFIXES=vehicle_sil/scenario/
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
set_tests_properties(launch.config_files_check_rejects_a_scenario_include PROPERTIES LABELS "unit;launch" TIMEOUT 60
                     PASS_REGULAR_EXPRESSION "config\\.cpp: vehicle_sil/scenario/scenario_x\\.hpp")

# vehicle_sil_launch check as a process: the examples of docs/simulation-*.md with the ECU
# executables of examples/ecu_main, executables that fail --describe (the helper of
# tests/process), --strict, files that cannot be read, the command line.
if(TARGET vehicle_sil_launch AND TARGET vehicle_sil_engine_ecu)
  add_executable(vehicle_sil_launch_cli_launcher launch_cli_launcher.cpp)
  target_link_libraries(vehicle_sil_launch_cli_launcher PRIVATE vehicle_sil_test_process)
  vehicle_sil_target_defaults(vehicle_sil_launch_cli_launcher)
  add_dependencies(vehicle_sil_launch_cli_launcher vehicle_sil_launch vehicle_sil_process_helper vehicle_sil_engine_ecu
                   vehicle_sil_dashboard_ecu)
  # For the scenario connect: vehicle_sil_launch and the two bridges under the names the launcher
  # looks for, in one directory (the bridges are the production main() on the fake pcap library
  # and on the fake XL driver: their --describe and their command line are the real ones). On
  # Windows the DLLs they need go there too.
  set(_connect_tools ${CMAKE_CURRENT_BINARY_DIR}/connect_tools)
  set(_connect_copies vehicle_sil_launch vehicle_sil_ethernet_bridge_on_fake_pcap vehicle_sil_vector_xl_bridge_on_fake)
  set(_connect_dlls)
  if(WIN32)
    foreach(_copy IN LISTS _connect_copies)
      list(APPEND _connect_dlls
           COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${_copy}>>,copy_if_different,true>
                   $<TARGET_RUNTIME_DLLS:${_copy}> ${_connect_tools})
    endforeach()
  endif()
  add_custom_target(vehicle_sil_launch_connect_tools
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_connect_tools}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:vehicle_sil_launch>
            ${_connect_tools}/vehicle_sil_launch$<TARGET_FILE_SUFFIX:vehicle_sil_launch>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:vehicle_sil_ethernet_bridge_on_fake_pcap>
            ${_connect_tools}/vehicle_sil_ethernet_bridge$<TARGET_FILE_SUFFIX:vehicle_sil_ethernet_bridge_on_fake_pcap>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:vehicle_sil_vector_xl_bridge_on_fake>
            ${_connect_tools}/vehicle_sil_vector_xl_bridge$<TARGET_FILE_SUFFIX:vehicle_sil_vector_xl_bridge_on_fake>
    ${_connect_dlls}
    COMMAND_EXPAND_LISTS
    VERBATIM)
  add_dependencies(vehicle_sil_launch_connect_tools ${_connect_copies})
  add_dependencies(vehicle_sil_launch_cli_launcher vehicle_sil_launch_connect_tools vehicle_sil_fake_pcap)
  vehicle_sil_add_scenario_tests(vehicle_sil_launch_cli_launcher
    PREFIX launch_cli LABELS integration launch TIMEOUT 120
    WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/cli
    SCENARIOS
      compare
      connect
      connections
      describe
      descriptions
      docs
      enabled
      file_only
      request
      strict
      unreadable
      usage
    ARGS --launch $<TARGET_FILE:vehicle_sil_launch>
         --connect-tools ${_connect_tools}
         --fake-pcap-dir ${VEHICLE_SIL_FAKE_PCAP_DIR}
         --helper $<TARGET_FILE:vehicle_sil_process_helper>
         --engine $<TARGET_FILE:vehicle_sil_engine_ecu>
         --example-dir $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..
         --example-config ${PROJECT_SOURCE_DIR}/examples/ecu_main/simulation.yaml
         --docs ${VEHICLE_SIL_TESTED_DOCS})
endif()

# The example of the guide docs/howto/integrate-ecus.md (examples/integrate_ecus: two ECUs that name one
# bus differently, and the same two after one was built again): the files, --describe and the
# launcher's outputs must be what the guide shows.
if(TARGET vehicle_sil_launch AND TARGET vehicle_sil_dashboard_v1_ecu)
  add_test(NAME launch_docs.integrate_ecus
           COMMAND ${CMAKE_COMMAND} -DLAUNCH=$<TARGET_FILE:vehicle_sil_launch>
                   -DDASHBOARD_V1=$<TARGET_FILE:vehicle_sil_dashboard_v1_ecu>
                   -DDIR=$<TARGET_FILE_DIR:vehicle_sil_dashboard_v1_ecu>/..
                   -DEXAMPLE=${PROJECT_SOURCE_DIR}/examples/integrate_ecus
                   -DDOC=${VEHICLE_SIL_TESTED_DOCS}/howto/integrate-ecus.md
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/integrate_ecus.cmake)
  set_tests_properties(launch_docs.integrate_ecus PROPERTIES LABELS "integration;launch" TIMEOUT 300)
endif()

# vehicle_sil_launch run as a process: the example of examples/ecu_main and docs/simulation-*.md,
# and an ecu_main executable that misbehaves as a file in its working directory says
# (run_test_ecu.cpp), with the helper of tests/process.
if(TARGET vehicle_sil_launch AND TARGET vehicle_sil_engine_ecu)
  add_executable(vehicle_sil_launch_test_ecu run_test_ecu.cpp)
  target_include_directories(vehicle_sil_launch_test_ecu PRIVATE ${PROJECT_SOURCE_DIR}/tests/process)
  target_link_libraries(vehicle_sil_launch_test_ecu PRIVATE vehicle_sil::simulation vehicle_sil_test_support)
  vehicle_sil_target_defaults(vehicle_sil_launch_test_ecu)
  vehicle_sil_copy_runtime_dlls(vehicle_sil_launch_test_ecu)

  # The scenarios by topic (launch_run_<topic>.cpp); launch_run_launcher.cpp has main().
  add_executable(vehicle_sil_launch_run_launcher
    launch_run_availability.cpp
    launch_run_connect.cpp
    launch_run_events.cpp
    launch_run_launcher.cpp
    launch_run_manual.cpp
    launch_run_networks.cpp
    launch_run_processes.cpp
    launch_run_samples.cpp
    launch_run_scenario.cpp
    launch_run_scenario_time.cpp
    launch_run_support.cpp
    launch_run_time.cpp
    launch_run_timeline.cpp
    launch_run_variants.cpp)
  target_include_directories(vehicle_sil_launch_run_launcher PRIVATE ${PROJECT_SOURCE_DIR}/tests/process)
  target_link_libraries(vehicle_sil_launch_run_launcher PRIVATE vehicle_sil_test_process)
  if(WIN32)
    # CommandLineToArgvW: the scenario manual splits a command line as a person's shell would, and
    # finds a free port for a registry.
    target_link_libraries(vehicle_sil_launch_run_launcher PRIVATE shell32 ws2_32)
  endif()
  vehicle_sil_target_defaults(vehicle_sil_launch_run_launcher)
  add_dependencies(vehicle_sil_launch_run_launcher vehicle_sil_launch vehicle_sil_launch_test_ecu vehicle_sil_process_helper
                   vehicle_sil_engine_ecu vehicle_sil_dashboard_ecu)
  if(TARGET vehicle_sil_eth_sensor_ecu)
    # The scenario ethernet: examples/ethernet, next to examples/ecu_main in the build tree.
    add_dependencies(vehicle_sil_launch_run_launcher vehicle_sil_eth_sensor_ecu vehicle_sil_eth_display_ecu)
  endif()
  vehicle_sil_add_scenario_tests(vehicle_sil_launch_run_launcher
    PREFIX launch_run LABELS integration launch TIMEOUT 300
    WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/run
    SCENARIOS
      availability
      busy_port
      check
      control
      crash
      disabled
      docs
      ended_early
      ethernet
      events
      events_findings
      events_nonblocking
      events_output
      events_slow
      events_stalled
      events_stderr
      events_usage
      grandchild
      hang
      interrupt
      interrupt_ending
      interrupt_twice
      j1939_nodes
      killed
      late
      live
      manual_again
      manual_fails
      manual_frozen
      manual_late
      manual_stop
      manual_stranger
      manual_thawed
      manual_timeout
      network
      network_names
      no_console
      no_console_stdin
      not_joined
      optional
      output_closed
      path
      pause_step
      paused_failures
      quitter
      realtime
      scenario
      scenario_disconnect
      scenario_dither
      scenario_each_write
      scenario_expect_beside
      scenario_failures
      scenario_paused_timeout
      scenario_same
      scenario_samples
      scenario_time_commands_same
      scenario_wave
      run_until_stdin
      state_on_arrival
      speed_change
      stall
      stand_still
      stdin_ending
      stdin_ending_optional
      stdin_eof
      stdin_killed
      stdin_stop
      stdin_twice
      step
      step_width
      stop_once
      test_command
      test_interrupt
      time_usage
      timeline
      timeout
      traffic
      usage
      variable_map
      variants
    ARGS --launch $<TARGET_FILE:vehicle_sil_launch>
         --test-ecu $<TARGET_FILE:vehicle_sil_launch_test_ecu>
         --helper $<TARGET_FILE:vehicle_sil_process_helper>
         --example-dir $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..
         --docs ${VEHICLE_SIL_TESTED_DOCS})
  # run --connect: the bridges of connect_tools (above) on the fake pcap library and the fake XL
  # driver, with the Ethernet ECU of the bridge's tests.
  add_dependencies(vehicle_sil_launch_run_launcher vehicle_sil_launch_connect_tools vehicle_sil_ethernet_bridge_it_ecu
                   vehicle_sil_fake_pcap sil-kit-registry)
  vehicle_sil_add_scenario_tests(vehicle_sil_launch_run_launcher
    PREFIX launch_run LABELS integration launch TIMEOUT 300
    WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/run
    SCENARIOS
      connect_bus_off
      connect_bus_off_lost
      connect_device_shared
      connect_device_shared_later
      connect_device_shared_own
      connect_dies_while_held
      connect_dies_while_waiting
      connect_lost_exit0
      connect_lost_kill
      connect_no_adapter
      connect_nothing_received
      connect_request
      connect_roundtrip
      connect_speed
      connect_time_changed
      connect_time_locked
    ARGS --launch $<TARGET_FILE:vehicle_sil_launch>
         --test-ecu $<TARGET_FILE:vehicle_sil_launch_test_ecu>
         --helper $<TARGET_FILE:vehicle_sil_process_helper>
         --example-dir $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..
         --docs ${VEHICLE_SIL_TESTED_DOCS}
         --connect-tools ${_connect_tools}
         --bridge-ecu $<TARGET_FILE:vehicle_sil_ethernet_bridge_it_ecu>
         --fake-pcap-dir ${VEHICLE_SIL_FAKE_PCAP_DIR}
         --registry $<TARGET_FILE:sil-kit-registry>)
  # The guide docs/howto/scenario-tests.md: what its reader does with the two ECUs of examples/ecu_main
  # (the scenarios that come with them, the ones of examples/scenario_tests the guide has the reader
  # write, check, run and test), in the guide's order; the files, the outputs and the exit codes must
  # be what the guide shows.
  add_test(NAME launch_run.scenario_guide
           COMMAND ${CMAKE_COMMAND} -DLAUNCH=$<TARGET_FILE:vehicle_sil_launch>
                   -DECU_MAIN=$<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..
                   -DECU_MAIN_SOURCE=${PROJECT_SOURCE_DIR}/examples/ecu_main
                   -DEXAMPLE=${PROJECT_SOURCE_DIR}/examples/scenario_tests
                   -DDOC=${VEHICLE_SIL_TESTED_DOCS}/howto/scenario-tests.md
                   -DWORK=${CMAKE_CURRENT_BINARY_DIR}/scenario_guide
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/scenario_guide.cmake)
  set_tests_properties(launch_run.scenario_guide PROPERTIES LABELS "integration;launch" TIMEOUT 300 ${_vehicle_sil_tsan_processors} COST 6)
  # The profiling trace of a whole run (docs/internals/trace.md): the files of the launcher and the two
  # ECUs, written at exit, with their records; scripts/trace-report.py summarises them when there is a
  # Python.
  add_test(NAME launch_run.trace_files
           COMMAND ${CMAKE_COMMAND} -DLAUNCH=$<TARGET_FILE:vehicle_sil_launch>
                   -DECU_MAIN=$<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..
                   -DWORK=${CMAKE_CURRENT_BINARY_DIR}/trace_files
                   -DSCRIPT=${PROJECT_SOURCE_DIR}/scripts/trace-report.py
                   -DPYTHON=${VEHICLE_SIL_J1939_CODEGEN_PYTHON}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/trace_files.cmake)
  set_tests_properties(launch_run.trace_files PROPERTIES LABELS "integration;launch" TIMEOUT 300)
  # The example of the docs with a participant started by hand: its registry is at 8500, the port
  # of such a run, which the docs show. Builds in other working trees on this machine take turns.
  vehicle_sil_add_scenario_tests(vehicle_sil_launch_run_launcher
    PREFIX launch_run LABELS integration launch TIMEOUT 300
    WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/run
    SCENARIOS manual
    ARGS --launch $<TARGET_FILE:vehicle_sil_launch>
         --test-ecu $<TARGET_FILE:vehicle_sil_launch_test_ecu>
         --helper $<TARGET_FILE:vehicle_sil_process_helper>
         --example-dir $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..
         --docs ${VEHICLE_SIL_TESTED_DOCS}
    MACHINE_LOCK port-8500)
  # Alone: it says that an idle simulation is not late at speed 1 within the default tolerance,
  # which is a bound on real time (the other scenarios have bounds on one side only).
  vehicle_sil_add_scenario_tests(vehicle_sil_launch_run_launcher
    PREFIX launch_run LABELS integration launch TIMEOUT 300
    WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/run
    SCENARIOS on_time
    ARGS --launch $<TARGET_FILE:vehicle_sil_launch>
         --test-ecu $<TARGET_FILE:vehicle_sil_launch_test_ecu>
         --helper $<TARGET_FILE:vehicle_sil_process_helper>
         --example-dir $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..
         --docs ${VEHICLE_SIL_TESTED_DOCS}
    RUN_SERIAL)
  # Start the longest multi-ECU scenario tests early on parallel CTest runs.
  set_tests_properties(launch_run.manual_frozen PROPERTIES COST 32)
  set_tests_properties(launch_run.connect_bus_off_lost PROPERTIES COST 30)
  set_tests_properties(launch_run.scenario_time_commands_same PROPERTIES COST 22)
  set_tests_properties(launch_run.connect_device_shared_later PROPERTIES COST 12)
  set_tests_properties(launch_run.stand_still PROPERTIES COST 11)
  set_tests_properties(launch_run.manual_late launch_run.events_stalled launch_run.manual PROPERTIES COST 10)
  set_tests_properties(launch_run.not_joined PROPERTIES COST 9)
  set_tests_properties(launch_run.paused_failures launch_run.stall PROPERTIES COST 8)
  set_tests_properties(launch_run.scenario_failures launch_run.timeout launch_run.control
                       launch_run.connect_nothing_received launch_run.timeline PROPERTIES COST 6)
  # Under TSan, throttle concurrency (PROCESSORS 2) only on genuinely CPU/multi-ECU-heavy
  # scenarios; sleep/timeout-bound and short CLI scenarios use 1 slot so CI cores stay busy.
  if(_vehicle_sil_tsan_processors)
    set_tests_properties(
      launch_run.docs
      launch_run.traffic
      launch_run.scenario_time_commands_same
      launch_run.scenario_failures
      launch_run.scenario_samples
      launch_run.scenario_wave
      launch_run.timeline
      launch_run.connect_time_locked
      launch_run.connect_speed
      launch_run.connect_roundtrip
      launch_run.paused_failures
      launch_run.events_slow
      launch_run.events_stalled
      PROPERTIES ${_vehicle_sil_tsan_processors})
  endif()
endif()

# The run of vehicle_sil_launch as a library, whose hub the caller keeps from the start window on:
# a parameter written in the window before the start is what each ECU's process sees in its step
# at 0, and the values the hub has at the end are those of the last steps.
if(TARGET vehicle_sil_launch_test_ecu)
  add_executable(vehicle_sil_launch_control_test launch_control_test.cpp)
  target_link_libraries(vehicle_sil_launch_control_test PRIVATE vehicle_sil::launch vehicle_sil::control SilKit::SilKit
                                                          vehicle_sil_test_support)
  # The names of the topics (a hand-made agent says hello and writes what is no message).
  target_include_directories(vehicle_sil_launch_control_test PRIVATE ${PROJECT_SOURCE_DIR}/libs/control_silkit/src)
  vehicle_sil_target_defaults(vehicle_sil_launch_control_test)
  vehicle_sil_copy_runtime_dlls(vehicle_sil_launch_control_test)
  add_dependencies(vehicle_sil_launch_control_test vehicle_sil_launch_test_ecu vehicle_sil_process_helper)
  # Each part is a test of its own, so that they run side by side: the write in the window
  # (5 runs), what fails before the start (also with a stop), an agent that says hello and does
  # not answer, an ECU without variables that is waited for, a variable map that does not fit an agent whose hello comes late, the limits of the start before and after the hold of --start-paused with a
  # control server (a participant of the test that never gets ready), a hold longer than the
  # ECUs' own limit for the start, --timeout-ms and Ctrl+C in
  # the hold, a resume before the window, when the control server is started and when it is not,
  # what its start throws out of run_file(), a request for a control server that is wrong.
  _vehicle_sil_scaled_ctest_timeout(_launch_control_timeout 300)
  foreach(part IN ITEMS a_write_in_the_start_window_reaches_each_process_before_its_first_step
                        what_fails_before_the_start_aborts_the_simulation
                        a_stop_while_what_is_done_before_the_start_fails_does_not_cut_the_end_short
                        a_hello_without_an_answer_and_what_is_no_message_are_reported
                        an_ecu_without_variables_is_waited_for
                        a_mapping_that_does_not_fit_is_reported_for_a_hello_after_the_look_in_the_window
                        a_mapping_that_does_not_fit_is_reported_for_a_hello_after_the_start
                        the_start_is_limited_before_and_after_the_hold_but_not_in_it
                        the_ecus_wait_as_long_as_the_run_stands_still
                        timeout_counts_the_hold an_interrupt_ends_the_hold a_resume_before_the_window_means_no_hold
                        a_refused_scenario_does_not_hold
                        the_control_server_is_started_after_the_runs_files_and_before_the_registry
                        what_leaves_run_file_between_the_stages_ends_the_time_commands
                        a_request_for_a_control_server_is_read_before_any_check)
    add_test(NAME launch_control.${part}
             COMMAND vehicle_sil_launch_control_test $<TARGET_FILE:vehicle_sil_launch_test_ecu>
                     ${CMAKE_CURRENT_BINARY_DIR}/control $<TARGET_FILE:vehicle_sil_process_helper> ${part})
    set_tests_properties(launch_control.${part} PROPERTIES LABELS "integration;launch;control" TIMEOUT ${_launch_control_timeout})
    _vehicle_sil_tell_ctest_timeout_factor(launch_control.${part})
  endforeach()
  set_tests_properties(launch_control.the_ecus_wait_as_long_as_the_run_stands_still PROPERTIES COST 15)
  set_tests_properties(launch_control.a_write_in_the_start_window_reaches_each_process_before_its_first_step
                       PROPERTIES COST 12 ${_vehicle_sil_tsan_processors})
  set_tests_properties(launch_control.the_start_is_limited_before_and_after_the_hold_but_not_in_it PROPERTIES COST 15)
endif()
