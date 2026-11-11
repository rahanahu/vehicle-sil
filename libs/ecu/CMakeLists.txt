# vehicle_sil::ecu - the interface an ECU's application is written against (vehicle_sil::Ecu:
# its J1939 networks, raw CAN channels and LIN nodes, its control variables, its work per step
# and the objects it owns) and what an ECU reports about itself (Ecu::describe(), to_json()).
# Peer and LocalBench implement it. No SIL Kit.
add_library(vehicle_sil_ecu STATIC src/describe.cpp)
add_library(vehicle_sil::ecu ALIAS vehicle_sil_ecu)
set_target_properties(vehicle_sil_ecu PROPERTIES EXPORT_NAME ecu)
target_include_directories(vehicle_sil_ecu PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_ecu PUBLIC vehicle_sil::can vehicle_sil::j1939 vehicle_sil::lin vehicle_sil::control)
# vehicle_sil::version(): the version of the vehicle_sil project, also as a subproject.
target_compile_definitions(vehicle_sil_ecu PRIVATE VEHICLE_SIL_VERSION="${vehicle_sil_VERSION}")
vehicle_sil_target_defaults(vehicle_sil_ecu)
