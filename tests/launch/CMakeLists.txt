# vehicle_sil_launch: reading the simulation configuration, the rules of the check (pure
# functions: no process), the output for people and as JSON.
if(NOT TARGET vehicle_sil::launch)
  return()
endif()
vehicle_sil_add_harness_tests(vehicle_sil_launch_tests
  PREFIX launch LABELS unit launch TIMEOUT 60
  SOURCES test_config.cpp test_check.cpp test_run.cpp test_events.cpp test_scenario.cpp
  LINK vehicle_sil::launch vehicle_sil::core) # core: the time commands with a controller (test_events.cpp)
target_include_directories(vehicle_sil_launch_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(vehicle_sil_launch_tests PRIVATE VEHICLE_SIL_LAUNCH_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch")

# vehicle_sil_launch check as a process: the examples of docs/simulation-config.md with the ECU
# executables of examples/ecu_main, executables that fail --describe (the helper of
# tests/process), --strict, files that cannot be read, the command line.
if(TARGET vehicle_sil_launch AND TARGET vehicle_sil_engine_ecu)
  add_executable(vehicle_sil_launch_cli_launcher launch_cli_launcher.cpp)
  target_link_libraries(vehicle_sil_launch_cli_launcher PRIVATE vehicle_sil_test_process)
  vehicle_sil_target_defaults(vehicle_sil_launch_cli_launcher)
  add_dependencies(vehicle_sil_launch_cli_launcher vehicle_sil_launch vehicle_sil_process_helper vehicle_sil_engine_ecu
                   vehicle_sil_dashboard_ecu)
  vehicle_sil_add_scenario_tests(vehicle_sil_launch_cli_launcher
    PREFIX launch_cli LABELS integration launch TIMEOUT 120
    WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/cli
    SCENARIOS docs describe strict unreadable usage enabled
    ARGS --launch $<TARGET_FILE:vehicle_sil_launch>
         --helper $<TARGET_FILE:vehicle_sil_process_helper>
         --engine $<TARGET_FILE:vehicle_sil_engine_ecu>
         --example-dir $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..
         --example-config ${PROJECT_SOURCE_DIR}/examples/ecu_main/simulation.yaml
         --docs ${PROJECT_SOURCE_DIR}/docs/simulation-config.md)
endif()

# vehicle_sil_launch run as a process: the example of examples/ecu_main and docs/simulation-config.md,
# and an ecu_main executable that misbehaves as a file in its working directory says
# (run_test_ecu.cpp), with the helper of tests/process.
if(TARGET vehicle_sil_launch AND TARGET vehicle_sil_engine_ecu)
  add_executable(vehicle_sil_launch_test_ecu run_test_ecu.cpp)
  target_include_directories(vehicle_sil_launch_test_ecu PRIVATE ${PROJECT_SOURCE_DIR}/tests/process)
  target_link_libraries(vehicle_sil_launch_test_ecu PRIVATE vehicle_sil::simulation)
  vehicle_sil_target_defaults(vehicle_sil_launch_test_ecu)
  vehicle_sil_copy_runtime_dlls(vehicle_sil_launch_test_ecu)

  add_executable(vehicle_sil_launch_run_launcher launch_run_launcher.cpp)
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
  vehicle_sil_add_scenario_tests(vehicle_sil_launch_run_launcher
    PREFIX launch_run LABELS integration launch TIMEOUT 300
    WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/run
    SCENARIOS docs check crash step optional not_joined quitter hang grandchild path interrupt interrupt_twice
              interrupt_ending ended_early timeout no_console killed usage disabled events events_usage stdin_stop
              stdin_eof stdin_twice stdin_ending output_closed stdin_killed no_console_stdin stop_once events_stalled
              events_slow events_findings events_nonblocking events_output events_stderr realtime late stall
              speed_change step_width pause_step paused_failures time_usage control scenario scenario_same
              scenario_disconnect scenario_failures manual
    ARGS --launch $<TARGET_FILE:vehicle_sil_launch>
         --test-ecu $<TARGET_FILE:vehicle_sil_launch_test_ecu>
         --helper $<TARGET_FILE:vehicle_sil_process_helper>
         --example-dir $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..
         --docs ${PROJECT_SOURCE_DIR}/docs/simulation-config.md)
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
         --docs ${PROJECT_SOURCE_DIR}/docs/simulation-config.md
    RUN_SERIAL)
endif()

# The run of vehicle_sil_launch as a library with a hub of the caller: a parameter written in the
# window before the start is what each ECU's process sees in its step at 0, and the values the
# hub has at the end are those of the last steps.
if(TARGET vehicle_sil_launch_test_ecu)
  add_executable(vehicle_sil_launch_control_test launch_control_test.cpp)
  target_link_libraries(vehicle_sil_launch_control_test PRIVATE vehicle_sil::launch vehicle_sil::control SilKit::SilKit)
  # The names of the topics (a hand-made agent says hello and writes what is no message).
  target_include_directories(vehicle_sil_launch_control_test PRIVATE ${PROJECT_SOURCE_DIR}/libs/control_silkit/src)
  vehicle_sil_target_defaults(vehicle_sil_launch_control_test)
  vehicle_sil_copy_runtime_dlls(vehicle_sil_launch_control_test)
  add_dependencies(vehicle_sil_launch_control_test vehicle_sil_launch_test_ecu vehicle_sil_process_helper)
  # Also: the limits of the start before and after the hold of --start-paused with a control
  # server (a participant of the test that never gets ready), --timeout-ms and Ctrl+C in the
  # hold, a resume before the window.
  add_test(NAME launch_control.a_write_in_the_start_window_reaches_each_process_before_its_first_step
           COMMAND vehicle_sil_launch_control_test $<TARGET_FILE:vehicle_sil_launch_test_ecu>
                   ${CMAKE_CURRENT_BINARY_DIR}/control $<TARGET_FILE:vehicle_sil_process_helper>)
  set_tests_properties(launch_control.a_write_in_the_start_window_reaches_each_process_before_its_first_step PROPERTIES
    LABELS "integration;launch;control" TIMEOUT 300)
endif()
