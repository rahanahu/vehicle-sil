# vehicle_sil::lin - LIN master and slave on SIL Kit LIN in virtual time: schedule tables, frame
# transmission and reception, sleep and wakeup. Depends on core only, not on J1939.
add_library(vehicle_sil_lin STATIC
  src/lin_schedule.cpp
  src/lin_master.cpp
  src/lin_slave.cpp)
add_library(vehicle_sil::lin ALIAS vehicle_sil_lin)
set_target_properties(vehicle_sil_lin PROPERTIES EXPORT_NAME lin)
target_include_directories(vehicle_sil_lin PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_lin PUBLIC vehicle_sil::core SilKit::SilKit)
vehicle_sil_target_defaults(vehicle_sil_lin)
