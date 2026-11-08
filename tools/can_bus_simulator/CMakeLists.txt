# vehicle_sil_can_bus_simulator: the simple CAN network simulator as a SIL Kit participant,
# with the command line and configuration file of the Vector SIL Kit Network Simulator.
if(NOT TARGET vehicle_sil_can_netsim)
  return()
endif()
add_executable(vehicle_sil_can_bus_simulator main.cpp)
target_link_libraries(vehicle_sil_can_bus_simulator PRIVATE vehicle_sil::can_netsim vehicle_sil_cli)
target_compile_definitions(vehicle_sil_can_bus_simulator PRIVATE VEHICLE_SIL_VERSION="${PROJECT_VERSION}")
vehicle_sil_target_defaults(vehicle_sil_can_bus_simulator)
vehicle_sil_copy_runtime_dlls(vehicle_sil_can_bus_simulator)
