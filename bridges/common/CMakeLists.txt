# What the bridge executables share, internal (not part of the library API).
#
#   vehicle_sil::bridge_runtime  run_bridge(): after a bridge's setup, the stop handlers, the
#                                monitor loop, the order of stopping and the exit code.
#                                Platform independent (the Windows console handling is inside).
#                                describe_json(): what a bridge writes for --describe.
#                                NothingCameHint: when a bridge says that nothing came back
#                                from the bus it sends to.
#                                format_link_line(): the "link <key>=<value>" line.
#                                DeviceMark, DeviceShareWatch: the mark a bridge puts on its
#                                device, and the other bridges' marks it counts
#                                (device_mark_os.cpp: Linux, Windows, none elsewhere).
add_library(vehicle_sil_bridge_runtime STATIC src/describe.cpp src/nothing_came_hint.cpp src/link_line.cpp
  src/device_mark.cpp src/device_mark_os.cpp src/run_bridge.cpp src/registry_failure.cpp)
add_library(vehicle_sil::bridge_runtime ALIAS vehicle_sil_bridge_runtime)
set_target_properties(vehicle_sil_bridge_runtime PROPERTIES EXPORT_NAME bridge_runtime)
target_include_directories(vehicle_sil_bridge_runtime PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_bridge_runtime PUBLIC SilKit::SilKit PRIVATE vehicle_sil::core vehicle_sil::json vehicle_sil_cli)
if(WIN32)
  # The security descriptor of the device mark (ConvertStringSecurityDescriptorToSecurityDescriptorW).
  target_link_libraries(vehicle_sil_bridge_runtime PRIVATE advapi32)
endif()
# "vehicle_sil" of --describe: the version of the vehicle_sil project, also as a subproject.
set_source_files_properties(src/describe.cpp PROPERTIES COMPILE_DEFINITIONS VEHICLE_SIL_VERSION="${VEHICLE_SIL_VERSION_FULL}")
vehicle_sil_target_defaults(vehicle_sil_bridge_runtime)
