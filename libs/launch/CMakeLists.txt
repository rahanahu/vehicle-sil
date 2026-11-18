# vehicle_sil_launch (library) - the run of the whole simulation of vehicle_sil_launch (registry,
# controller, child processes), the scenario and the output for people and as JSON. The
# configuration file and its check are vehicle_sil::config (libs/config). Internal; not part of the
# library API. The file format: docs/simulation-config.md; the rules: docs/simulation-check.md.
if(NOT TARGET rapidyaml)
  message(STATUS "vehicle-sil: SIL Kit's rapidyaml target is not available (SIL Kit not built from source); vehicle_sil_launch is skipped")
  return()
endif()

add_library(vehicle_sil_launch_lib STATIC
  src/check.cpp
  src/check_format.cpp
  src/command_input.cpp
  src/connections.cpp
  src/control_rules.cpp
  src/error_result.cpp
  src/events.cpp
  src/host_addresses.cpp
  src/probe.cpp
  src/reported_networks.cpp
  src/request.cpp
  src/request_read.cpp
  src/run.cpp
  src/run_assess.cpp
  src/run_connect.cpp
  src/run_control.cpp
  src/run_controls.cpp
  src/run_file.cpp
  src/run_format.cpp
  src/run_notes.cpp
  src/run_output.cpp
  src/run_outside.cpp
  src/run_participants.cpp
  src/run_plan.cpp
  src/run_report.cpp
  src/run_samples.cpp
  src/run_scenario.cpp
  src/run_start.cpp
  src/run_traffic.cpp
  src/run_watch.cpp
  src/scenario.cpp
  src/scenario_check.cpp
  src/scenario_csv.cpp
  src/scenario_failure.cpp
  src/scenario_inputs.cpp
  src/scenario_line.cpp
  src/scenario_run.cpp
  src/scenario_shapes.cpp
  src/scenario_values.cpp
  src/scenario_waves.cpp
  src/test.cpp
  src/timeline.cpp
  src/timeline_format.cpp
  src/time_commands.cpp
)
add_library(vehicle_sil::launch ALIAS vehicle_sil_launch_lib)
target_include_directories(vehicle_sil_launch_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# The configuration and its check (vehicle_sil/config/...), which the public headers include.
target_link_libraries(vehicle_sil_launch_lib PUBLIC vehicle_sil::config)
target_link_libraries(vehicle_sil_launch_lib PUBLIC vehicle_sil::ecu vehicle_sil::process)
# The simulation time (time_commands.hpp: TimeCommands::wait_paused()); no SIL Kit.
target_link_libraries(vehicle_sil_launch_lib PUBLIC vehicle_sil::sim_base)
# The public headers include control's (hub.hpp, agent.hpp, variable_map.hpp).
target_link_libraries(vehicle_sil_launch_lib PUBLIC vehicle_sil::control)
# The JSON writer that the launcher shares with --describe (vehicle_sil/json/json_writer.hpp).
target_link_libraries(vehicle_sil_launch_lib PRIVATE vehicle_sil::json)
# The shapes of a scenario's wave steps (scenario_shapes.cpp); header only.
target_link_libraries(vehicle_sil_launch_lib PRIVATE vehicle_sil::signal)
# The registry and the controller of vehicle_sil_launch run.
target_link_libraries(vehicle_sil_launch_lib PRIVATE vehicle_sil::core)
# The hub's end of the ECUs' control agents, on the controller's participant.
target_link_libraries(vehicle_sil_launch_lib PRIVATE vehicle_sil::control_silkit)
# The YAML/JSON parser that SIL Kit builds from its ThirdParty directory (rapidyaml, MIT); the
# public headers do not include it.
target_link_libraries(vehicle_sil_launch_lib PRIVATE rapidyaml)
# This computer's addresses (host_addresses.cpp): GetAdaptersAddresses on Windows.
if(WIN32)
  target_link_libraries(vehicle_sil_launch_lib PRIVATE iphlpapi ws2_32)
endif()
# The Control Server of run --control (src/control_server.cpp): gRPC on the run's hub. Only with
# VEHICLE_SIL_BUILD_CONTROL_SERVER; no public header includes control_grpc's. Without it, a run
# that is asked for a control server has none to make (control_server_none.cpp).
if(TARGET vehicle_sil::control_grpc)
  target_sources(vehicle_sil_launch_lib PRIVATE src/control_server.cpp)
  target_link_libraries(vehicle_sil_launch_lib PRIVATE vehicle_sil::control_grpc)
else()
  target_sources(vehicle_sil_launch_lib PRIVATE src/control_server_none.cpp)
endif()
vehicle_sil_target_defaults(vehicle_sil_launch_lib)
