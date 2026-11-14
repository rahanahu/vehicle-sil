# vehicle_sil::step - what steps the protocol processing of one participant, for every bus: the
# time domain of a port, the members a host steps (Member), the host (Host), the virtual-time
# step driver (StepDriver), the real-time driver (RealTimeDriver) and its clocks. Protocol
# independent (vehicle_sil::can builds its Host on it).
# Must build without SIL Kit: link only vehicle_sil::sim_base.
add_library(vehicle_sil_step STATIC
  src/host.cpp
  src/real_time_clock.cpp
  src/real_time_driver.cpp
  src/step_driver.cpp)
add_library(vehicle_sil::step ALIAS vehicle_sil_step)
set_target_properties(vehicle_sil_step PROPERTIES EXPORT_NAME step)
target_include_directories(vehicle_sil_step PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_step PUBLIC vehicle_sil::sim_base)
vehicle_sil_target_defaults(vehicle_sil_step)
