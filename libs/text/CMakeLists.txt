# vehicle_sil::text - how many console columns UTF-8 text takes (vehicle_sil/text/display_width.hpp),
# so that the tables of vehicle_sil_launch and the interface list of the Ethernet bridge pad a
# name in Japanese to the same width. Header only; it uses no other vehicle-sil library and no SIL
# Kit, so every library and tool may link it (also vehicle_sil::eth_link). Internal; not part of
# the library API.
add_library(vehicle_sil_text INTERFACE)
add_library(vehicle_sil::text ALIAS vehicle_sil_text)
set_target_properties(vehicle_sil_text PROPERTIES EXPORT_NAME text)
target_include_directories(vehicle_sil_text INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(vehicle_sil_text INTERFACE cxx_std_20)
