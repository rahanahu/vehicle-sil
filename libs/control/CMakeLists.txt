# vehicle_sil::control - the control agent of a participant: typed variables, timed writes
# applied inside the simulation steps, sampled values and events, through bounded queues.
# No transport and no SIL Kit: a participant (Peer, ParticipantRuntime) drives it from its steps.
add_library(vehicle_sil_control STATIC
  src/agent.cpp)
add_library(vehicle_sil::control ALIAS vehicle_sil_control)
set_target_properties(vehicle_sil_control PROPERTIES EXPORT_NAME control)
target_include_directories(vehicle_sil_control PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_control PUBLIC vehicle_sil::sim_base)
vehicle_sil_target_defaults(vehicle_sil_control)

# vehicle_sil::peer_control - drives a control agent from the steps of a vehicle_sil::Peer.
add_library(vehicle_sil_peer_control STATIC src/peer_agent.cpp)
add_library(vehicle_sil::peer_control ALIAS vehicle_sil_peer_control)
set_target_properties(vehicle_sil_peer_control PROPERTIES EXPORT_NAME peer_control)
target_link_libraries(vehicle_sil_peer_control PUBLIC vehicle_sil::control vehicle_sil::peer)
vehicle_sil_target_defaults(vehicle_sil_peer_control)
