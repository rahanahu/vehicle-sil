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

# The lists below hold one directory a line, in the order of the names, so that two changes that
# each add one do not touch the same line. What each target holds: the table in
# docs/architecture.md; where its code and tests are: docs/code-map.md.
# The order of the groups matters (libraries, then bridges, then tools, then examples, then tests):
# a later group asks if(TARGET ...) for targets of an earlier one. Inside a group it does not.
#
# The J1939 stack, the CAN base, the LIN port and buses, the local buses, the test kit, Peer, Ecu
# and Simulation are under development; their API may still change.
# As a subproject (FetchContent / add_subdirectory) the libraries are excluded
# from "all", so a consumer only builds the libraries it links against.
if(PROJECT_IS_TOP_LEVEL)
  set(_vehicle_sil_exclude "")
else()
  set(_vehicle_sil_exclude EXCLUDE_FROM_ALL)
endif()
add_subdirectory(libs/can ${_vehicle_sil_exclude})
add_subdirectory(libs/can_netsim ${_vehicle_sil_exclude})
add_subdirectory(libs/can_silkit ${_vehicle_sil_exclude})
add_subdirectory(libs/control ${_vehicle_sil_exclude})
add_subdirectory(libs/control_silkit ${_vehicle_sil_exclude})
add_subdirectory(libs/core ${_vehicle_sil_exclude})
add_subdirectory(libs/ecu ${_vehicle_sil_exclude})
add_subdirectory(libs/eth ${_vehicle_sil_exclude})
add_subdirectory(libs/eth_link ${_vehicle_sil_exclude})
add_subdirectory(libs/eth_silkit ${_vehicle_sil_exclude})
add_subdirectory(libs/j1939 ${_vehicle_sil_exclude})
add_subdirectory(libs/launch ${_vehicle_sil_exclude})
add_subdirectory(libs/lin ${_vehicle_sil_exclude})
add_subdirectory(libs/lin_silkit ${_vehicle_sil_exclude})
add_subdirectory(libs/local ${_vehicle_sil_exclude})
add_subdirectory(libs/peer ${_vehicle_sil_exclude})
add_subdirectory(libs/process ${_vehicle_sil_exclude})
add_subdirectory(libs/simulation ${_vehicle_sil_exclude})
add_subdirectory(libs/step ${_vehicle_sil_exclude})
add_subdirectory(libs/testkit ${_vehicle_sil_exclude})
# The control server (gRPC). After the libraries above, and proto before control_grpc, which
# links the generated code.
if(VEHICLE_SIL_BUILD_CONTROL_SERVER)
  add_subdirectory(proto ${_vehicle_sil_exclude})
  add_subdirectory(libs/control_grpc ${_vehicle_sil_exclude})
endif()

# What the bridge executables share: run_bridge() (stop handling, monitor loop, exit code).
add_subdirectory(bridges/common ${_vehicle_sil_exclude})
# Ethernet bridge: the bridge between a SIL Kit Ethernet network and an external link (portable).
add_subdirectory(bridges/ethernet ${_vehicle_sil_exclude})
# Vector XL CAN bridge: portable core everywhere, XL backend and executable on Windows only.
add_subdirectory(bridges/vector-xl ${_vehicle_sil_exclude})

add_subdirectory(tools/can_bus_simulator ${_vehicle_sil_exclude})
# Command-line helpers shared by the executables.
add_subdirectory(tools/common ${_vehicle_sil_exclude})
# vehicle_sil_launch: a simulation of ECU executables from a configuration file.
add_subdirectory(tools/launch ${_vehicle_sil_exclude})
# The system controller for coordinated virtual-time simulations.
add_subdirectory(tools/sim_controller ${_vehicle_sil_exclude})

if(VEHICLE_SIL_BUILD_EXAMPLES)
  add_subdirectory(examples/ecu_main)
  add_subdirectory(examples/ethernet)
  add_subdirectory(examples/fault_injection)
  add_subdirectory(examples/getting_started)
  add_subdirectory(examples/j1939)
  add_subdirectory(examples/lin)
  add_subdirectory(examples/low_level)
  add_subdirectory(examples/pid_stream)
  add_subdirectory(examples/typed_messages)
  add_subdirectory(examples/vehicle)
  add_subdirectory(examples/virtual_time)
  # The YAML files that the examples copy next to their executables name their JSON Schema in the first line, by a
  # path relative to the file (# yaml-language-server: $schema=../../schema/simulation.schema.json). The schemas are
  # copied to the same places relative to the copies, so that an editor completes and checks a copy as it does the
  # source. A changed schema is copied again at the next build.
  foreach(_schema IN ITEMS
          schema/simulation.schema.json
          schema/scenario.schema.json
          gui/protocol/dashboard.schema.json)
    configure_file(${_schema} ${CMAKE_CURRENT_BINARY_DIR}/${_schema} COPYONLY)
  endforeach()
endif()

if(VEHICLE_SIL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
