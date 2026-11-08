# vehicle_sil::j1939_address_commander - protocol part of the J1939 stack: sends Commanded Address
# and waits until the target claimed the new address. Uses the public API and the toolbox of
# vehicle_sil::j1939 only.
add_library(vehicle_sil_j1939_address_commander STATIC src/address_commander.cpp)
add_library(vehicle_sil::j1939_address_commander ALIAS vehicle_sil_j1939_address_commander)
set_target_properties(vehicle_sil_j1939_address_commander PROPERTIES EXPORT_NAME j1939_address_commander)
target_include_directories(vehicle_sil_j1939_address_commander PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_j1939_address_commander PUBLIC vehicle_sil::j1939)
vehicle_sil_target_defaults(vehicle_sil_j1939_address_commander)
