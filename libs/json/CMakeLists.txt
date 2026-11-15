# vehicle_sil::json - the JSON writer that the output of an ECU's --describe and the JSON of
# vehicle_sil_launch share (vehicle_sil/json/json_writer.hpp). Header only; it uses no other
# vehicle-sil library and no SIL Kit, so every library and tool may link it. Internal; not part of
# the library API.
add_library(vehicle_sil_json INTERFACE)
add_library(vehicle_sil::json ALIAS vehicle_sil_json)
set_target_properties(vehicle_sil_json PROPERTIES EXPORT_NAME json)
target_include_directories(vehicle_sil_json INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(vehicle_sil_json INTERFACE cxx_std_20)
