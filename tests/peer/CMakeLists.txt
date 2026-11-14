# vehicle_sil::Peer on real participants (in-process registry): J1939 and LIN nodes, the same
# results as the local virtual bus, real time with an external port. The example's typed
# messages (vehicle_sil_ecu_pair_messages) come from the examples or from tests/testkit.
vehicle_sil_add_harness_tests(vehicle_sil_peer_tests
  PREFIX peer LABELS unit peer j1939 TIMEOUT 180
  SOURCES test_peer.cpp
  LINK vehicle_sil::peer vehicle_sil::testkit vehicle_sil_ecu_pair_messages vehicle_sil::j1939_diagnostics
       vehicle_sil::j1939_periodic_sender vehicle_sil::j1939_requester vehicle_sil::j1939_monitor)
target_include_directories(vehicle_sil_peer_tests PRIVATE ${PROJECT_SOURCE_DIR}/examples/j1939/ecu_pair)

# Every SIL Kit bus controller of the libraries is made in libs/peer/src/peer.cpp on the network's
# name in the simulation (Peer::simulation_name(), PeerOptions::network_names); the check itself
# must reject a controller made on the code's name, and one made in another file.
add_test(NAME peer.controllers_are_made_on_the_simulation_name
         COMMAND ${CMAKE_COMMAND} -DDIRS=${PROJECT_SOURCE_DIR}/libs -DFILE=peer.cpp
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckControllerNetworks.cmake)
add_test(NAME peer.controller_check_rejects_a_direct_name
         COMMAND ${CMAKE_COMMAND} -DDIRS=${CMAKE_CURRENT_SOURCE_DIR}/controller_fixtures/direct -DFILE=peer.cpp
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckControllerNetworks.cmake)
set_tests_properties(peer.controllers_are_made_on_the_simulation_name peer.controller_check_rejects_a_direct_name
                     PROPERTIES LABELS "unit;peer" TIMEOUT 60)
set_tests_properties(peer.controller_check_rejects_a_direct_name PROPERTIES
                     PASS_REGULAR_EXPRESSION "elsewhere\\.cpp: participant\\.CreateLinController(.|\n)*peer\\.cpp: participant\\.CreateCanController\\(network, network\\)")
