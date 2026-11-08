# vehicle_sil::j1939_periodic_sender - protocol part of the J1939 stack: sends one parameter group
# of a node periodically (follows the node's address and DM13). Uses the public API and the
# toolbox of vehicle_sil::j1939 only.
add_library(vehicle_sil_j1939_periodic_sender STATIC src/periodic_sender.cpp)
add_library(vehicle_sil::j1939_periodic_sender ALIAS vehicle_sil_j1939_periodic_sender)
set_target_properties(vehicle_sil_j1939_periodic_sender PROPERTIES EXPORT_NAME j1939_periodic_sender)
target_include_directories(vehicle_sil_j1939_periodic_sender PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_j1939_periodic_sender PUBLIC vehicle_sil::j1939_broadcast_suppression)
vehicle_sil_target_defaults(vehicle_sil_j1939_periodic_sender)
