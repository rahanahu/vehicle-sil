# The control server of vehicle_sil_launch run --control (libs/control_grpc, gRPC). Added only with
# VEHICLE_SIL_BUILD_CONTROL_SERVER.
if(NOT VEHICLE_SIL_BUILD_CONTROL_SERVER)
  return()
endif()
vehicle_sil_add_harness_tests(vehicle_sil_control_server_tests
  PREFIX control_server LABELS unit control control_server TIMEOUT 120
  SOURCES
    test_authorization.cpp
    test_authorization_rules.cpp
    test_availability.cpp
    test_control_server.cpp
    test_convert.cpp
    test_grpc_ping.cpp
    test_limits.cpp
    test_observe_only.cpp
    test_run_binding.cpp
    test_security.cpp
    test_simulation_monitor.cpp
    test_snapshot.cpp
    test_watch.cpp
  LINK vehicle_sil::control_proto vehicle_sil::control_grpc vehicle_sil::control_silkit vehicle_sil::peer)
target_include_directories(vehicle_sil_control_server_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/control_grpc/src)
target_compile_definitions(vehicle_sil_control_server_tests PRIVATE
  VEHICLE_SIL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

# The Python client and CLI against vehicle_sil_launch run --control, with a Python 3.11+ that has grpcio and grpcio-tools.
# VEHICLE_SIL_CONTROL_PYTHON may name it; otherwise the repository's uv environment (.venv) if it has
# them, else the Python 3 found here (cmake/VehicleSilPython.cmake).
if(TARGET vehicle_sil_launch AND TARGET vehicle_sil_engine_ecu AND TARGET vehicle_sil_dashboard_ecu)
  set(_stubs "${CMAKE_CURRENT_BINARY_DIR}/control_python_stubs")
  vehicle_sil_add_python_grpc_stubs(vehicle_sil_control_python_stubs
    ALL
    OUT "${_stubs}"
    PYTHON_VAR _control_python
    PYTHONPATH "${PROJECT_SOURCE_DIR}/python"
    DEPENDS vehicle_sil_launch vehicle_sil_engine_ecu vehicle_sil_dashboard_ecu)
  if(_control_python)
    set(_example_dir "$<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..")
    _vehicle_sil_scaled_ctest_timeout(_control_example_timeout 120)
    _vehicle_sil_scaled_ctest_timeout(_control_python_client_timeout 180)
    _vehicle_sil_scaled_ctest_timeout(_control_launcher_timeout 300)
    # The client and the CLI under the launcher with the ECUs of examples/ecu_main; what the client
    # makes of the server's messages.
    add_test(NAME control_server.python_client
             COMMAND "${_control_python}" -m unittest -v test_python_client
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(control_server.python_client PROPERTIES
      LABELS "control;control_server;python" TIMEOUT ${_control_python_client_timeout} ${_vehicle_sil_tsan_processors} COST 11
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs};VEHICLE_SIL_LAUNCH_EXE=$<TARGET_FILE:vehicle_sil_launch>;VEHICLE_SIL_EXAMPLE_DIR=${_example_dir};VEHICLE_SIL_LAUNCH_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/python_client_scratch;VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=${VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT}")
    # vehicle_sil_launch run --control with the ECUs of examples/ecu_main, driven from Python;
    # split across 2 parallel CTest entries with isolated scratch directories.
    add_test(NAME control_server.launcher
             COMMAND "${_control_python}" -m unittest -v
               test_launcher.LauncherTest.test_a_write_in_the_hold_and_the_commands_of_both_sources
               test_launcher.LauncherTest.test_the_time_events_of_a_step_from_the_hold_begin_at_0_and_say_where_it_stands
               test_launcher.LauncherTest.test_a_variable_map_is_what_a_client_sees_and_writes
               test_launcher.LauncherTest.test_a_snapshot_has_the_time_and_the_values_of_one_moment
               test_launcher.LauncherTest.test_calls_that_wait_end_with_the_run
               test_launcher.LauncherTest.test_calls_that_wait_end_on_a_timeout
               test_launcher.LauncherTest.test_calls_that_wait_end_when_a_participant_fails
               test_launcher.LauncherTest.test_a_participant_that_fails_in_the_hold_ends_the_start_at_once
               test_launcher.LauncherTest.test_a_stop_on_standard_input_in_the_hold
               test_launcher.LauncherTest.test_credentials_roles_tls_audit_and_no_agents
               test_launcher.LauncherTest.test_a_unix_socket
               test_launcher.LauncherTest.test_an_earlier_runs_client_at_a_fixed_address_is_refused
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(control_server.launcher PROPERTIES
      LABELS "control;control_server;python;launch;integration" TIMEOUT ${_control_launcher_timeout} ${_vehicle_sil_tsan_processors} COST 12
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs};VEHICLE_SIL_LAUNCH_EXE=$<TARGET_FILE:vehicle_sil_launch>;VEHICLE_SIL_EXAMPLE_DIR=${_example_dir};VEHICLE_SIL_TEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata;VEHICLE_SIL_LAUNCH_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/launcher_scratch;VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=${VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT}")
    add_test(NAME control_server.launcher_scenarios
             COMMAND "${_control_python}" -m unittest -v
               test_launcher.LauncherTest.test_a_scenario_gives_the_same_result_with_readers_and_without
               test_launcher.LauncherTest.test_a_scenario_gives_the_same_result_when_a_client_moves_the_time
               test_launcher.LauncherTest.test_during_a_scenario_a_client_moves_the_time_and_does_not_write
               test_launcher.LauncherTest.test_the_scenario_writes_as_the_client_scenario_which_a_watch_can_leave_out
               test_launcher.LauncherTest.test_a_run_that_is_connected_outside_refuses_what_moves_its_time_away_from_real_time
               test_launcher.LauncherTest.test_with_the_time_policy_free_a_run_that_is_connected_outside_takes_the_time_commands
               test_launcher.LauncherTest.test_version_lists_scenario_control_and_scenario_time
               test_launcher.LauncherTest.test_usage
               test_launcher.LauncherTest.test_a_port_in_use_is_a_setup_error
               test_launcher.LauncherTest.test_a_port_another_launcher_listens_on_is_a_setup_error
               test_launcher.LauncherTest.test_version_lists_the_control_server
               test_launcher.LauncherTest.test_control_address_says_why_there_is_none
               test_launcher.LauncherTest.test_launch_skips_what_is_no_event
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(control_server.launcher_scenarios PROPERTIES
      LABELS "control;control_server;python;launch;integration" TIMEOUT ${_control_launcher_timeout} ${_vehicle_sil_tsan_processors} COST 12
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs};VEHICLE_SIL_LAUNCH_EXE=$<TARGET_FILE:vehicle_sil_launch>;VEHICLE_SIL_EXAMPLE_DIR=${_example_dir};VEHICLE_SIL_TEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata;VEHICLE_SIL_LAUNCH_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/launcher_scenarios_scratch;VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=${VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT}")
    add_test(NAME control_server.serve_unit
             COMMAND "${_control_python}" -m unittest -v test_serve_unit
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(control_server.serve_unit PROPERTIES
      LABELS "unit;control;control_server;python" TIMEOUT 60
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python")
    add_test(NAME control_server.serve_client
             COMMAND "${_control_python}" -m unittest -v test_serve_client
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(control_server.serve_client PROPERTIES
      LABELS "control;control_server;python;launch;integration" TIMEOUT ${_control_launcher_timeout} ${_vehicle_sil_tsan_processors} COST 12
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs};VEHICLE_SIL_LAUNCH_EXE=$<TARGET_FILE:vehicle_sil_launch>;VEHICLE_SIL_EXAMPLE_DIR=${_example_dir};VEHICLE_SIL_LAUNCH_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/serve_client_scratch;VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=${VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT}")
    add_test(NAME control_server.example_write_before_start
             COMMAND "${_control_python}" "${PROJECT_SOURCE_DIR}/examples/launch_control/write_before_start.py"
                     --launch $<TARGET_FILE:vehicle_sil_launch> $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/../simulation.yaml)
    set_tests_properties(control_server.example_write_before_start PROPERTIES
      LABELS "control;control_server;python;examples" TIMEOUT ${_control_example_timeout}
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs}")
    add_test(NAME control_server.example_pause_and_step
             COMMAND "${_control_python}" "${PROJECT_SOURCE_DIR}/examples/launch_control/pause_and_step.py"
                     --launch $<TARGET_FILE:vehicle_sil_launch> $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/../simulation.yaml)
    set_tests_properties(control_server.example_pause_and_step PROPERTIES
      LABELS "control;control_server;python;examples" TIMEOUT ${_control_example_timeout} RUN_SERIAL TRUE
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs}")
    # The guide docs/howto/operate-signals.md against what it shows: the examples of examples/launch_control
    # (write_and_read.py whole, the outputs, the excerpts), the CLI and the commands of standard input.
    add_test(NAME control_server.guide_operate_signals
             COMMAND "${_control_python}" -m unittest -v test_guide_operate_signals
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(control_server.guide_operate_signals PROPERTIES
      LABELS "control;control_server;python;examples" TIMEOUT ${_control_launcher_timeout} RUN_SERIAL TRUE
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs};VEHICLE_SIL_LAUNCH_EXE=$<TARGET_FILE:vehicle_sil_launch>;VEHICLE_SIL_EXAMPLE_DIR=${_example_dir};VEHICLE_SIL_REPOSITORY=${PROJECT_SOURCE_DIR};VEHICLE_SIL_TESTED_DOCS=${VEHICLE_SIL_TESTED_DOCS}")
    # The script of the guide docs/howto/first-ecu.md (examples/first_ecu/read_cooler.py) with the three ECUs
    # of that example, which tests/template builds wherever there is a launcher (the targets are named in
    # generator expressions only, so that the order of the areas does not matter). Its output is the guide's.
    add_test(NAME control_server.example_first_ecu
             COMMAND ${CMAKE_COMMAND} -DLAUNCH=$<TARGET_FILE:vehicle_sil_launch> -DBIN=$<TARGET_FILE_DIR:cooler_ecu>
                     -DEXAMPLE=${PROJECT_SOURCE_DIR}/examples/first_ecu -DTEMPLATE=${PROJECT_SOURCE_DIR}/template
                     -DDOC=${VEHICLE_SIL_TESTED_DOCS}/howto/first-ecu.md -DWORK=${CMAKE_CURRENT_BINARY_DIR}/first_ecu
                     "-DPYTHON=${_control_python}" -DPYTHON_PATH=${PROJECT_SOURCE_DIR}/python -DSTUBS=${_stubs}
                     -P ${PROJECT_SOURCE_DIR}/tests/template/first_ecu.cmake)
    set_tests_properties(control_server.example_first_ecu PROPERTIES
      LABELS "control;control_server;python;examples" TIMEOUT ${_control_example_timeout} ${_vehicle_sil_tsan_processors} COST 5)
  else()
    vehicle_sil_python_install_hint(_hint "grpcio grpcio-tools protobuf" "the OS packages"
                                    "mingw-w64-ucrt-x86_64-python-grpcio mingw-w64-ucrt-x86_64-python-grpcio-tools"
                                    -DVEHICLE_SIL_CONTROL_PYTHON)
    if(NOT "${VEHICLE_SIL_CONTROL_PYTHON}" STREQUAL "")
      string(CONCAT _what "${VEHICLE_SIL_CONTROL_PYTHON} (VEHICLE_SIL_CONTROL_PYTHON) is not Python 3.11+ with grpcio "
                          "and grpcio-tools: ${_control_python_REASON}")
    else()
      set(_what "no Python 3.11+ with grpcio and grpcio-tools was found (tried: ${_control_python_TRIED})")
    endif()
    message(STATUS "vehicle-sil: ${_what}; the Python client tests are skipped (${_hint})")
  endif()
endif()
