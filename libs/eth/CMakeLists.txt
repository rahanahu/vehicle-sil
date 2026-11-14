# vehicle_sil::eth - raw Ethernet frames on vehicle_sil::step: frame and MAC address values, the
# port contract, the channel an application sends and receives frames on (a step::Member) and the
# in-process virtual network. vehicle-sil does not interpret EtherTypes or payloads here.
# Must build without SIL Kit: link only vehicle_sil::step and vehicle_sil::sim_base.
add_library(vehicle_sil_eth STATIC
  src/channel.cpp
  src/frame.cpp
  src/virtual_ethernet_bus.cpp
)
add_library(vehicle_sil::eth ALIAS vehicle_sil_eth)
set_target_properties(vehicle_sil_eth PROPERTIES EXPORT_NAME eth)
target_include_directories(vehicle_sil_eth PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_eth PUBLIC vehicle_sil::step vehicle_sil::sim_base)
vehicle_sil_target_defaults(vehicle_sil_eth)
