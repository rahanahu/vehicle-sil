# vehicle_sil::can - backend-neutral CAN: frame values, the port contract, the host that steps
# protocol stacks, raw frames of an application (RawChannel), the virtual-time step driver, the
# virtual bus and the real-time driver.
# Must build without SIL Kit and Vector XL: link only vehicle_sil::sim_base.
add_library(vehicle_sil_can STATIC
  src/bus_model.cpp
  src/host.cpp
  src/raw_channel.cpp
  src/real_time.cpp
  src/step_driver.cpp
  src/tap_port.cpp
  src/virtual_can_bus.cpp)
add_library(vehicle_sil::can ALIAS vehicle_sil_can)
set_target_properties(vehicle_sil_can PROPERTIES EXPORT_NAME can)
target_include_directories(vehicle_sil_can PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_can PUBLIC vehicle_sil::sim_base)
vehicle_sil_target_defaults(vehicle_sil_can)
