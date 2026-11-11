# The engine ECU as an executable: vehicle_sil_engine_ecu --help.
add_executable(vehicle_sil_engine_ecu main.cpp)
target_include_directories(vehicle_sil_engine_ecu PRIVATE ..)
target_link_libraries(vehicle_sil_engine_ecu PRIVATE vehicle_sil::simulation)
vehicle_sil_target_defaults(vehicle_sil_engine_ecu)
vehicle_sil_copy_runtime_dlls(vehicle_sil_engine_ecu)
