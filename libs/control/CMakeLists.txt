# vehicle_sil::control - the control agent of an ECU: typed variables, timed writes applied
# inside the simulation steps, sampled values and events, through bounded queues; and the
# control server's core (Hub).
# No transport and no SIL Kit: the ECU that owns it (vehicle_sil::Ecu, Peer, LocalBench) drives
# it from its steps.
add_library(vehicle_sil_control STATIC
  src/agent.cpp
  src/hub.cpp
  src/wire.cpp)
add_library(vehicle_sil::control ALIAS vehicle_sil_control)
set_target_properties(vehicle_sil_control PROPERTIES EXPORT_NAME control)
target_include_directories(vehicle_sil_control PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_control PUBLIC vehicle_sil::sim_base)
vehicle_sil_target_defaults(vehicle_sil_control)
