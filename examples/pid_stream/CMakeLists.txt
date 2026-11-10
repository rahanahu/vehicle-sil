# A protocol of one's own, as two parts (pid_stream.hpp): on a request, an ECU sends values to the
# requester every period, in a byte layout that can be replaced.
add_executable(vehicle_sil_pid_stream main.cpp pid_stream.hpp)
target_link_libraries(vehicle_sil_pid_stream PRIVATE vehicle_sil::simulation vehicle_sil::j1939_periodic_sender)
vehicle_sil_target_defaults(vehicle_sil_pid_stream)
vehicle_sil_copy_runtime_dlls(vehicle_sil_pid_stream)
