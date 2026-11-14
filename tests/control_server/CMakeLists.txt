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
    test_control_server.cpp
    test_convert.cpp
    test_grpc_ping.cpp
    test_limits.cpp
    test_observe_only.cpp
    test_security.cpp
    test_simulation_monitor.cpp
    test_snapshot.cpp
  LINK vehicle_sil::control_proto vehicle_sil::control_grpc vehicle_sil::control_silkit vehicle_sil::peer)
target_include_directories(vehicle_sil_control_server_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/control_grpc/src)
target_compile_definitions(vehicle_sil_control_server_tests PRIVATE
  VEHICLE_SIL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

# The Python client and CLI against vehicle_sil_launch run --control, with a Python 3.11+ that has grpcio and grpcio-tools.
# VEHICLE_SIL_CONTROL_PYTHON may name it; otherwise the repository's uv environment (.venv) if it has
# them, else the Python 3 found here (cmake/VehicleSilPython.cmake).
if(VEHICLE_SIL_CONTROL_PYTHON)
  set(_control_python "${VEHICLE_SIL_CONTROL_PYTHON}")
else()
  vehicle_sil_find_python(_control_python grpc grpc_tools)
endif()
if(TARGET vehicle_sil_launch AND TARGET vehicle_sil_engine_ecu AND TARGET vehicle_sil_dashboard_ecu)
  vehicle_sil_python_can_import(_vehicle_sil_control_python_ok "${_control_python}" grpc grpc_tools)
  if(_vehicle_sil_control_python_ok)
    set(_stubs "${CMAKE_CURRENT_BINARY_DIR}/control_python_stubs")
    add_custom_command(
      OUTPUT "${_stubs}/vehicle_sil/control/v1/control_pb2.py"
      COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${PROJECT_SOURCE_DIR}/python"
              "${_control_python}" -m vehicle_sil_control.generate --proto-root "${PROJECT_SOURCE_DIR}/proto" --out "${_stubs}"
      DEPENDS "${PROJECT_SOURCE_DIR}/proto/vehicle_sil/control/v1/control.proto" "${PROJECT_SOURCE_DIR}/python/vehicle_sil_control/generate.py"
      COMMENT "Generating the Python stubs of the Control API")
    add_custom_target(vehicle_sil_control_python_stubs ALL DEPENDS "${_stubs}/vehicle_sil/control/v1/control_pb2.py")
    add_dependencies(vehicle_sil_control_python_stubs vehicle_sil_launch vehicle_sil_engine_ecu vehicle_sil_dashboard_ecu)
    set(_example_dir "$<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..")
    # The client and the CLI under the launcher with the ECUs of examples/ecu_main; what the client
    # makes of the server's messages.
    add_test(NAME control_server.python_client
             COMMAND "${_control_python}" -m unittest -v test_python_client
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(control_server.python_client PROPERTIES
      LABELS "control;control_server;python" TIMEOUT 180 RUN_SERIAL TRUE
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs};VEHICLE_SIL_LAUNCH_EXE=$<TARGET_FILE:vehicle_sil_launch>;VEHICLE_SIL_EXAMPLE_DIR=${_example_dir};VEHICLE_SIL_LAUNCH_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/python_client_scratch")
    # vehicle_sil_launch run --control with the ECUs of examples/ecu_main, driven from Python; and
    # the example of examples/launch_control (it checks itself).
    add_test(NAME control_server.launcher
             COMMAND "${_control_python}" -m unittest -v test_launcher
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(control_server.launcher PROPERTIES
      LABELS "control;control_server;python;launch;integration" TIMEOUT 300 RUN_SERIAL TRUE
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs};VEHICLE_SIL_LAUNCH_EXE=$<TARGET_FILE:vehicle_sil_launch>;VEHICLE_SIL_EXAMPLE_DIR=${_example_dir};VEHICLE_SIL_TEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata;VEHICLE_SIL_LAUNCH_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/launcher_scratch")
    add_test(NAME control_server.example_write_before_start
             COMMAND "${_control_python}" "${PROJECT_SOURCE_DIR}/examples/launch_control/write_before_start.py"
                     --launch $<TARGET_FILE:vehicle_sil_launch> $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/../simulation.yaml)
    set_tests_properties(control_server.example_write_before_start PROPERTIES
      LABELS "control;control_server;python;examples" TIMEOUT 120 RUN_SERIAL TRUE
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs}")
    add_test(NAME control_server.example_pause_and_step
             COMMAND "${_control_python}" "${PROJECT_SOURCE_DIR}/examples/launch_control/pause_and_step.py"
                     --launch $<TARGET_FILE:vehicle_sil_launch> $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/../simulation.yaml)
    set_tests_properties(control_server.example_pause_and_step PROPERTIES
      LABELS "control;control_server;python;examples" TIMEOUT 120 RUN_SERIAL TRUE
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs}")
  else()
    vehicle_sil_python_install_hint(_hint "grpcio grpcio-tools protobuf" "the OS packages"
                                    "mingw-w64-ucrt-x86_64-python-grpcio mingw-w64-ucrt-x86_64-python-grpcio-tools"
                                    -DVEHICLE_SIL_CONTROL_PYTHON)
    if(VEHICLE_SIL_CONTROL_PYTHON)
      string(CONCAT _what "${VEHICLE_SIL_CONTROL_PYTHON} (VEHICLE_SIL_CONTROL_PYTHON) is not Python 3.11+ with grpcio "
                          "and grpcio-tools: ${_vehicle_sil_control_python_ok_REASON}")
    else()
      set(_what "no Python 3.11+ with grpcio and grpcio-tools was found (tried: ${_control_python_TRIED})")
    endif()
    message(STATUS "vehicle-sil: ${_what}; the Python client tests are skipped (${_hint})")
  endif()
endif()
