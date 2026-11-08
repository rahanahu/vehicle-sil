# vehicle_sil::j1939_diagnostics - protocol parts of the J1939 stack: DTCs (DM1, DM2, DM3,
# DM11, DM22) and identification. Uses the public API and the toolbox of
# vehicle_sil::j1939 only, and the broadcast suppression part (DM13).
add_library(vehicle_sil_j1939_diagnostics STATIC src/diagnostics.cpp src/identification.cpp)
add_library(vehicle_sil::j1939_diagnostics ALIAS vehicle_sil_j1939_diagnostics)
set_target_properties(vehicle_sil_j1939_diagnostics PROPERTIES EXPORT_NAME j1939_diagnostics)
target_include_directories(vehicle_sil_j1939_diagnostics PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_j1939_diagnostics PUBLIC vehicle_sil::j1939 vehicle_sil::j1939_broadcast_suppression)
vehicle_sil_target_defaults(vehicle_sil_j1939_diagnostics)
