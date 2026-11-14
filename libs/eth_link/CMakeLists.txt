# vehicle_sil::eth_link - the external side of an Ethernet bridge, without SIL Kit: the interface
# of an external Ethernet link (ExternalLink, LinkEvent, LinkError), MAC addresses, the frame
# filter (FrameFilter: EtherType behind VLAN tags, MAC, length) and the table that keeps a bridge
# from forwarding frames in a loop (MacSideTable).
# Must build without SIL Kit: link nothing of vehicle-sil (checked by the test eth_link.boundary).
find_package(Threads REQUIRED) # MacSideTable uses std::mutex

add_library(vehicle_sil_eth_link STATIC
  src/external_link.cpp
  src/frame_filter.cpp
  src/mac_address.cpp
  src/mac_side_table.cpp)
add_library(vehicle_sil::eth_link ALIAS vehicle_sil_eth_link)
set_target_properties(vehicle_sil_eth_link PROPERTIES EXPORT_NAME eth_link)
target_include_directories(vehicle_sil_eth_link PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_eth_link PUBLIC Threads::Threads)
vehicle_sil_target_defaults(vehicle_sil_eth_link)
