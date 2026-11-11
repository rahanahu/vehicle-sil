# vehicle_sil::lin_silkit - vehicle_sil::lin port on a SIL Kit LIN controller (virtual time).
add_library(vehicle_sil_lin_silkit STATIC src/silkit_lin_port.cpp)
add_library(vehicle_sil::lin_silkit ALIAS vehicle_sil_lin_silkit)
set_target_properties(vehicle_sil_lin_silkit PROPERTIES EXPORT_NAME lin_silkit)
target_include_directories(vehicle_sil_lin_silkit PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_lin_silkit PUBLIC vehicle_sil::lin vehicle_sil::core SilKit::SilKit)
vehicle_sil_target_defaults(vehicle_sil_lin_silkit)
