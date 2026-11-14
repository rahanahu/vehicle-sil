# Ethernet bridge between a SIL Kit Ethernet network and an external Ethernet link.
#
#   vehicle_sil::ethernet_bridge_core  platform independent: EthernetBridge (forwarding between
#                                      IEthernetController and eth_link::ExternalLink, statistics,
#                                      the monitor line), the command line parsing, and what the
#                                      executable says (the start line, which backend works on
#                                      which OS, the hint about promiscuous mode). Built and
#                                      tested on every platform, against fake links.
#   vehicle_sil_ethernet_bridge        the executable, on every platform: app/main.cpp on
#                                      run_bridge() (bridges/common) with the links of this OS
#                                      (app/os_link_factory.cpp: tap, packet and pcap on Linux,
#                                      npcap on Windows; the pcap library is loaded at run time).
add_library(vehicle_sil_ethernet_bridge_core STATIC
  src/ethernet_bridge.cpp
  src/bridge_app.cpp
  src/bridge_options.cpp)
add_library(vehicle_sil::ethernet_bridge_core ALIAS vehicle_sil_ethernet_bridge_core)
set_target_properties(vehicle_sil_ethernet_bridge_core PROPERTIES EXPORT_NAME ethernet_bridge_core)
target_include_directories(vehicle_sil_ethernet_bridge_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_ethernet_bridge_core PUBLIC SilKit::SilKit vehicle_sil::eth_link)
vehicle_sil_target_defaults(vehicle_sil_ethernet_bridge_core)

add_executable(vehicle_sil_ethernet_bridge app/main.cpp app/os_link_factory.cpp app/pcap_library.cpp)
target_link_libraries(vehicle_sil_ethernet_bridge
  PRIVATE vehicle_sil::ethernet_bridge_core vehicle_sil::bridge_runtime vehicle_sil::core vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_ethernet_bridge)
vehicle_sil_copy_runtime_dlls(vehicle_sil_ethernet_bridge)
