# vehicle_sil_runner (library) - the run of the whole simulation of vehicle_sil_launch run: the
# registry, the controller, the child processes, the control hub, the scenario's steps, the time
# and stop commands and their rules, the run's outcome. It takes a checked configuration and a plan
# and knows no request file, output format or command line (the launcher's library, libs/launch,
# which links it). Internal; not part of the library API.
if(NOT TARGET rapidyaml)
  message(STATUS "vehicle-sil: SIL Kit's rapidyaml target is not available (SIL Kit not built from source); vehicle_sil_runner is skipped")
  return()
endif()

add_library(vehicle_sil_runner STATIC
  src/control_rules.cpp
  src/host_addresses.cpp
  src/run.cpp
  src/run_assess.cpp
  src/run_can_recorder.cpp
  src/run_connect.cpp
  src/run_control.cpp
  src/run_controls.cpp
  src/run_notes.cpp
  src/run_outcome.cpp
  src/run_output.cpp
  src/run_outside.cpp
  src/run_participants.cpp
  src/run_plan.cpp
  src/run_recorder.cpp
  src/run_samples.cpp
  src/run_scenario.cpp
  src/run_start.cpp
  src/run_traffic.cpp
  src/run_watch.cpp
  src/time_commands.cpp
)
add_library(vehicle_sil::runner ALIAS vehicle_sil_runner)
target_include_directories(vehicle_sil_runner PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# The public headers include the scenario's, the configuration's and the check's (RunPlan holds a
# CheckReport and a ScenarioCheck).
target_link_libraries(vehicle_sil_runner PUBLIC vehicle_sil::scenario vehicle_sil::config)
# The public headers include control's (hub.hpp), the process headers (process.hpp) and the
# simulation time (sim_time.hpp); no SIL Kit.
target_link_libraries(vehicle_sil_runner PUBLIC vehicle_sil::control vehicle_sil::process vehicle_sil::sim_base)
# The description of an ECU (monitor_names.hpp, describe.hpp): config carries it too, but the runner
# includes it itself.
target_link_libraries(vehicle_sil_runner PRIVATE vehicle_sil::ecu)
# The registry and the controller of the run, and the hub's end of the ECUs' control agents on the
# controller's participant. Only the .cpp files and the private headers (src/) include them; the
# public headers keep them behind RunnerImpl (runner/detail/runner.hpp).
target_link_libraries(vehicle_sil_runner PRIVATE vehicle_sil::core vehicle_sil::control_silkit)
# The scenario's ticks and writes record into the profiling trace (docs/internals/trace.md).
target_link_libraries(vehicle_sil_runner PRIVATE vehicle_sil::trace)
# This computer's addresses (host_addresses.cpp): GetAdaptersAddresses on Windows.
if(WIN32)
  target_link_libraries(vehicle_sil_runner PRIVATE iphlpapi ws2_32)
endif()
# Tests only (VEHICLE_SIL_FAULT_INJECTION): public, so that every file that includes run_connection.hpp
# sees the same fault_close_delay().
if(VEHICLE_SIL_FAULT_INJECTION)
  target_compile_definitions(vehicle_sil_runner PUBLIC $<$<CONFIG:Debug>:VEHICLE_SIL_FAULT_INJECTION>)
endif()
vehicle_sil_target_defaults(vehicle_sil_runner)
