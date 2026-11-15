# vehicle_sil::spy - a part of an ECU's application that hears a J1939 bus through a
# j1939::Listener and publishes what it hears as the ECU's variables (Spy), through watchers the
# user writes against spy::Scope. Built on the public API of vehicle_sil::j1939 and
# vehicle_sil::ecu only (j1939_spy.boundary). No SIL Kit.
add_library(vehicle_sil_spy STATIC src/spy.cpp)
add_library(vehicle_sil::spy ALIAS vehicle_sil_spy)
set_target_properties(vehicle_sil_spy PROPERTIES EXPORT_NAME spy)
target_include_directories(vehicle_sil_spy PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_spy PUBLIC vehicle_sil::ecu vehicle_sil::j1939)
vehicle_sil_target_defaults(vehicle_sil_spy)
