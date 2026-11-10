include(VehicleSilTestHelpers)

# main() and the registry of the in-process test harness (unit/test_harness.hpp).
add_library(vehicle_sil_test_main OBJECT unit/test_main.cpp)
target_include_directories(vehicle_sil_test_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/unit)
vehicle_sil_target_defaults(vehicle_sil_test_main)

# Child processes, a registry process, log helpers and coordinated virtual-time runs for the
# process-level launchers.
add_library(vehicle_sil_test_process STATIC
  integration/child_process.cpp
  integration/launcher_support.cpp
  integration/simulation_run.cpp)
target_include_directories(vehicle_sil_test_process PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/integration)
target_link_libraries(vehicle_sil_test_process PUBLIC vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_test_process)

# ---------------------------------------------------------------------------
# Unit tests: command line.
# ---------------------------------------------------------------------------
vehicle_sil_add_harness_tests(vehicle_sil_unit_tests
  PREFIX unit LABELS unit TIMEOUT 60
  SOURCES unit/test_cli_args.cpp
  LINK vehicle_sil_cli)

# ---------------------------------------------------------------------------
# Core: simulation time helpers and SIL Kit virtual time (in-process registry). The virtual
# time cases measure wall-clock speed and run alone.
# ---------------------------------------------------------------------------
vehicle_sil_add_harness_tests(vehicle_sil_core_tests
  PREFIX core LABELS unit core TIMEOUT 120 RUN_SERIAL "^virtual_time_"
  SOURCES core/test_time_helpers.cpp core/test_virtual_time.cpp core/test_time_gate.cpp
  LINK vehicle_sil::core)

# ---------------------------------------------------------------------------
# Control agent: typed variables and timed writes applied in the steps, without a transport;
# driven by vehicle_sil::Peer (in-process registry).
# ---------------------------------------------------------------------------
vehicle_sil_add_harness_tests(vehicle_sil_control_tests
  PREFIX control LABELS unit control TIMEOUT 120
  SOURCES control/test_agent.cpp control/test_peer_agent.cpp control/test_hub.cpp
  LINK vehicle_sil::peer_control)

# ---------------------------------------------------------------------------
# Control server (gRPC). Only with VEHICLE_SIL_BUILD_CONTROL_SERVER.
# ---------------------------------------------------------------------------
if(VEHICLE_SIL_BUILD_CONTROL_SERVER)
  vehicle_sil_add_harness_tests(vehicle_sil_control_server_tests
    PREFIX control_server LABELS unit control control_server TIMEOUT 120
    SOURCES control_server/test_grpc_ping.cpp control_server/test_control_server.cpp control_server/test_security.cpp
            control_server/test_convert.cpp control_server/test_limits.cpp
    LINK vehicle_sil::control_proto vehicle_sil::control_grpc vehicle_sil::peer_control)
  target_include_directories(vehicle_sil_control_server_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/control_grpc/src)
  target_compile_definitions(vehicle_sil_control_server_tests PRIVATE
    VEHICLE_SIL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/control_server/testdata")

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
               WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/control_server")
      set_tests_properties(control_server.python_client PROPERTIES
        LABELS "control;control_server;python" TIMEOUT 180 RUN_SERIAL TRUE
        ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs};VEHICLE_SIL_CONTROL_DEMO=$<TARGET_FILE:vehicle_sil_control_demo>;VEHICLE_SIL_CONTROL_SERVER_EXE=$<TARGET_FILE:vehicle_sil_control_server>")
      # Registry, server and two peers as separate processes, driven from Python.
      if(TARGET vehicle_sil_control_peer AND TARGET sil-kit-registry)
        add_test(NAME control_server.multi_process
                 COMMAND "${_control_python}" "${CMAKE_CURRENT_SOURCE_DIR}/control_server/multi_process.py"
                         --registry $<TARGET_FILE:sil-kit-registry> --server $<TARGET_FILE:vehicle_sil_control_server>
                         --peer $<TARGET_FILE:vehicle_sil_control_peer> --logs "${CMAKE_CURRENT_BINARY_DIR}/control_multi_process"
                         --tls-cert "${CMAKE_CURRENT_SOURCE_DIR}/control_server/testdata/server.crt"
                         --tls-key "${CMAKE_CURRENT_SOURCE_DIR}/control_server/testdata/server.key")
        set_tests_properties(control_server.multi_process PROPERTIES
          LABELS "control;control_server;python;integration" TIMEOUT 300 RUN_SERIAL TRUE
          ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;VEHICLE_SIL_CONTROL_STUBS=${_stubs}")
        add_dependencies(vehicle_sil_control_python_stubs sil-kit-registry vehicle_sil_control_server vehicle_sil_control_peer)
      endif()
    else()
      message(STATUS "vehicle-sil: ${_control_python} has no grpcio/grpcio-tools; the Python client tests are skipped")
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# LIN: schedule, frames, checksums, diagnostic frames, sleep and wakeup with SIL Kit LIN in
# virtual time (in-process registry).
# ---------------------------------------------------------------------------
vehicle_sil_add_harness_tests(vehicle_sil_lin_tests
  PREFIX lin LABELS unit lin TIMEOUT 120 RUN_SERIAL "wall_clock|every_speed"
  SOURCES lin/test_lin_virtual_time.cpp lin/test_lin_cluster.cpp lin/test_lin_protocol.cpp
  LINK vehicle_sil::lin)

# ---------------------------------------------------------------------------
# vehicle_sil::can (base of the J1939 stack): frame values, virtual bus, host and step
# driver. Links only vehicle_sil::can, so building it checks that the library needs no SIL Kit.
# ---------------------------------------------------------------------------
vehicle_sil_add_harness_tests(vehicle_sil_can_tests
  PREFIX can LABELS unit can TIMEOUT 60
  SOURCES can/test_can_values_and_bus.cpp can/test_can_host.cpp can/test_can_bus_model.cpp
          can/test_can_real_time.cpp can/test_can_faults.cpp can/test_can_tap_port.cpp
          can/test_can_raw_channel.cpp
  LINK vehicle_sil::can)
target_include_directories(vehicle_sil_can_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/can)

# The test kit (vehicle_sil::testkit): J1939 networks on a local virtual bus, recorders, fault
# injection. Links no SIL Kit.
vehicle_sil_add_harness_tests(vehicle_sil_testkit_tests
  PREFIX testkit LABELS unit testkit j1939 TIMEOUT 60
  SOURCES testkit/test_testkit.cpp testkit/test_local_ecus.cpp
  LINK vehicle_sil::testkit)

# The ECU application API as its users write it (examples/j1939/ecu_pair and parts of one's own)
# on the local virtual bus. The example's typed messages come from its YAML (generated here too
# when the examples are not built).
if(NOT TARGET vehicle_sil_ecu_pair_messages)
  vehicle_sil_add_j1939_codec(vehicle_sil_ecu_pair_messages
                              ${PROJECT_SOURCE_DIR}/examples/j1939/ecu_pair/ecu_pair_messages.yaml)
endif()
vehicle_sil_add_harness_tests(vehicle_sil_ecu_api_tests
  PREFIX ecu_api LABELS unit testkit j1939 TIMEOUT 60
  SOURCES testkit/test_ecu_personas.cpp
  LINK vehicle_sil::testkit vehicle_sil_ecu_pair_messages vehicle_sil::j1939_diagnostics
       vehicle_sil::j1939_periodic_sender vehicle_sil::j1939_requester vehicle_sil::j1939_monitor
       vehicle_sil::j1939_address_commander)
target_include_directories(vehicle_sil_ecu_api_tests PRIVATE ${PROJECT_SOURCE_DIR}/examples/j1939/ecu_pair)

# A protocol of one's own as its developer tests it: the parameter service of
# examples/j1939/param_service (built here too when the examples are not built) on the local
# virtual bus, and the example's demo. The example uses the stack's public API only.
if(NOT TARGET vehicle_sil_param_service)
  set(_param_service ${PROJECT_SOURCE_DIR}/examples/j1939/param_service)
  vehicle_sil_add_j1939_codec(vehicle_sil_param_service_messages ${_param_service}/param_service_messages.yaml)
  add_library(vehicle_sil_param_service STATIC ${_param_service}/param_service.cpp)
  target_include_directories(vehicle_sil_param_service PUBLIC ${_param_service})
  target_link_libraries(vehicle_sil_param_service PUBLIC vehicle_sil::j1939_exchange vehicle_sil_param_service_messages)
  vehicle_sil_target_defaults(vehicle_sil_param_service)
endif()
vehicle_sil_add_harness_tests(vehicle_sil_param_service_tests
  PREFIX param_service LABELS unit testkit j1939 TIMEOUT 60
  SOURCES testkit/test_param_service.cpp
  LINK vehicle_sil::testkit vehicle_sil_param_service)
add_test(NAME param_service.boundary
         COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${PROJECT_SOURCE_DIR}/examples/j1939/param_service -DSOURCES_ONLY=ON
                 -DFORBID_OBSERVER=ON -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
set_tests_properties(param_service.boundary PROPERTIES LABELS "unit;testkit;j1939" TIMEOUT 60)
if(TARGET vehicle_sil_j1939_param_service)
  add_test(NAME param_service.demo COMMAND vehicle_sil_j1939_param_service)
  set_tests_properties(param_service.demo PROPERTIES LABELS "unit;testkit;j1939" TIMEOUT 60)
endif()

# The first example, on the virtual buses and on SIL Kit (registry of the process).
if(TARGET vehicle_sil_getting_started)
  add_test(NAME getting_started.demo COMMAND vehicle_sil_getting_started)
  add_test(NAME getting_started.silkit_demo COMMAND vehicle_sil_getting_started --silkit)
  set_tests_properties(getting_started.demo getting_started.silkit_demo
                       PROPERTIES LABELS "examples;simulation" TIMEOUT 120)
endif()

# Examples that run on the virtual buses and check their own result.
if(TARGET vehicle_sil_typed_messages)
  add_test(NAME typed_messages.demo COMMAND vehicle_sil_typed_messages)
  set_tests_properties(typed_messages.demo PROPERTIES LABELS "examples;simulation" TIMEOUT 120)
endif()
if(TARGET vehicle_sil_pid_stream)
  add_test(NAME pid_stream.demo COMMAND vehicle_sil_pid_stream)
  set_tests_properties(pid_stream.demo PROPERTIES LABELS "examples;simulation" TIMEOUT 120)
endif()
if(TARGET vehicle_sil_fault_injection)
  add_test(NAME fault_injection.demo COMMAND vehicle_sil_fault_injection)
  set_tests_properties(fault_injection.demo PROPERTIES LABELS "examples;simulation" TIMEOUT 120)
endif()
if(TARGET vehicle_sil_vehicle)
  add_test(NAME vehicle.silkit_demo COMMAND vehicle_sil_vehicle)
  add_test(NAME vehicle.local_demo COMMAND vehicle_sil_vehicle --local)
  set_tests_properties(vehicle.silkit_demo vehicle.local_demo PROPERTIES LABELS "examples;simulation" TIMEOUT 120)
endif()
if(TARGET vehicle_sil_low_level)
  add_test(NAME low_level.demo COMMAND vehicle_sil_low_level)
  set_tests_properties(low_level.demo PROPERTIES LABELS "examples" TIMEOUT 120)
endif()

# The SIL Kit port of vehicle_sil::can: against a fake controller, and on real participants
# (in-process registry), where the traces must equal the virtual bus's.
vehicle_sil_add_harness_tests(vehicle_sil_can_silkit_tests
  PREFIX can_silkit LABELS unit can TIMEOUT 120
  SOURCES can/test_silkit_can_port.cpp can/test_can_silkit_runs.cpp
  LINK vehicle_sil::can_silkit)
target_include_directories(vehicle_sil_can_silkit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/can)

# vehicle_sil::Peer on real participants (in-process registry): J1939 and LIN nodes, the same
# results as the local virtual bus, real time with an external port.
vehicle_sil_add_harness_tests(vehicle_sil_peer_tests
  PREFIX peer LABELS unit peer j1939 TIMEOUT 180
  SOURCES peer/test_peer.cpp
  LINK vehicle_sil::peer vehicle_sil::testkit vehicle_sil_ecu_pair_messages vehicle_sil::j1939_diagnostics
       vehicle_sil::j1939_periodic_sender vehicle_sil::j1939_requester vehicle_sil::j1939_monitor)
target_include_directories(vehicle_sil_peer_tests PRIVATE ${PROJECT_SOURCE_DIR}/examples/j1939/ecu_pair)

# vehicle_sil::Simulation: the same ECUs (written against vehicle_sil::Ecu) on the local virtual
# buses and on SIL Kit (in-process registry), failures, the rules of a run, a controller
# elsewhere and other participants.
vehicle_sil_add_harness_tests(vehicle_sil_simulation_tests
  PREFIX simulation LABELS unit simulation TIMEOUT 180
  SOURCES simulation/test_simulation.cpp
  LINK vehicle_sil::simulation)

# The simple CAN network simulator (vehicle_sil::can_netsim) on real participants (in-process
# registry), compared with the virtual bus; its configuration file; and the executable.
if(TARGET vehicle_sil_can_bus_simulator)
  vehicle_sil_add_harness_tests(vehicle_sil_can_netsim_tests
    PREFIX can_netsim LABELS unit can TIMEOUT 120
    SOURCES can/test_can_netsim.cpp
    LINK vehicle_sil::can_netsim vehicle_sil_test_process)
  target_include_directories(vehicle_sil_can_netsim_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/can)
  target_compile_definitions(vehicle_sil_can_netsim_tests
    PRIVATE VEHICLE_SIL_CAN_BUS_SIMULATOR="$<TARGET_FILE:vehicle_sil_can_bus_simulator>")
  add_dependencies(vehicle_sil_can_netsim_tests vehicle_sil_can_bus_simulator)
endif()

# ---------------------------------------------------------------------------
# Vector XL CAN bridge (against a fake XL driver; runs on every platform).
# ---------------------------------------------------------------------------
add_subdirectory(vector_xl)

# ---------------------------------------------------------------------------
# FetchContent consumer test (opt-in): configures, builds and runs
# examples/fetchcontent_consumer against a git URL + commit.
# ---------------------------------------------------------------------------
if(VEHICLE_SIL_TEST_FETCHCONTENT)
  set(VEHICLE_SIL_FETCHCONTENT_TEST_REPOSITORY "${PROJECT_SOURCE_DIR}"
      CACHE STRING "Git repository the FetchContent test clones (local path or URL)")
  set(VEHICLE_SIL_FETCHCONTENT_TEST_TAG "HEAD"
      CACHE STRING "Commit the FetchContent test checks out; HEAD is resolved at test time")
  add_test(
    NAME fetchcontent.consumer
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/examples/fetchcontent_consumer
            -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/fetchcontent_consumer
            -DREPOSITORY=${VEHICLE_SIL_FETCHCONTENT_TEST_REPOSITORY}
            -DTAG=${VEHICLE_SIL_FETCHCONTENT_TEST_TAG}
            -DGENERATOR=${CMAKE_GENERATOR}
            -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DC_COMPILER=${CMAKE_C_COMPILER}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/fetchcontent/run_consumer.cmake)
  set_tests_properties(fetchcontent.consumer PROPERTIES LABELS fetchcontent TIMEOUT 7200)
endif()

# The remaining tests run the example peers as processes.
if(NOT VEHICLE_SIL_BUILD_EXAMPLES)
  message(STATUS "vehicle-sil: VEHICLE_SIL_BUILD_EXAMPLES is OFF; process-level J1939 and virtual time tests are skipped")
  return()
endif()

# ---------------------------------------------------------------------------
# J1939 integration: real sil-kit-registry, sender and receiver processes.
# ---------------------------------------------------------------------------
add_executable(vehicle_sil_it_launcher integration/launcher.cpp)
target_link_libraries(vehicle_sil_it_launcher PRIVATE vehicle_sil_test_process)
vehicle_sil_target_defaults(vehicle_sil_it_launcher)
# The launcher starts these executables, so they must be built with it.
add_dependencies(vehicle_sil_it_launcher sil-kit-registry vehicle_sil_j1939_sender vehicle_sil_j1939_receiver)

vehicle_sil_add_scenario_tests(vehicle_sil_it_launcher
  PREFIX integration LABELS integration TIMEOUT 120
  WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/integration
  SCENARIOS address-claim-and-pgn receiver-timeout receiver-detects-wrong-name receiver-detects-wrong-payload
  ARGS --registry $<TARGET_FILE:sil-kit-registry>
       --sender $<TARGET_FILE:vehicle_sil_j1939_sender>
       --receiver $<TARGET_FILE:vehicle_sil_j1939_receiver>
       --timeout-ms 60000)

# ---------------------------------------------------------------------------
# Virtual time across processes: controller + physics (1 ms) + ECU with an existing tick()
# model and J1939 (10 ms) + LIN sensor (2 ms) + LIN body controller (5 ms). The scenarios
# measure wall-clock speed and run alone.
# ---------------------------------------------------------------------------
add_executable(vehicle_sil_vt_launcher virtual_time/vt_launcher.cpp)
target_link_libraries(vehicle_sil_vt_launcher PRIVATE vehicle_sil_test_process)
vehicle_sil_target_defaults(vehicle_sil_vt_launcher)
add_dependencies(vehicle_sil_vt_launcher sil-kit-registry vehicle_sil_sim_controller vehicle_sil_vt_physics
                 vehicle_sil_vt_ecu vehicle_sil_vt_lin_sensor vehicle_sil_vt_body_controller)

vehicle_sil_add_scenario_tests(vehicle_sil_vt_launcher
  PREFIX virtual_time LABELS integration virtual_time TIMEOUT 300 RUN_SERIAL
  WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/virtual_time
  SCENARIOS fast slow-tick paced-fast paced-slow peer-failure
  ARGS --registry $<TARGET_FILE:sil-kit-registry>
       --controller $<TARGET_FILE:vehicle_sil_sim_controller>
       --physics $<TARGET_FILE:vehicle_sil_vt_physics>
       --ecu $<TARGET_FILE:vehicle_sil_vt_ecu>
       --lin-sensor $<TARGET_FILE:vehicle_sil_vt_lin_sensor>
       --body-controller $<TARGET_FILE:vehicle_sil_vt_body_controller>
       --end-ms 1000
       --timeout-ms 120000)

# ---------------------------------------------------------------------------
# LIN cluster across processes: controller + LIN master (5 ms) + two LIN slaves (2 ms, 4 ms):
# schedule, master and slave frames, sleep and wakeup by master, slave and bus idle.
# ---------------------------------------------------------------------------
add_executable(vehicle_sil_lin_launcher lin/lin_launcher.cpp)
target_link_libraries(vehicle_sil_lin_launcher PRIVATE vehicle_sil_test_process)
vehicle_sil_target_defaults(vehicle_sil_lin_launcher)
add_dependencies(vehicle_sil_lin_launcher sil-kit-registry vehicle_sil_sim_controller vehicle_sil_lin_master
                 vehicle_sil_lin_slave)

vehicle_sil_add_scenario_tests(vehicle_sil_lin_launcher
  PREFIX lin_cluster LABELS integration lin TIMEOUT 300
  WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/lin_cluster
  SCENARIOS cluster cluster-paced cluster-master-10ms
  ARGS --registry $<TARGET_FILE:sil-kit-registry>
       --controller $<TARGET_FILE:vehicle_sil_sim_controller>
       --master $<TARGET_FILE:vehicle_sil_lin_master>
       --slave $<TARGET_FILE:vehicle_sil_lin_slave>
       --timeout-ms 120000)

# ---------------------------------------------------------------------------
# The J1939 stack (vehicle_sil::j1939) on the virtual bus. Links no SIL Kit, so
# building it checks that the stack needs none.
# ---------------------------------------------------------------------------
vehicle_sil_add_harness_tests(vehicle_sil_j1939_tests
  PREFIX j1939 LABELS unit j1939 TIMEOUT 60
  SOURCES j1939/test_j1939_types.cpp j1939/test_j1939_network.cpp j1939/test_j1939_request.cpp
          j1939/test_j1939_address_claim.cpp j1939/test_j1939_address_management.cpp
          j1939/test_j1939_transport_bam.cpp j1939/test_j1939_transport_rts.cpp
          j1939/test_j1939_transport_faults.cpp j1939/test_j1939_bus_timing.cpp
          j1939/test_j1939_outage.cpp j1939/test_j1939_real_time.cpp j1939/test_j1939_toolbox.cpp
          j1939/test_j1939_observer.cpp
  LINK vehicle_sil::j1939)
target_include_directories(vehicle_sil_j1939_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/j1939)

# The protocol parts (DM13, diagnostics, identification, periodic sender, requester, monitor,
# address commander): built only from the engine's public API
# and toolbox; the boundary test checks their sources and links.
vehicle_sil_add_harness_tests(vehicle_sil_j1939_parts_tests
  PREFIX j1939_parts LABELS unit j1939 TIMEOUT 60
  SOURCES j1939/test_j1939_dm13.cpp j1939/test_j1939_diagnostics.cpp j1939/test_j1939_identification.cpp
          j1939/test_j1939_periodic_sender.cpp j1939/test_j1939_requester.cpp j1939/test_j1939_monitor.cpp
          j1939/test_j1939_exchange.cpp
  LINK vehicle_sil::j1939_diagnostics vehicle_sil::j1939_periodic_sender vehicle_sil::j1939_requester
       vehicle_sil::j1939_monitor vehicle_sil::j1939_address_commander vehicle_sil::j1939_exchange)
target_include_directories(vehicle_sil_j1939_parts_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/j1939)
add_test(NAME j1939_parts.boundary
         COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${PROJECT_SOURCE_DIR}/libs/j1939/parts
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
set_tests_properties(j1939_parts.boundary PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
# The check itself: a hidden friend function passes, befriending a class or type fails.
foreach(fixture IN ITEMS hidden_friend friend_class friend_type)
  add_test(NAME j1939_parts.boundary_check_${fixture}
           COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/j1939/boundary_fixtures/${fixture}
                   -DSOURCES_ONLY=ON -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
  set_tests_properties(j1939_parts.boundary_check_${fixture} PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
endforeach()
# Fails for the right reason (the exit code is not used with a regular expression).
set_tests_properties(j1939_parts.boundary_check_friend_class j1939_parts.boundary_check_friend_type
                     PROPERTIES PASS_REGULAR_EXPRESSION "reach into the J1939 engine:.*part\\.hpp: matches 'friend")
add_test(NAME j1939_parts.boundary_check_observer
         COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/j1939/boundary_fixtures/observer
                 -DSOURCES_ONLY=ON -DFORBID_OBSERVER=ON -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
set_tests_properties(j1939_parts.boundary_check_observer PROPERTIES LABELS "unit;j1939" TIMEOUT 60
                     PASS_REGULAR_EXPRESSION "reach into the J1939 engine:.*part\\.hpp: matches 'set_observer'")
# The test kit, Peer, Ecu and Simulation use the engine's public API only, like the parts.
foreach(dir IN ITEMS testkit peer ecu simulation)
  add_test(NAME ${dir}.boundary
           COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${PROJECT_SOURCE_DIR}/libs/${dir} -DSOURCES_ONLY=ON
                   -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
  set_tests_properties(${dir}.boundary PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
endforeach()

# Typed messages: the generator (Python, its error checks) and the generated types
# (codec/codec_test_messages.yaml), alone and on a node.
vehicle_sil_add_j1939_codec(vehicle_sil_codec_test_messages j1939/codec/codec_test_messages.yaml
                            j1939/codec/codec_tricky_names.yaml j1939/codec/codec_profiles.yaml)
vehicle_sil_add_harness_tests(vehicle_sil_j1939_codec_tests
  PREFIX j1939_codec LABELS unit j1939 TIMEOUT 60
  SOURCES j1939/test_j1939_codec.cpp j1939/test_j1939_codec_node.cpp j1939/test_j1939_codec_profiles.cpp
  LINK vehicle_sil_codec_test_messages vehicle_sil::j1939_monitor vehicle_sil::testkit)
target_include_directories(vehicle_sil_j1939_codec_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/j1939)
add_test(NAME j1939_codec.generator
         COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${PROJECT_SOURCE_DIR}/tools/j1939_codegen/test_j1939_codegen.py)
set_tests_properties(j1939_codec.generator PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
# codec.hpp knows the engine's value types only; codec_node.hpp uses the public API only.
add_test(NAME j1939_codec.independent_of_the_engine
         COMMAND ${CMAKE_COMMAND} -DHEADER=${PROJECT_SOURCE_DIR}/libs/j1939/codec/include/vehicle_sil/j1939/codec.hpp
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Codec.cmake)
add_test(NAME j1939_codec.boundary
         COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${PROJECT_SOURCE_DIR}/libs/j1939/codec -DSOURCES_ONLY=ON
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
set_tests_properties(j1939_codec.independent_of_the_engine j1939_codec.boundary PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
# GCC's -fsanitize=undefined makes some expressions no constant expressions (a pointer compared
# with nullptr): the codec's compile-time checks must still compile with it. Syntax only, so it
# needs no libubsan.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(_codec_includes "$<TARGET_PROPERTY:vehicle_sil_codec_test_messages,INTERFACE_INCLUDE_DIRECTORIES>"
                      "$<TARGET_PROPERTY:vehicle_sil_j1939_codec,INTERFACE_INCLUDE_DIRECTORIES>"
                      "$<TARGET_PROPERTY:vehicle_sil_j1939,INTERFACE_INCLUDE_DIRECTORIES>"
                      "$<TARGET_PROPERTY:vehicle_sil_can,INTERFACE_INCLUDE_DIRECTORIES>"
                      "${CMAKE_CURRENT_SOURCE_DIR}/unit" "${CMAKE_CURRENT_SOURCE_DIR}/j1939")
  add_test(NAME j1939_codec.compiles_with_gcc_undefined_behavior_sanitizer
           COMMAND ${CMAKE_CXX_COMPILER} -std=c++20 -fsyntax-only -fsanitize=undefined -Wall -Wextra -Werror
                   "-I$<JOIN:${_codec_includes},;-I>"
                   ${CMAKE_CURRENT_SOURCE_DIR}/j1939/test_j1939_codec.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/j1939/test_j1939_codec_profiles.cpp
           COMMAND_EXPAND_LISTS)
  set_tests_properties(j1939_codec.compiles_with_gcc_undefined_behavior_sanitizer PROPERTIES LABELS "unit;j1939" TIMEOUT 120)
endif()
# The check itself: a header that includes the engine (with angle brackets) fails.
add_test(NAME j1939_codec.independent_check_rejects_the_engine
         COMMAND ${CMAKE_COMMAND} -DHEADER=${CMAKE_CURRENT_SOURCE_DIR}/j1939/boundary_fixtures/codec_includes_node/codec.hpp
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Codec.cmake)
set_tests_properties(j1939_codec.independent_check_rejects_the_engine PROPERTIES LABELS "unit;j1939" TIMEOUT 60
                     PASS_REGULAR_EXPRESSION "found: vehicle_sil/j1939/network.hpp")

# Replaces the global operator new to count allocations in steady-state steps: its own binary.
vehicle_sil_add_harness_tests(vehicle_sil_j1939_allocation_tests
  PREFIX j1939 LABELS unit j1939 TIMEOUT 60
  SOURCES j1939/test_j1939_allocation.cpp
  LINK vehicle_sil::j1939 vehicle_sil::j1939_diagnostics vehicle_sil::j1939_periodic_sender vehicle_sil::j1939_requester
       vehicle_sil::j1939_monitor vehicle_sil::j1939_address_commander vehicle_sil::j1939_exchange
       vehicle_sil_codec_test_messages)
target_include_directories(vehicle_sil_j1939_allocation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/j1939)

# The J1939 stack on SIL Kit participants (in-process registry): same traces as on the
# virtual bus, with SIL Kit's trivial simulation and with the simple CAN network simulator.
vehicle_sil_add_harness_tests(vehicle_sil_j1939_silkit_tests
  PREFIX j1939_silkit LABELS unit j1939 TIMEOUT 180
  SOURCES j1939/test_j1939_silkit.cpp
  LINK vehicle_sil::j1939 vehicle_sil::can_silkit vehicle_sil::can_netsim)
