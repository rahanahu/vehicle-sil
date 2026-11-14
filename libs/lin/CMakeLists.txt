# vehicle_sil::lin - LIN master and slave in virtual time: schedule tables, frame transmission
# and reception, diagnostic frames, sleep and wakeup, on a LinPort; and the virtual LIN bus of
# this process. Must build without SIL Kit: link only vehicle_sil::sim_base (the SIL Kit port is
# vehicle_sil::lin_silkit). Does not depend on CAN or J1939.
add_library(vehicle_sil_lin STATIC
  src/lin_master.cpp
  src/lin_schedule.cpp
  src/lin_slave.cpp
  src/virtual_lin_bus.cpp
)
add_library(vehicle_sil::lin ALIAS vehicle_sil_lin)
set_target_properties(vehicle_sil_lin PROPERTIES EXPORT_NAME lin)
target_include_directories(vehicle_sil_lin PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_lin PUBLIC vehicle_sil::sim_base)
vehicle_sil_target_defaults(vehicle_sil_lin)
