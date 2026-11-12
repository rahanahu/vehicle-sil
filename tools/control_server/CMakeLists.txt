# vehicle_sil_control_server: the Control API server of a SIL Kit simulation that
# vehicle_sil_launch does not run; it reaches the ECUs' control agents over SIL Kit and is,
# optionally, the system controller with its time gate. Only with VEHICLE_SIL_BUILD_CONTROL_SERVER.
add_executable(vehicle_sil_control_server main.cpp)
target_link_libraries(vehicle_sil_control_server PRIVATE vehicle_sil::control_grpc vehicle_sil::control_silkit
                                                         vehicle_sil::core vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_control_server)
vehicle_sil_copy_runtime_dlls(vehicle_sil_control_server)
