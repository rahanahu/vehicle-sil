# The layers under Simulation, assembled by hand: bus, port, host, driver, J1939 network.
add_executable(vehicle_sil_low_level main.cpp)
target_link_libraries(vehicle_sil_low_level PRIVATE vehicle_sil::j1939 vehicle_sil::can)
vehicle_sil_target_defaults(vehicle_sil_low_level)
vehicle_sil_copy_runtime_dlls(vehicle_sil_low_level)
