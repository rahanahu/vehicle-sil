# vehicle_sil::peer - one SIL Kit participant with its J1939 networks (vehicle_sil::j1939
# on vehicle_sil::can_silkit ports or external ports), raw CAN channels, LIN nodes (on
# vehicle_sil::lin_silkit ports), Ethernet channels (on vehicle_sil::eth_silkit ports) and its control agent (vehicle_sil::control, with the peer's
# monitoring outputs), assembled and run on virtual or real time. Uses only the public APIs of
# the libraries below. Internal for now: the API may change.
add_library(vehicle_sil_peer STATIC
  src/peer.cpp
  src/peer_monitor.cpp
)
add_library(vehicle_sil::peer ALIAS vehicle_sil_peer)
set_target_properties(vehicle_sil_peer PROPERTIES EXPORT_NAME peer)
target_include_directories(vehicle_sil_peer PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_peer PUBLIC vehicle_sil::core vehicle_sil::can vehicle_sil::can_silkit vehicle_sil::ecu
                                              vehicle_sil::j1939 vehicle_sil::lin_silkit vehicle_sil::eth_silkit
                                              vehicle_sil::control)
vehicle_sil_target_defaults(vehicle_sil_peer)
