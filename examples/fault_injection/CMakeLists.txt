# Fault injection and observation on the virtual buses: four scenes, each checked by what the
# stack reports (Monitor, address state, Observer events, the frames on the bus).
add_executable(vehicle_sil_fault_injection main.cpp)
target_link_libraries(vehicle_sil_fault_injection PRIVATE vehicle_sil::simulation vehicle_sil::testkit vehicle_sil::j1939_monitor
                      vehicle_sil::j1939_periodic_sender)
vehicle_sil_target_defaults(vehicle_sil_fault_injection)
vehicle_sil_copy_runtime_dlls(vehicle_sil_fault_injection)
