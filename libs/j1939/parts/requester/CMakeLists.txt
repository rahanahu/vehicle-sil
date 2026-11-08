# vehicle_sil::j1939_requester - protocol part of the J1939 stack: sends Requests and waits for
# the answer (response, acknowledgement, timeout), with retries. Uses the public API and the
# toolbox of vehicle_sil::j1939 only.
add_library(vehicle_sil_j1939_requester STATIC src/requester.cpp)
add_library(vehicle_sil::j1939_requester ALIAS vehicle_sil_j1939_requester)
set_target_properties(vehicle_sil_j1939_requester PROPERTIES EXPORT_NAME j1939_requester)
target_include_directories(vehicle_sil_j1939_requester PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_j1939_requester PUBLIC vehicle_sil::j1939)
vehicle_sil_target_defaults(vehicle_sil_j1939_requester)
