cmake_minimum_required(VERSION 3.21)

project(
  vehicle_sil
  VERSION 0.1.0
  DESCRIPTION "Vehicle network SIL simulation peers on Vector SIL Kit"
  LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options. Examples and tests default to ON only when vehicle-sil is the
# top-level project, so FetchContent consumers only build the libraries they
# link against (everything else is EXCLUDE_FROM_ALL or not added).
# ---------------------------------------------------------------------------
option(VEHICLE_SIL_BUILD_EXAMPLES "Build the example peer executables" ${PROJECT_IS_TOP_LEVEL})
option(VEHICLE_SIL_BUILD_TESTS "Build the vehicle-sil tests" ${PROJECT_IS_TOP_LEVEL})
option(VEHICLE_SIL_TEST_FETCHCONTENT
       "Add a (slow) test that consumes this repository via FetchContent" OFF)
option(VEHICLE_SIL_WARNINGS_AS_ERRORS "Treat warnings in vehicle-sil targets as errors" OFF)
option(VEHICLE_SIL_STATIC
       "Executables that link SIL Kit need no vehicle-sil library next to them: SIL Kit as a static library, and the C++ runtime linked statically (MinGW: also winpthread; glibc stays shared)" OFF)
option(VEHICLE_SIL_BUILD_CONTROL_SERVER
       "Build the control server into vehicle_sil_launch (run --control; needs gRPC: found, or built with FetchContent)" OFF)
set(VEHICLE_SIL_SANITIZERS
    ""
    CACHE STRING "Semicolon separated -fsanitize= values, e.g. address;undefined (top-level builds only)")

include(VehicleSilHelpers)
# Sanitizers must be set before the dependencies are added so that SIL Kit is instrumented
# too.
# Only when vehicle-sil is the top-level project: a consumer's own targets would not get the
# flags and would fail to link against instrumented libraries.
if(PROJECT_IS_TOP_LEVEL)
  vehicle_sil_enable_sanitizers()
elseif(NOT "${VEHICLE_SIL_SANITIZERS}" STREQUAL "")
  message(WARNING "vehicle-sil: VEHICLE_SIL_SANITIZERS is ignored in a subproject build; set the flags for the whole build instead")
endif()

include(VehicleSilDependencies)
# vehicle_sil_add_j1939_codec(): typed J1939 messages generated from YAML.
include(VehicleSilJ1939Codec)
if(VEHICLE_SIL_BUILD_CONTROL_SERVER)
  include(VehicleSilGrpc)
endif()

# Libraries (every target and what it holds: the table in docs/architecture.md):
#   vehicle_sil::core       SIL Kit participant and lifecycle management (protocol independent)
#   vehicle_sil::sim_base   simulation time and the step inbox, without SIL Kit (part of libs/core)
#   vehicle_sil::step       steps the protocol processing of a participant for any bus: host,
#                           step driver, real-time driver and clocks (no SIL Kit)
#   vehicle_sil::can        backend-neutral CAN: frames, ports, host, step driver, virtual bus (no SIL Kit)
#   vehicle_sil::can_silkit vehicle_sil::can port on SIL Kit CAN (virtual or real time)
#   vehicle_sil::can_netsim the simple CAN network simulator (SIL Kit)
#   vehicle_sil::eth        raw Ethernet frames: channel, port, virtual network (no SIL Kit)
#   vehicle_sil::eth_silkit vehicle_sil::eth port on SIL Kit Ethernet (virtual time)
#   vehicle_sil::eth_link   the external side of an Ethernet bridge: link interface, frame filter, MAC table (no SIL Kit)
#   vehicle_sil::j1939      J1939 stack on vehicle_sil::can (no SIL Kit); its protocol parts are the
#                           vehicle_sil::j1939_* targets of libs/j1939/parts
#   vehicle_sil::j1939_codec typed J1939 messages (header only; the types are generated from YAML by
#                           vehicle_sil_add_j1939_codec())
#   vehicle_sil::lin        LIN master and slave on a port, and the virtual LIN bus (no SIL Kit)
#   vehicle_sil::lin_silkit vehicle_sil::lin port on SIL Kit LIN (virtual time)
#   vehicle_sil::control    the control agent of an ECU (variables, timed writes, samples), the
#                           control server's core (Hub) and their messages as bytes; no transport, no SIL Kit
#   vehicle_sil::control_silkit the control agent's messages over SIL Kit publish/subscribe (both ends)
#   vehicle_sil::ecu        the interface an ECU's application is written against and its description (no SIL Kit)
#   vehicle_sil::local      ECUs on virtual CAN and LIN buses in this process, on virtual time (no SIL Kit; internal)
#   vehicle_sil::testkit    recorders and fault injection for tests on vehicle_sil::local (internal)
#   vehicle_sil::peer       one SIL Kit participant with its J1939 networks, raw CAN and LIN nodes (internal)
#   vehicle_sil::simulation runs ECUs on local virtual buses or as SIL Kit participants of this process;
#                           ecu_main(), the main() of an executable that is one ECU
#   vehicle_sil::process    child processes for the tools and the tests (internal)
#   vehicle_sil::launch     the simulation configuration of vehicle_sil_launch and its check (internal)
# The J1939 stack, the CAN base, the LIN port and buses, the local buses, the test kit, Peer, Ecu
# and Simulation are under development; their API may still change.
# As a subproject (FetchContent / add_subdirectory) the libraries are excluded
# from "all", so a consumer only builds the libraries it links against.
if(PROJECT_IS_TOP_LEVEL)
  set(_vehicle_sil_exclude "")
else()
  set(_vehicle_sil_exclude EXCLUDE_FROM_ALL)
endif()
add_subdirectory(libs/core ${_vehicle_sil_exclude})
add_subdirectory(libs/step ${_vehicle_sil_exclude})
add_subdirectory(libs/lin ${_vehicle_sil_exclude})
add_subdirectory(libs/lin_silkit ${_vehicle_sil_exclude})
add_subdirectory(libs/can ${_vehicle_sil_exclude})
add_subdirectory(libs/can_silkit ${_vehicle_sil_exclude})
add_subdirectory(libs/can_netsim ${_vehicle_sil_exclude})
add_subdirectory(libs/eth ${_vehicle_sil_exclude})
add_subdirectory(libs/eth_silkit ${_vehicle_sil_exclude})
add_subdirectory(libs/eth_link ${_vehicle_sil_exclude})
add_subdirectory(libs/j1939 ${_vehicle_sil_exclude})
add_subdirectory(libs/control ${_vehicle_sil_exclude})
add_subdirectory(libs/control_silkit ${_vehicle_sil_exclude})
add_subdirectory(libs/ecu ${_vehicle_sil_exclude})
add_subdirectory(libs/local ${_vehicle_sil_exclude})
add_subdirectory(libs/testkit ${_vehicle_sil_exclude})
add_subdirectory(libs/peer ${_vehicle_sil_exclude})
add_subdirectory(libs/simulation ${_vehicle_sil_exclude})
add_subdirectory(libs/process ${_vehicle_sil_exclude})
add_subdirectory(libs/launch ${_vehicle_sil_exclude})
if(VEHICLE_SIL_BUILD_CONTROL_SERVER)
  add_subdirectory(proto ${_vehicle_sil_exclude})
  add_subdirectory(libs/control_grpc ${_vehicle_sil_exclude})
endif()
# What the bridge executables share: run_bridge() (stop handling, monitor loop, exit code).
add_subdirectory(bridges/common ${_vehicle_sil_exclude})
# Vector XL CAN bridge: portable core everywhere, XL backend and executable on Windows only.
add_subdirectory(bridges/vector-xl ${_vehicle_sil_exclude})
# Ethernet bridge: the bridge between a SIL Kit Ethernet network and an external link (portable).
add_subdirectory(bridges/ethernet ${_vehicle_sil_exclude})
# Command-line helpers shared by the executables, and the system controller for coordinated
# virtual-time simulations.
add_subdirectory(tools/common ${_vehicle_sil_exclude})
add_subdirectory(tools/sim_controller ${_vehicle_sil_exclude})
add_subdirectory(tools/can_bus_simulator ${_vehicle_sil_exclude})
# vehicle_sil_launch: a simulation of ECU executables from a configuration file.
add_subdirectory(tools/launch ${_vehicle_sil_exclude})

if(VEHICLE_SIL_BUILD_EXAMPLES)
  add_subdirectory(examples/getting_started)
  add_subdirectory(examples/typed_messages)
  add_subdirectory(examples/pid_stream)
  add_subdirectory(examples/fault_injection)
  add_subdirectory(examples/vehicle)
  add_subdirectory(examples/low_level)
  add_subdirectory(examples/j1939)
  add_subdirectory(examples/virtual_time)
  add_subdirectory(examples/lin)
  add_subdirectory(examples/ecu_main)
endif()

if(VEHICLE_SIL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
