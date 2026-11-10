# Control server (gRPC). Added only with VEHICLE_SIL_BUILD_CONTROL_SERVER.
vehicle_sil_add_harness_tests(vehicle_sil_control_server_tests
  PREFIX control_server LABELS unit control control_server TIMEOUT 120
  SOURCES test_grpc_ping.cpp test_control_server.cpp test_security.cpp
          test_convert.cpp test_limits.cpp
  LINK vehicle_sil::control_proto vehicle_sil::control_grpc vehicle_sil::peer_control)
target_include_directories(vehicle_sil_control_server_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/control_grpc/src)
target_compile_definitions(vehicle_sil_control_server_tests PRIVATE
  VEHICLE_SIL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

# The Python client and CLI against the demo (examples/control), with a Python that has grpcio
# and grpcio-tools.
# VEHICLE_SIL_CONTROL_PYTHON may name it; otherwise the Python 3 found here.
if(VEHICLE_SIL_CONTROL_PYTHON)
  set(_control_python "${VEHICLE_SIL_CONTROL_PYTHON}")
else()
  find_package(Python3 3.8 COMPONENTS Interpreter QUIET)
  set(_control_python "${Python3_EXECUTABLE}")
endif()
if(_control_python AND TARGET vehicle_sil_control_demo AND TARGET vehicle_sil_control_server)
  execute_process(COMMAND "${_control_python}" -c "import grpc, grpc_tools"
                  RESULT_VARIABLE _vehicle_sil_control_python_ok OUTPUT_QUIET ERROR_QUIET)
  if(_vehicle_sil_control_python_ok EQUAL 0)
    set(_stubs "${CMAKE_CURRENT_BINARY_DIR}/control_python_stubs")
    add_custom_command(
      OUTPUT "${_stubs}/vehicle_sil/control/v1/control_pb2.py"
      COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${PROJECT_SOURCE_DIR}/python"
              "${_control_python}" -m vehicle_sil_control.generate --proto-root "${PROJECT_SOURCE_DIR}/proto" --out "${_stubs}"
      DEPENDS "${PROJECT_SOURCE_DIR}/proto/vehicle_sil/control/v1/control.proto" "${PROJECT_SOURCE_DIR}/python/vehicle_sil_control/generate.py"
      COMMENT "Generating the Python stubs of the Control API")
    add_custom_target(vehicle_sil_control_python_stubs ALL DEPENDS "${_stubs}/vehicle_sil/control/v1/control_pb2.py")
    add_test(NAME control_server.python_client
             COMMAND "${_control_python}" -m unittest -v test_python_client
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(control_server.python_client PROPERTIES
      LABELS "control;control_server;python" TIMEOUT 180 RUN_SERIAL TRUE
      ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs};VEHICLE_SIL_CONTROL_DEMO=$<TARGET_FILE:vehicle_sil_control_demo>;VEHICLE_SIL_CONTROL_SERVER_EXE=$<TARGET_FILE:vehicle_sil_control_server>")
    # Registry, server and two peers as separate processes, driven from Python.
    if(TARGET vehicle_sil_control_peer AND TARGET sil-kit-registry)
      add_test(NAME control_server.multi_process
               COMMAND "${_control_python}" "${CMAKE_CURRENT_SOURCE_DIR}/multi_process.py"
                       --registry $<TARGET_FILE:sil-kit-registry> --server $<TARGET_FILE:vehicle_sil_control_server>
                       --peer $<TARGET_FILE:vehicle_sil_control_peer> --logs "${CMAKE_CURRENT_BINARY_DIR}/control_multi_process"
                       --tls-cert "${CMAKE_CURRENT_SOURCE_DIR}/testdata/server.crt"
                       --tls-key "${CMAKE_CURRENT_SOURCE_DIR}/testdata/server.key")
      set_tests_properties(control_server.multi_process PROPERTIES
        LABELS "control;control_server;python;integration" TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs}")
      add_dependencies(vehicle_sil_control_python_stubs sil-kit-registry vehicle_sil_control_server vehicle_sil_control_peer)
    endif()
  else()
    message(STATUS "vehicle-sil: ${_control_python} has no grpcio/grpcio-tools; the Python client tests are skipped")
  endif()
endif()
