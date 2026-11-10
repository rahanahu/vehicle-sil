# The first ECUs: an engine and a display, written against vehicle_sil::Ecu and run by
# vehicle_sil::Simulation on virtual buses or on SIL Kit. Their message is a typed value
# generated from getting_started_messages.yaml.
vehicle_sil_add_j1939_codec(vehicle_sil_getting_started_messages getting_started_messages.yaml)
add_executable(vehicle_sil_getting_started main.cpp)
target_link_libraries(vehicle_sil_getting_started PRIVATE vehicle_sil::simulation vehicle_sil_getting_started_messages)
vehicle_sil_target_defaults(vehicle_sil_getting_started)
vehicle_sil_copy_runtime_dlls(vehicle_sil_getting_started)
