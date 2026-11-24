# Acceptance tests for `vehicle_sil_launch serve` (resident launcher, sequential Runs).
if(NOT TARGET vehicle_sil_launch OR NOT VEHICLE_SIL_BUILD_EXAMPLES)
  return()
endif()

include(VehicleSilPython)
vehicle_sil_find_python(_serve_python)
if(NOT _serve_python)
  message(STATUS "vehicle-sil: no Python 3.11+ found (tried: ${_serve_python_TRIED}); tests/serve skipped")
  return()
endif()

# LIN test ECUs (Master and Slave on DOOR) for serve LIN repetitions.
add_executable(vehicle_sil_serve_lin_master lin_ecu.cpp)
target_compile_definitions(vehicle_sil_serve_lin_master PRIVATE SERVE_LIN_MASTER=1)
target_link_libraries(vehicle_sil_serve_lin_master PRIVATE vehicle_sil::simulation)
vehicle_sil_target_defaults(vehicle_sil_serve_lin_master)
vehicle_sil_copy_runtime_dlls(vehicle_sil_serve_lin_master)

add_executable(vehicle_sil_serve_lin_slave lin_ecu.cpp)
target_link_libraries(vehicle_sil_serve_lin_slave PRIVATE vehicle_sil::simulation)
vehicle_sil_target_defaults(vehicle_sil_serve_lin_slave)
vehicle_sil_copy_runtime_dlls(vehicle_sil_serve_lin_slave)

set(_serve_fault_delay "OFF")
if(VEHICLE_SIL_FAULT_INJECTION)
  set(_serve_fault_delay "ON")
endif()
set(_serve_control "OFF")
if(VEHICLE_SIL_BUILD_CONTROL_SERVER)
  set(_serve_control "ON")
endif()
set(_serve_asan "OFF")
if("address" IN_LIST VEHICLE_SIL_SANITIZERS)
  set(_serve_asan "ON")
endif()

set(_serve_common_env
  "VEHICLE_SIL_LAUNCH_EXE=$<TARGET_FILE:vehicle_sil_launch>"
  "VEHICLE_SIL_TEST_ECU=$<TARGET_FILE:vehicle_sil_launch_test_ecu>"
  "VEHICLE_SIL_EXAMPLE_DIR=${CMAKE_BINARY_DIR}/examples"
  "VEHICLE_SIL_SERVE_LIN_MASTER=$<TARGET_FILE:vehicle_sil_serve_lin_master>"
  "VEHICLE_SIL_SERVE_LIN_SLAVE=$<TARGET_FILE:vehicle_sil_serve_lin_slave>"
  "VEHICLE_SIL_SERVE_FAULT_DELAY=${_serve_fault_delay}"
  "VEHICLE_SIL_SERVE_CONTROL_SERVER=${_serve_control}"
  "VEHICLE_SIL_SERVE_ASAN=${_serve_asan}"
  "VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=${VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT}"
)

# Pure unit tests for tests/serve helpers (digest and stream validator).
add_test(NAME serve.tools
         COMMAND "${_serve_python}" -m unittest -v test_serve_tools
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(serve.tools PROPERTIES
  LABELS "unit;serve;python" TIMEOUT 60)

_vehicle_sil_scaled_ctest_timeout(_serve_long_timeout 600)

# #1 (50 Run 1-ECU + 20 Run 10-ECU vt=0), #2 (Cold equivalence, protocols, faults, binary swap),
# #10 (fixed registry refusal, manual refusal, LIN repetitions), #11 (prepare/status/stop/force).
# Split across 3 parallel CTest entries with isolated scratch directories so the 10-ECU repetition
# run and the equivalence/extension runs overlap on multi-core runners.
add_test(NAME serve.basic
         COMMAND "${_serve_python}" -m unittest -v
           test_serve_basic.ServeBasicAcceptanceTest.test_01_many_runs_same_launcher_vt0_1ecu_and_10ecu
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(serve.basic PROPERTIES
  LABELS "serve;launch;integration;python" TIMEOUT ${_serve_long_timeout} ${_vehicle_sil_tsan_processors} COST 25
  ENVIRONMENT "VEHICLE_SIL_SERVE_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/scratch_basic_vt0;${_serve_common_env}")

add_test(NAME serve.basic_equivalence
         COMMAND "${_serve_python}" -m unittest -v
           test_serve_basic.ServeBasicAcceptanceTest.test_02_cold_equivalence_aba_protocols_faults_and_binary_swap
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(serve.basic_equivalence PROPERTIES
  LABELS "serve;launch;integration;python" TIMEOUT ${_serve_long_timeout} COST 15
  ENVIRONMENT "VEHICLE_SIL_SERVE_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/scratch_basic_equivalence;${_serve_common_env}")

add_test(NAME serve.basic_extensions
         COMMAND "${_serve_python}" -m unittest -v
           test_serve_basic.ServeBasicAcceptanceTest.test_10_fixed_registry_refused_and_lin_repetitions
           test_serve_basic.ServeBasicAcceptanceTest.test_11_prepare_status_stop_force_and_inline_intent_equivalence
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(serve.basic_extensions PROPERTIES
  LABELS "serve;launch;integration;python" TIMEOUT ${_serve_long_timeout} COST 15
  ENVIRONMENT "VEHICLE_SIL_SERVE_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/scratch_basic_extensions;${_serve_common_env}")

# #3 (stale stdin), #4 (event window isolation & log-dir), #6 (idle signals & run/SIGINT race),
# #7 (stop vs launcher end & close_cut), #8 (H2 not_closed_ms Cold fallback), #9 (trace refusal & resources).
# All checks use ephemeral ports, isolated scratch directories, and own-PID resource inspection
# (no wall-clock speed bounds), so they run in parallel across 2 split CTest entries.
add_test(NAME serve.isolation
         COMMAND "${_serve_python}" -m unittest -v
           test_serve_isolation.ServeIsolationAcceptanceTest.test_03_stale_stdin_stop_and_time_refused_50_runs
           test_serve_isolation.ServeIsolationAcceptanceTest.test_04_events_isolation_and_per_run_log_dir
           test_serve_isolation.ServeIsolationAcceptanceTest.test_07_stop_vs_launcher_end_and_close_cut
           test_serve_isolation.ServeIsolationAcceptanceTest.test_08_not_closed_ms_contaminates_and_fresh_launcher_matches_cold
           test_serve_isolation.ServeIsolationAcceptanceTest.test_09_trace_refused_and_bounded_resources
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(serve.isolation PROPERTIES
  LABELS "serve;launch;integration;python" TIMEOUT ${_serve_long_timeout} COST 25
  ENVIRONMENT "VEHICLE_SIL_SERVE_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/scratch_isolation_runs;${_serve_common_env}")

add_test(NAME serve.isolation_signals
         COMMAND "${_serve_python}" -m unittest -v
           test_serve_isolation.ServeIsolationAcceptanceTest.test_06_idle_signals_eof_stdout_closed_and_run_sigint_race
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(serve.isolation_signals PROPERTIES
  LABELS "serve;launch;integration;python" TIMEOUT ${_serve_long_timeout} COST 30
  ENVIRONMENT "VEHICLE_SIL_SERVE_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/scratch_isolation_signals;${_serve_common_env}")

# #11: Windows-specific handle/thread/Job Object/CTRL_BREAK/fixed-port gRPC acceptance test.
# Uses ephemeral ports, own-PID handles/threads/children, and CREATE_NEW_PROCESS_GROUP.
if(WIN32)
  set(_serve_win_labels "serve;launch;integration;python;windows")
  if(VEHICLE_SIL_BUILD_CONTROL_SERVER)
    list(APPEND _serve_win_labels "control")
  endif()
  add_test(NAME serve.windows
           COMMAND "${_serve_python}" -m unittest -v test_serve_windows
           WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(serve.windows PROPERTIES
    LABELS "${_serve_win_labels}" TIMEOUT ${_serve_long_timeout} COST 15
    ENVIRONMENT "VEHICLE_SIL_SERVE_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/scratch_windows;${_serve_common_env}")
endif()

# #5, #7b: Fixed-address gRPC Run binding and per-run credentials reload.
if(VEHICLE_SIL_BUILD_CONTROL_SERVER)
  set(_serve_stubs "${CMAKE_CURRENT_BINARY_DIR}/control_python_stubs")
  vehicle_sil_add_python_grpc_stubs(vehicle_sil_serve_control_stubs
    ALL
    OUT "${_serve_stubs}"
    PYTHON_VAR _serve_grpc_python
    PYTHONPATH "${PROJECT_SOURCE_DIR}/python"
    DEPENDS vehicle_sil_launch vehicle_sil_launch_test_ecu)
  if(_serve_grpc_python)
    _vehicle_sil_scaled_ctest_timeout(_serve_grpc_timeout 300)
    add_test(NAME serve.grpc
             COMMAND "${_serve_grpc_python}" -m unittest -v test_serve_grpc
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(serve.grpc PROPERTIES
      LABELS "serve;control;control_server;launch;integration;python" TIMEOUT ${_serve_grpc_timeout}
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_serve_stubs};VEHICLE_SIL_SERVE_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/scratch_grpc;${_serve_common_env}")
  endif()
endif()
