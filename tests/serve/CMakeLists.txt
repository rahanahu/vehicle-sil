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
  "VEHICLE_SIL_SERVE_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/scratch"
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

# #1 (50 Run 1-ECU + 20 Run 10-ECU vt=0), #2 (Cold equivalence, protocols, faults, binary swap),
# #10 (fixed registry refusal, manual refusal, LIN repetitions).
add_test(NAME serve.basic
         COMMAND "${_serve_python}" -m unittest -v test_serve_basic
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(serve.basic PROPERTIES
  LABELS "serve;launch;integration;python" TIMEOUT 600 ${_vehicle_sil_tsan_processors} COST 25
  ENVIRONMENT "${_serve_common_env}")

# #3 (stale stdin), #4 (event window isolation & log-dir), #6 (idle signals & run/SIGINT race),
# #7 (stop vs launcher end & close_cut), #8 (H2 not_closed_ms Cold fallback), #9 (trace refusal & resources).
add_test(NAME serve.isolation
         COMMAND "${_serve_python}" -m unittest -v test_serve_isolation
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(serve.isolation PROPERTIES
  LABELS "serve;launch;integration;python" TIMEOUT 600 RUN_SERIAL TRUE COST 30
  ENVIRONMENT "${_serve_common_env}")

# #11: Windows-specific handle/thread/Job Object/CTRL_BREAK/fixed-port gRPC acceptance test.
if(WIN32)
  set(_serve_win_labels "serve;launch;integration;python;windows")
  if(VEHICLE_SIL_BUILD_CONTROL_SERVER)
    list(APPEND _serve_win_labels "control")
  endif()
  add_test(NAME serve.windows
           COMMAND "${_serve_python}" -m unittest -v test_serve_windows
           WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(serve.windows PROPERTIES
    LABELS "${_serve_win_labels}" TIMEOUT 600 RUN_SERIAL TRUE COST 15
    ENVIRONMENT "${_serve_common_env}")
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
    add_test(NAME serve.grpc
             COMMAND "${_serve_grpc_python}" -m unittest -v test_serve_grpc
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(serve.grpc PROPERTIES
      LABELS "serve;control;control_server;launch;integration;python" TIMEOUT 300
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_serve_stubs};${_serve_common_env}")
  endif()
endif()
