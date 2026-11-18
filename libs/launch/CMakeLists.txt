# vehicle_sil_launch (library) - the use cases of vehicle_sil_launch (check, run, test, probe, connections),
# the request and the output for people and as JSON. The run itself is vehicle_sil::runner
# (libs/runner); the configuration file and its check are vehicle_sil::config (libs/config); the
# scenario is vehicle_sil::scenario (libs/scenario). Internal; not part of the library API. The file format: docs/simulation-config.md; the rules: docs/simulation-check.md.
if(NOT TARGET rapidyaml)
  message(STATUS "vehicle-sil: SIL Kit's rapidyaml target is not available (SIL Kit not built from source); vehicle_sil_launch is skipped")
  return()
endif()

add_library(vehicle_sil_launch_lib STATIC
  src/check.cpp
  src/check_format.cpp
  src/command_input.cpp
  src/connections.cpp
  src/error_result.cpp
  src/events.cpp
  src/probe.cpp
  src/reported_networks.cpp
  src/request.cpp
  src/request_read.cpp
  src/run_file.cpp
  src/run_format.cpp
  src/run_report.cpp
  src/test.cpp
  src/timeline_format.cpp
)
add_library(vehicle_sil::launch ALIAS vehicle_sil_launch_lib)
target_include_directories(vehicle_sil_launch_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# The run (vehicle_sil/runner/...: the plan, the controls, the events, the outcome), the configuration
# and its check (vehicle_sil/config/...) and the scenario (vehicle_sil/scenario/...), which the public
# headers include. The runner brings control and the simulation time with it.
target_link_libraries(vehicle_sil_launch_lib PUBLIC vehicle_sil::runner vehicle_sil::scenario vehicle_sil::config)
# The process headers: events.hpp, a public header, includes process.hpp (child_process.hpp and paths.hpp
# are used by the probe, the test and the output).
target_link_libraries(vehicle_sil_launch_lib PUBLIC vehicle_sil::process)
# The description of an ECU (describe.hpp, describe_json.hpp) that the outputs repeat.
target_link_libraries(vehicle_sil_launch_lib PRIVATE vehicle_sil::ecu)
# The JSON writer that the launcher shares with --describe (vehicle_sil/json/json_writer.hpp).
target_link_libraries(vehicle_sil_launch_lib PRIVATE vehicle_sil::json)
# The YAML/JSON parser that SIL Kit builds from its ThirdParty directory (rapidyaml, MIT); the
# public headers do not include it.
target_link_libraries(vehicle_sil_launch_lib PRIVATE rapidyaml)
# The Control Server of run --control (src/control_server.cpp): gRPC on the run's hub. Only with
# VEHICLE_SIL_BUILD_CONTROL_SERVER; no public header includes control_grpc's. Without it, a run
# that is asked for a control server has none to make (control_server_none.cpp).
if(TARGET vehicle_sil::control_grpc)
  target_sources(vehicle_sil_launch_lib PRIVATE src/control_server.cpp)
  # control_server.cpp reads the controller's and the participant's SIL Kit types (the stop of the
  # simulation, the failure check), so this is the only place where the launcher's library links core.
  target_link_libraries(vehicle_sil_launch_lib PRIVATE vehicle_sil::control_grpc vehicle_sil::core)
else()
  target_sources(vehicle_sil_launch_lib PRIVATE src/control_server_none.cpp)
endif()
vehicle_sil_target_defaults(vehicle_sil_launch_lib)
