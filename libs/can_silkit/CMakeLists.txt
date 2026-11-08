# vehicle_sil::can_silkit - vehicle_sil::can port on a SIL Kit CAN controller (virtual or real time),
# and the binding of a vehicle_sil::can::Host to a ParticipantRuntime.
add_library(vehicle_sil_can_silkit STATIC src/silkit_can_port.cpp)
add_library(vehicle_sil::can_silkit ALIAS vehicle_sil_can_silkit)
set_target_properties(vehicle_sil_can_silkit PROPERTIES EXPORT_NAME can_silkit)
target_include_directories(vehicle_sil_can_silkit PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_can_silkit PUBLIC vehicle_sil::can vehicle_sil::core SilKit::SilKit)
vehicle_sil_target_defaults(vehicle_sil_can_silkit)
