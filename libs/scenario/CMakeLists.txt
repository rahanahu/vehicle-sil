# vehicle_sil_scenario (library) - the scenario of vehicle_sil_launch: its file, the engine that
# steps through it on a run's variables, the timeline it writes and the part of a check that reads
# it (pure: no process, no SIL Kit). Internal; not part of the library API. The file format:
# docs/scenario.md.
if(NOT TARGET rapidyaml)
  message(STATUS "vehicle-sil: SIL Kit's rapidyaml target is not available (SIL Kit not built from source); vehicle_sil_scenario is skipped")
  return()
endif()

add_library(vehicle_sil_scenario STATIC
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
  src/timeline.cpp
)
add_library(vehicle_sil::scenario ALIAS vehicle_sil_scenario)
target_include_directories(vehicle_sil_scenario PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# The configuration (the scenario is read against it) and the check that scenario_check.hpp extends.
target_link_libraries(vehicle_sil_scenario PUBLIC vehicle_sil::config)
# The public headers include control's (hub.hpp, agent.hpp, variable_map.hpp); no SIL Kit.
target_link_libraries(vehicle_sil_scenario PUBLIC vehicle_sil::control)
# The simulation time (sim_time.hpp); no SIL Kit.
target_link_libraries(vehicle_sil_scenario PUBLIC vehicle_sil::sim_base)
# The shapes of the wave steps (scenario_shapes.cpp); header only.
target_link_libraries(vehicle_sil_scenario PRIVATE vehicle_sil::signal)
# The YAML/JSON parser that SIL Kit builds from its ThirdParty directory (rapidyaml, MIT); the
# reader (vehicle_sil/config/detail/yaml_reader.hpp) includes it.
target_link_libraries(vehicle_sil_scenario PRIVATE rapidyaml)
vehicle_sil_target_defaults(vehicle_sil_scenario)
