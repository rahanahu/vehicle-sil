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
# tests), run by vehicle_sil::Simulation as two SIL Kit participants of one process or on
# virtual buses. Their messages are typed values
# generated from ecu_pair_messages.yaml.
vehicle_sil_add_j1939_codec(vehicle_sil_ecu_pair_messages ecu_pair/ecu_pair_messages.yaml)
add_executable(vehicle_sil_j1939_ecu_pair ecu_pair/main.cpp ecu_pair/ecu_pair.hpp)
target_link_libraries(vehicle_sil_j1939_ecu_pair PRIVATE vehicle_sil::simulation vehicle_sil_ecu_pair_messages vehicle_sil::j1939_diagnostics
                      vehicle_sil::j1939_periodic_sender vehicle_sil::j1939_requester vehicle_sil::j1939_monitor)
vehicle_sil_target_defaults(vehicle_sil_j1939_ecu_pair)
vehicle_sil_copy_runtime_dlls(vehicle_sil_j1939_ecu_pair)

# A protocol of one's own: a manufacturer's parameter service (param_service.hpp) built on the
# Exchange part and typed messages generated from param_service_messages.yaml, with the public
# API only. The demo runs on the local virtual bus (vehicle_sil::local, no SIL Kit); the tests use
# the same code (tests/testkit/CMakeLists.txt builds it when the examples are not built).
vehicle_sil_add_j1939_codec(vehicle_sil_param_service_messages param_service/param_service_messages.yaml)
add_library(vehicle_sil_param_service STATIC param_service/param_service.cpp param_service/param_service.hpp)
target_include_directories(vehicle_sil_param_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/param_service)
target_link_libraries(vehicle_sil_param_service PUBLIC vehicle_sil::j1939_exchange vehicle_sil_param_service_messages)
vehicle_sil_target_defaults(vehicle_sil_param_service)
add_executable(vehicle_sil_j1939_param_service param_service/main.cpp)
target_link_libraries(vehicle_sil_j1939_param_service PRIVATE vehicle_sil_param_service vehicle_sil::local)
vehicle_sil_target_defaults(vehicle_sil_j1939_param_service)
