# Ethernet bridge between a SIL Kit Ethernet network and an external Ethernet link.
#
#   vehicle_sil::ethernet_bridge_core  platform independent: EthernetBridge (forwarding between
#                                      IEthernetController and eth_link::ExternalLink, statistics,
#                                      the monitor line) and the command line parsing. Built and
#                                      tested on every platform, against fake links.
add_library(vehicle_sil_ethernet_bridge_core STATIC
  src/ethernet_bridge.cpp
  src/bridge_options.cpp)
add_library(vehicle_sil::ethernet_bridge_core ALIAS vehicle_sil_ethernet_bridge_core)
set_target_properties(vehicle_sil_ethernet_bridge_core PROPERTIES EXPORT_NAME ethernet_bridge_core)
target_include_directories(vehicle_sil_ethernet_bridge_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_ethernet_bridge_core PUBLIC SilKit::SilKit vehicle_sil::eth_link)
vehicle_sil_target_defaults(vehicle_sil_ethernet_bridge_core)
