# vehicle_sil_launch (library) - the simulation configuration file of vehicle_sil_launch, the
# check of the ecu_main executables against it (what they report with --describe) and the run of
# the whole simulation: reading, the rules (pure functions), the run (registry, controller, child
# processes) and the output for people and as JSON. Internal; not part of the
# library API. The file format: docs/simulation-config.md; the rules: docs/simulation-check.md.
if(NOT TARGET rapidyaml)
  message(STATUS "vehicle-sil: SIL Kit's rapidyaml target is not available (SIL Kit not built from source); vehicle_sil_launch is skipped")
  return()
endif()

add_library(vehicle_sil_launch_lib STATIC
  src/config.cpp
  src/check.cpp
  src/check_format.cpp
  src/check_rules.cpp
  src/describe_read.cpp
  src/events.cpp
  src/host_addresses.cpp
  src/run.cpp
  src/run_assess.cpp
  src/run_control.cpp
  src/run_format.cpp
  src/run_notes.cpp
  src/run_output.cpp
  src/run_participants.cpp
  src/run_scenario.cpp
  src/run_start.cpp
  src/run_watch.cpp
  src/scenario.cpp
  src/scenario_run.cpp
  src/test.cpp
  src/time_commands.cpp)
add_library(vehicle_sil::launch ALIAS vehicle_sil_launch_lib)
target_include_directories(vehicle_sil_launch_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_launch_lib PUBLIC vehicle_sil::ecu vehicle_sil::process)
# The simulation time (run.hpp: TimeCommands::wait_paused()); no SIL Kit.
target_link_libraries(vehicle_sil_launch_lib PUBLIC vehicle_sil::sim_base)
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
vehicle_sil_target_defaults(vehicle_sil_launch_lib)
