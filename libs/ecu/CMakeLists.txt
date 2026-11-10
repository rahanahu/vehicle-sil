# vehicle_sil::ecu - the interface an ECU's application is written against (vehicle_sil::Ecu:
# its J1939 networks and raw CAN channels, its work per step and the objects it owns), header
# only. Peer and the test kit implement it. No SIL Kit.
add_library(vehicle_sil_ecu INTERFACE)
add_library(vehicle_sil::ecu ALIAS vehicle_sil_ecu)
set_target_properties(vehicle_sil_ecu PROPERTIES EXPORT_NAME ecu)
target_include_directories(vehicle_sil_ecu INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_ecu INTERFACE vehicle_sil::can vehicle_sil::j1939)
target_compile_features(vehicle_sil_ecu INTERFACE cxx_std_20)
