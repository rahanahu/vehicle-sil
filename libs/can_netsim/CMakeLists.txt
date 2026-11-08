# vehicle_sil::can_netsim - a simple CAN network simulator on SIL Kit's network simulator API
# (arbitration, transmission time, bus occupancy with vehicle_sil::can::CanBusModel), and the
# network simulator configuration file shared with the Vector SIL Kit Network Simulator.
if(NOT TARGET rapidyaml)
  message(STATUS "vehicle-sil: SIL Kit's rapidyaml target is not available (SIL Kit not built from source); vehicle_sil::can_netsim is skipped")
  return()
endif()

add_library(vehicle_sil_can_netsim STATIC
  src/can_bus_simulator.cpp
  src/network_simulator_config.cpp)
add_library(vehicle_sil::can_netsim ALIAS vehicle_sil_can_netsim)
set_target_properties(vehicle_sil_can_netsim PROPERTIES EXPORT_NAME can_netsim)
target_include_directories(vehicle_sil_can_netsim PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_can_netsim PUBLIC vehicle_sil::can vehicle_sil::can_silkit vehicle_sil::core SilKit::SilKit)
# The YAML/JSON parser that SIL Kit builds from its ThirdParty directory (rapidyaml, MIT).
target_link_libraries(vehicle_sil_can_netsim PRIVATE rapidyaml)
vehicle_sil_target_defaults(vehicle_sil_can_netsim)
