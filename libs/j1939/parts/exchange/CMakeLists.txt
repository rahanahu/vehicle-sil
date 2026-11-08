# vehicle_sil::j1939_exchange - protocol part of the J1939 stack for protocols of one's own: sends
# a command to one control function and waits for its reply (timeout, retries, interim replies,
# cancellation, the node's and the counterpart's addresses). Uses the public API and the toolbox
# of vehicle_sil::j1939 only.
add_library(vehicle_sil_j1939_exchange STATIC src/exchange.cpp)
add_library(vehicle_sil::j1939_exchange ALIAS vehicle_sil_j1939_exchange)
set_target_properties(vehicle_sil_j1939_exchange PROPERTIES EXPORT_NAME j1939_exchange)
target_include_directories(vehicle_sil_j1939_exchange PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_j1939_exchange PUBLIC vehicle_sil::j1939)
vehicle_sil_target_defaults(vehicle_sil_j1939_exchange)
