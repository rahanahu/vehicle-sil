# vehicle_sil::j1939_broadcast_suppression - protocol part of the J1939 stack: the broadcast
# suppression of a node (DM13). Uses the public API and the toolbox of vehicle_sil::j1939
# only; parts that broadcast on their own read its state.
add_library(vehicle_sil_j1939_broadcast_suppression STATIC src/broadcast_suppression.cpp)
add_library(vehicle_sil::j1939_broadcast_suppression ALIAS vehicle_sil_j1939_broadcast_suppression)
set_target_properties(vehicle_sil_j1939_broadcast_suppression PROPERTIES EXPORT_NAME j1939_broadcast_suppression)
target_include_directories(vehicle_sil_j1939_broadcast_suppression PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_j1939_broadcast_suppression PUBLIC vehicle_sil::j1939)
vehicle_sil_target_defaults(vehicle_sil_j1939_broadcast_suppression)
