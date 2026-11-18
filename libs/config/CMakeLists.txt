# vehicle_sil_config (library) - the simulation configuration file of vehicle_sil_launch and its
# check against what the ecu_main executables report with --describe: reading, and the rules (pure
# functions: no process, no SIL Kit). Internal; not part of the library API. The file format:
# docs/simulation-config.md; the rules: docs/simulation-check.md.
if(NOT TARGET rapidyaml)
  message(STATUS "vehicle-sil: SIL Kit's rapidyaml target is not available (SIL Kit not built from source); vehicle_sil_config is skipped")
  return()
endif()

add_library(vehicle_sil_config STATIC
  src/check_connect.cpp
  src/check_networks.cpp
  src/check_report.cpp
  src/check_rules.cpp
  src/config.cpp
  src/connect.cpp
  src/describe_read.cpp
  src/seen_ecu.cpp
  src/time_plan.cpp
)
add_library(vehicle_sil::config ALIAS vehicle_sil_config)
target_include_directories(vehicle_sil_config PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_config PUBLIC vehicle_sil::ecu)
# The simulation time and the loopback test of a registry URI (registry_uri.hpp); no SIL Kit.
target_link_libraries(vehicle_sil_config PUBLIC vehicle_sil::sim_base)
# The public headers include control's (variable_map.hpp, wire.hpp); no SIL Kit.
target_link_libraries(vehicle_sil_config PUBLIC vehicle_sil::control)
# The conversion of a path from the file's form to this computer's (process/paths.hpp). Only that
# header: the processes are the run's (cmake/VehicleSilCheckIncludes.cmake guards this).
target_link_libraries(vehicle_sil_config PRIVATE vehicle_sil::process)
# The JSON writer that the launcher shares with --describe (vehicle_sil/json/json_writer.hpp).
target_link_libraries(vehicle_sil_config PRIVATE vehicle_sil::json)
# The YAML/JSON parser that SIL Kit builds from its ThirdParty directory (rapidyaml, MIT). The
# headers of vehicle_sil/config/detail/ (yaml_tree.hpp, yaml_reader.hpp) include it, so a library
# that includes them links it too.
target_link_libraries(vehicle_sil_config PRIVATE rapidyaml)
vehicle_sil_target_defaults(vehicle_sil_config)
