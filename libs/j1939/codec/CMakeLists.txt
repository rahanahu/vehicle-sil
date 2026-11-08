# vehicle_sil::j1939_codec - typed J1939 messages (header only): codec.hpp turns signal values into
# bytes and back and knows no node; codec_node.hpp sends and receives them on a Node and with a
# PeriodicSender. The message types are generated from YAML by vehicle_sil_add_j1939_codec()
# (cmake/VehicleSilJ1939Codec.cmake).
add_library(vehicle_sil_j1939_codec INTERFACE)
add_library(vehicle_sil::j1939_codec ALIAS vehicle_sil_j1939_codec)
set_target_properties(vehicle_sil_j1939_codec PROPERTIES EXPORT_NAME j1939_codec)
target_include_directories(vehicle_sil_j1939_codec INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_j1939_codec INTERFACE vehicle_sil::j1939 vehicle_sil::j1939_periodic_sender)
target_compile_features(vehicle_sil_j1939_codec INTERFACE cxx_std_20)
