# vehicle_sil::j1939_monitor - protocol part of the J1939 stack: the latest message of one
# parameter group and its timeout (loss of the sender). Uses the public API and the toolbox of
# vehicle_sil::j1939 only.
add_library(vehicle_sil_j1939_monitor STATIC src/monitor.cpp)
add_library(vehicle_sil::j1939_monitor ALIAS vehicle_sil_j1939_monitor)
set_target_properties(vehicle_sil_j1939_monitor PROPERTIES EXPORT_NAME j1939_monitor)
target_include_directories(vehicle_sil_j1939_monitor PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_j1939_monitor PUBLIC vehicle_sil::j1939)
vehicle_sil_target_defaults(vehicle_sil_j1939_monitor)
