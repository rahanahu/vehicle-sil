# vehicle_sil::Peer on real participants (in-process registry): J1939 and LIN nodes, the same
# results as the local virtual bus, real time with an external port. The example's typed
# messages (vehicle_sil_ecu_pair_messages) come from the examples or from tests/testkit.
vehicle_sil_add_harness_tests(vehicle_sil_peer_tests
  PREFIX peer LABELS unit peer j1939 TIMEOUT 180
  SOURCES test_peer.cpp
  LINK vehicle_sil::peer vehicle_sil::testkit vehicle_sil_ecu_pair_messages vehicle_sil::j1939_diagnostics
       vehicle_sil::j1939_periodic_sender vehicle_sil::j1939_requester vehicle_sil::j1939_monitor)
target_include_directories(vehicle_sil_peer_tests PRIVATE ${PROJECT_SOURCE_DIR}/examples/j1939/ecu_pair)
