# A small vehicle: ECUs on two CAN networks and a LIN network, one Simulation.
add_executable(vehicle_sil_vehicle main.cpp)
target_link_libraries(vehicle_sil_vehicle PRIVATE vehicle_sil::simulation vehicle_sil::j1939_periodic_sender)
vehicle_sil_target_defaults(vehicle_sil_vehicle)
vehicle_sil_copy_runtime_dlls(vehicle_sil_vehicle)
