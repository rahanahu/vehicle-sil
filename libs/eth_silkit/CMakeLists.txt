# vehicle_sil::eth_silkit - vehicle_sil::eth port on a SIL Kit Ethernet controller (virtual time).
add_library(vehicle_sil_eth_silkit STATIC src/silkit_eth_port.cpp)
add_library(vehicle_sil::eth_silkit ALIAS vehicle_sil_eth_silkit)
set_target_properties(vehicle_sil_eth_silkit PROPERTIES EXPORT_NAME eth_silkit)
target_include_directories(vehicle_sil_eth_silkit PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_eth_silkit PUBLIC vehicle_sil::eth SilKit::SilKit)
vehicle_sil_target_defaults(vehicle_sil_eth_silkit)
