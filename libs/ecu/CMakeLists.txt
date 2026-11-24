# vehicle_sil::ecu - the interface an ECU's application is written against (vehicle_sil::Ecu:
# its J1939 networks, raw CAN channels, LIN nodes and Ethernet channels, its control variables, its work per step
# and the objects it owns) and what an ECU reports about itself (Ecu::describe(), to_json()).
# Peer and LocalBench implement it. No SIL Kit.
add_library(vehicle_sil_ecu STATIC
  src/bus_connection.cpp
  src/describe.cpp
  src/j1939_node_argument.cpp
  src/version.cpp
)
add_library(vehicle_sil::ecu ALIAS vehicle_sil_ecu)
set_target_properties(vehicle_sil_ecu PROPERTIES EXPORT_NAME ecu)
target_include_directories(vehicle_sil_ecu PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_ecu PUBLIC vehicle_sil::can vehicle_sil::j1939 vehicle_sil::lin vehicle_sil::eth
                                             vehicle_sil::control)
# The JSON writer of to_json(); public: describe_json.hpp takes a json::JsonWriter.
target_link_libraries(vehicle_sil_ecu PUBLIC vehicle_sil::json)
# vehicle_sil::version(): the full version of vehicle-sil (VEHICLE_SIL_VERSION_FULL), also as a subproject.
set_source_files_properties(src/version.cpp PROPERTIES COMPILE_DEFINITIONS VEHICLE_SIL_VERSION="${VEHICLE_SIL_VERSION_FULL}")
vehicle_sil_target_defaults(vehicle_sil_ecu)
