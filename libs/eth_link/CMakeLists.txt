# vehicle_sil::eth_link - the external side of an Ethernet bridge, without SIL Kit: the interface
# of an external Ethernet link (ExternalLink, LinkEvent, LinkError), MAC addresses, the frame
# filter (FrameFilter: EtherType behind VLAN tags, MAC, length), the table that keeps a bridge
# from forwarding frames in a loop (MacSideTable), and the links the OS provides without a library
# (os_links.hpp: on Linux a TAP device and a packet socket), and the link through the pcap library
# the OS has installed (pcap_link.hpp: Npcap on Windows, libpcap elsewhere). The pcap library is
# loaded at run time: nothing of it is needed to build, and nothing of it is linked.
# Must build without SIL Kit: link nothing of vehicle-sil (checked by the test eth_link.boundary).
find_package(Threads REQUIRED) # MacSideTable uses std::mutex

add_library(vehicle_sil_eth_link STATIC
  src/external_link.cpp
  src/frame_filter.cpp
  src/mac_address.cpp
  src/mac_side_table.cpp
  src/os_link_errors.cpp
  src/os_links.cpp
  src/pcap/pcap_errors.cpp
  src/pcap/pcap_link.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(vehicle_sil_eth_link PRIVATE
    src/linux/linux_link.cpp
    src/linux/os_links_linux.cpp
    src/linux/packet_link.cpp
    src/linux/tap_link.cpp)
else()
  target_sources(vehicle_sil_eth_link PRIVATE src/os_links_none.cpp)
endif()
if(WIN32)
  # iphlpapi: the names of the network connections for --list-interfaces and --interface.
  target_sources(vehicle_sil_eth_link PRIVATE src/pcap/pcap_loader_win.cpp)
  target_link_libraries(vehicle_sil_eth_link PRIVATE iphlpapi)
else()
  target_sources(vehicle_sil_eth_link PRIVATE src/pcap/pcap_loader_posix.cpp)
  target_link_libraries(vehicle_sil_eth_link PRIVATE ${CMAKE_DL_LIBS})
endif()
add_library(vehicle_sil::eth_link ALIAS vehicle_sil_eth_link)
set_target_properties(vehicle_sil_eth_link PROPERTIES EXPORT_NAME eth_link)
target_include_directories(vehicle_sil_eth_link PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_eth_link PUBLIC Threads::Threads)
vehicle_sil_target_defaults(vehicle_sil_eth_link)
