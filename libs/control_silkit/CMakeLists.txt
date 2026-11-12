# vehicle_sil::control_silkit - the control agent's messages (vehicle_sil::control, wire.hpp)
# carried over SIL Kit's publish/subscribe: AgentTransport on the ECU's participant, HubTransport
# on the participant of whoever holds the hub (the controller of vehicle_sil_launch). No gRPC and
# no protobuf: an ECU executable links only this and SIL Kit.
add_library(vehicle_sil_control_silkit STATIC
  src/agent_transport.cpp
  src/hub_transport.cpp)
add_library(vehicle_sil::control_silkit ALIAS vehicle_sil_control_silkit)
set_target_properties(vehicle_sil_control_silkit PROPERTIES EXPORT_NAME control_silkit)
target_include_directories(vehicle_sil_control_silkit PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_control_silkit PUBLIC vehicle_sil::control vehicle_sil::core SilKit::SilKit)
vehicle_sil_target_defaults(vehicle_sil_control_silkit)
