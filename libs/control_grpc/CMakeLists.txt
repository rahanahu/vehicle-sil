# vehicle_sil::control_grpc - the Control API over gRPC: the control server (a Hub with the
# Control API v1 services and the time gate of a SimulationController) and the agent's client.
# Only with VEHICLE_SIL_BUILD_CONTROL_SERVER (needs gRPC).
add_library(vehicle_sil_control_grpc STATIC
  src/server.cpp
  src/agent_client.cpp
  src/simulation_monitor.cpp)
add_library(vehicle_sil::control_grpc ALIAS vehicle_sil_control_grpc)
set_target_properties(vehicle_sil_control_grpc PROPERTIES EXPORT_NAME control_grpc)
target_include_directories(vehicle_sil_control_grpc PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_control_grpc PUBLIC vehicle_sil::control vehicle_sil::core
                                               PRIVATE vehicle_sil::control_proto)
vehicle_sil_target_defaults(vehicle_sil_control_grpc)
