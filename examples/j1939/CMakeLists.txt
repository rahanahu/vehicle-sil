# Minimal J1939 peers used by the integration tests: the sender runs the J1939 stack on a
# vehicle_sil::Peer in real time; the receiver checks its frames on plain SIL Kit CAN, without
# the stack.
add_executable(vehicle_sil_j1939_sender sender/main.cpp common/peer_common.hpp)
target_link_libraries(vehicle_sil_j1939_sender PRIVATE vehicle_sil::peer vehicle_sil_cli)
add_executable(vehicle_sil_j1939_receiver receiver/main.cpp common/peer_common.hpp)
target_link_libraries(vehicle_sil_j1939_receiver PRIVATE vehicle_sil::core vehicle_sil_cli)
foreach(peer IN ITEMS sender receiver)
  vehicle_sil_target_defaults(vehicle_sil_j1939_${peer})
  vehicle_sil_copy_runtime_dlls(vehicle_sil_j1939_${peer})
endforeach()

# The ECU application API: two ECUs built from the J1939 parts (ecu_pair.hpp, also used by the
# tests), run as two SIL Kit participants of one process.
add_executable(vehicle_sil_j1939_ecu_pair ecu_pair/main.cpp ecu_pair/ecu_pair.hpp)
target_link_libraries(vehicle_sil_j1939_ecu_pair PRIVATE vehicle_sil::peer vehicle_sil::j1939_diagnostics
                      vehicle_sil::j1939_periodic_sender vehicle_sil::j1939_requester vehicle_sil::j1939_monitor)
vehicle_sil_target_defaults(vehicle_sil_j1939_ecu_pair)
vehicle_sil_copy_runtime_dlls(vehicle_sil_j1939_ecu_pair)
