# A simulation for Control API clients (Python, CLI): two J1939 peers with agents, the control
# server and the time gate in one process.
add_executable(vehicle_sil_control_demo control_demo.cpp)
target_link_libraries(vehicle_sil_control_demo PRIVATE vehicle_sil::control_grpc vehicle_sil::peer vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_control_demo)
vehicle_sil_copy_runtime_dlls(vehicle_sil_control_demo)

# One peer with a control agent per process (also on another PC).
add_executable(vehicle_sil_control_peer control_peer.cpp)
target_link_libraries(vehicle_sil_control_peer PRIVATE vehicle_sil::control_grpc vehicle_sil::peer vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_control_peer)
vehicle_sil_copy_runtime_dlls(vehicle_sil_control_peer)
