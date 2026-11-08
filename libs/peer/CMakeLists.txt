# vehicle_sil::peer - one SIL Kit participant with its J1939 networks (vehicle_sil::j1939
# on vehicle_sil::can_silkit ports or external ports) and LIN nodes, assembled and run on
# virtual or real time. Uses only the public APIs of the libraries below. Internal for now:
# the API may change.
add_library(vehicle_sil_peer STATIC src/peer.cpp)
add_library(vehicle_sil::peer ALIAS vehicle_sil_peer)
set_target_properties(vehicle_sil_peer PROPERTIES EXPORT_NAME peer)
target_include_directories(vehicle_sil_peer PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_peer PUBLIC vehicle_sil::core vehicle_sil::can vehicle_sil::can_silkit
                                              vehicle_sil::j1939 vehicle_sil::lin)
vehicle_sil_target_defaults(vehicle_sil_peer)
