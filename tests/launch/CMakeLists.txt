# vehicle_sil_launch: reading the simulation configuration, the rules of the check (pure
# functions: no process), the output for people and as JSON.
if(NOT TARGET vehicle_sil::launch)
  return()
endif()
vehicle_sil_add_harness_tests(vehicle_sil_launch_tests
  PREFIX launch LABELS unit launch TIMEOUT 60
  SOURCES
    test_batch.cpp
    test_check.cpp
    test_config.cpp
    test_connect.cpp
    test_connections.cpp
    test_control_rules.cpp
    test_events.cpp
    test_j1939_nodes_match.cpp
    test_run.cpp
    test_run_controls.cpp
    test_run_outside.cpp
    test_scenario.cpp
    test_scenario_line.cpp
    test_shared_values.cpp
    test_stop_requests.cpp
    test_traffic.cpp
  LINK vehicle_sil::launch vehicle_sil::core # core: the time commands with a controller (test_events.cpp)
       vehicle_sil::control # the hub that the run's samples come from (test_traffic.cpp)
       vehicle_sil::control_silkit # the hub's transport of a real connection (test_run.cpp)
       vehicle_sil_cli # the other tools' exit codes and durations (test_shared_values.cpp)
       vehicle_sil::signal # the shapes as the launcher makes them (test_scenario_line.cpp: scenario_shapes.hpp)
       vehicle_sil::j1939 # the network that applies j1939_nodes too (test_j1939_nodes_match.cpp)
       # the statistics lines as the bridges print them, which run reads (test_run_outside.cpp)
       vehicle_sil::ethernet_bridge_core vehicle_sil::can_bridge_core)
# libs/launch/src: the run's parts that have no dependencies and are tested as written (run_start_limit.hpp, run_pacing.hpp).
target_include_directories(vehicle_sil_launch_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/libs/launch/src)
target_compile_definitions(vehicle_sil_launch_tests PRIVATE VEHICLE_SIL_LAUNCH_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
                           # test_shared_values.cpp reads the GUI's sources.
                           VEHICLE_SIL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# The sources of the launcher (its library and its tool) read the networks of a description in
# one place (seen_networks(), which applies a participant's network_names and j1939_nodes; the J1939
# nodes are members of the networks, so this keeps their values in the code from the rules too), and where --describe's
# JSON is read and written back; the check itself must reject a read anywhere else (also one
# through a pointer to the member), and pass the networks of the file.
add_test(NAME launch.described_networks_are_read_in_one_place
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DDIR=libs/launch,tools/launch
                 -DFILES=libs/launch/src/check_networks.cpp,libs/launch/src/describe_read.cpp
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

# The files of the launcher that make up its pure layer (the configuration, the checks and the
# scenario) include no SIL Kit header, no process header, no run, events, time-command or
# stop-request header (the run's controls are a run header),
# no probe.hpp (what looks at the computer),
# directly or through the headers they include (the private ones of libs/launch/src too). Add a
# file to the list when it no longer includes them. The check itself must reject a fixture that
# reaches them, and name the chain.
set(_launch_pure_files
    libs/launch/src/yaml_reader.hpp libs/launch/src/yaml_tree.hpp libs/launch/src/check_rules.cpp
    libs/launch/src/config.cpp libs/launch/src/connect.cpp libs/launch/src/check_connect.cpp
    libs/launch/src/scenario_csv.cpp libs/launch/src/scenario_csv.hpp
    libs/launch/src/time_plan.cpp libs/launch/include/vehicle_sil/launch/time_plan.hpp
    libs/launch/src/timeline.cpp libs/launch/include/vehicle_sil/launch/timeline_format.hpp
    libs/launch/include/vehicle_sil/launch/check.hpp libs/launch/include/vehicle_sil/launch/connections.hpp
    libs/launch/src/describe_read.cpp libs/launch/src/seen_ecu.cpp libs/launch/src/seen_ecu.hpp
    libs/launch/src/check_words.hpp libs/launch/src/scenario.cpp libs/launch/src/scenario_reader.hpp
    libs/launch/src/scenario_values.cpp libs/launch/src/scenario_waves.cpp libs/launch/src/scenario_inputs.cpp
    libs/launch/src/scenario_shapes.cpp libs/launch/src/scenario_shapes.hpp libs/launch/src/scenario_line.cpp
    libs/launch/src/scenario_line.hpp libs/launch/src/scenario_run.cpp libs/launch/src/scenario_failure.cpp
    libs/launch/src/scenario_failure.hpp libs/launch/include/vehicle_sil/launch/config.hpp
    libs/launch/include/vehicle_sil/launch/scenario.hpp libs/launch/include/vehicle_sil/launch/scenario_run.hpp
    libs/launch/include/vehicle_sil/launch/timeline.hpp libs/launch/include/vehicle_sil/launch/connect.hpp
    libs/launch/include/vehicle_sil/launch/traffic.hpp libs/launch/include/vehicle_sil/launch/command.hpp
    libs/launch/src/check_format.hpp libs/launch/src/check_format.cpp libs/launch/src/check_networks.cpp
    libs/launch/src/timeline_format.cpp
    libs/launch/src/control_rules.cpp libs/launch/include/vehicle_sil/launch/control_rules.hpp)
list(JOIN _launch_pure_files "," _launch_pure_files_arg)
add_test(NAME launch.pure_files_include_no_runtime
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DFILES=${_launch_pure_files_arg}
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
add_test(NAME launch.pure_files_check_rejects_a_forbidden_include
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/include_fixtures/direct
                 -DFILES=launch/src/pure.cpp,launch/src/clean.hpp
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
set_tests_properties(launch.pure_files_include_no_runtime launch.pure_files_check_rejects_a_forbidden_include
                     PROPERTIES LABELS "unit;launch" TIMEOUT 60)
set_tests_properties(launch.pure_files_check_rejects_a_forbidden_include PROPERTIES
                     PASS_REGULAR_EXPRESSION "pure\\.cpp: helper\\.hpp -> silkit/SilKit\\.hpp"
                     FAIL_REGULAR_EXPRESSION "clean\\.hpp:")

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
      connect
      connections
      describe
      docs
      enabled
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
  target_link_libraries(vehicle_sil_launch_test_ecu PRIVATE vehicle_sil::simulation)
  vehicle_sil_target_defaults(vehicle_sil_launch_test_ecu)
  vehicle_sil_copy_runtime_dlls(vehicle_sil_launch_test_ecu)

  # The scenarios by topic (launch_run_<topic>.cpp); launch_run_launcher.cpp has main().
  add_executable(vehicle_sil_launch_run_launcher
    launch_run_connect.cpp
    launch_run_events.cpp
    launch_run_launcher.cpp
    launch_run_manual.cpp
    launch_run_networks.cpp
    launch_run_processes.cpp
    launch_run_scenario.cpp
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
      scenario_same
      scenario_wave
      run_until_stdin
      speed_change
      stall
      stand_still
      stdin_ending
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
      connect_dies_while_waiting
      connect_lost_exit0
      connect_lost_kill
      connect_no_adapter
      connect_nothing_received
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
  set_tests_properties(launch_run.scenario_guide PROPERTIES LABELS "integration;launch" TIMEOUT 300)
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
endif()

# The run of vehicle_sil_launch as a library, whose hub the caller keeps from the start window on:
# a parameter written in the window before the start is what each ECU's process sees in its step
# at 0, and the values the hub has at the end are those of the last steps.
if(TARGET vehicle_sil_launch_test_ecu)
  add_executable(vehicle_sil_launch_control_test launch_control_test.cpp)
  target_link_libraries(vehicle_sil_launch_control_test PRIVATE vehicle_sil::launch vehicle_sil::control SilKit::SilKit)
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
  # what its start throws out of run_file().
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
                        the_control_server_is_started_after_the_runs_files_and_before_the_registry
                        what_leaves_run_file_between_the_stages_ends_the_time_commands)
    add_test(NAME launch_control.${part}
             COMMAND vehicle_sil_launch_control_test $<TARGET_FILE:vehicle_sil_launch_test_ecu>
                     ${CMAKE_CURRENT_BINARY_DIR}/control $<TARGET_FILE:vehicle_sil_process_helper> ${part})
    set_tests_properties(launch_control.${part} PROPERTIES LABELS "integration;launch;control" TIMEOUT 300)
  endforeach()
endif()
