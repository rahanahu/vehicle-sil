# What the bridge executables share, internal (not part of the library API).
#
#   vehicle_sil::bridge_runtime  run_bridge(): after a bridge's setup, the stop handlers, the
#                                monitor loop, the order of stopping and the exit code.
#                                Platform independent (the Windows console handling is inside).
add_library(vehicle_sil_bridge_runtime STATIC src/run_bridge.cpp src/registry_failure.cpp)
add_library(vehicle_sil::bridge_runtime ALIAS vehicle_sil_bridge_runtime)
set_target_properties(vehicle_sil_bridge_runtime PROPERTIES EXPORT_NAME bridge_runtime)
target_include_directories(vehicle_sil_bridge_runtime PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_bridge_runtime PUBLIC SilKit::SilKit PRIVATE vehicle_sil::core vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_bridge_runtime)
