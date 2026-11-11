# vehicle_sil_launch (library) - the simulation configuration file of vehicle_sil_launch and the
# check of the ecu_main executables against it (what they report with --describe): reading,
# the rules (pure functions) and the output for people and as JSON. Internal; not part of the
# library API. The file format and the rules: docs/simulation-config.md.
if(NOT TARGET rapidyaml)
  message(STATUS "vehicle-sil: SIL Kit's rapidyaml target is not available (SIL Kit not built from source); vehicle_sil_launch is skipped")
  return()
endif()

add_library(vehicle_sil_launch_lib STATIC
  src/config.cpp
  src/check.cpp)
add_library(vehicle_sil::launch ALIAS vehicle_sil_launch_lib)
target_include_directories(vehicle_sil_launch_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_launch_lib PUBLIC vehicle_sil::ecu vehicle_sil::process)
# The YAML/JSON parser that SIL Kit builds from its ThirdParty directory (rapidyaml, MIT); the
# public headers do not include it.
target_link_libraries(vehicle_sil_launch_lib PRIVATE rapidyaml)
vehicle_sil_target_defaults(vehicle_sil_launch_lib)
