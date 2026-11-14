# vehicle_sil::local - ECUs (vehicle_sil::Ecu) on virtual CAN and LIN buses in this process, each
# on a host of its own, stepped on virtual time in the calling thread (LocalBench). Uses only the
# public API of vehicle_sil::can, vehicle_sil::j1939, vehicle_sil::lin and vehicle_sil::ecu; no SIL
# Kit. Internal for now: the API may change.
add_library(vehicle_sil_local STATIC src/local_bench.cpp)
add_library(vehicle_sil::local ALIAS vehicle_sil_local)
set_target_properties(vehicle_sil_local PROPERTIES EXPORT_NAME local)
target_include_directories(vehicle_sil_local PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_local PUBLIC vehicle_sil::can vehicle_sil::j1939 vehicle_sil::lin vehicle_sil::eth
                                               vehicle_sil::ecu)
vehicle_sil_target_defaults(vehicle_sil_local)
