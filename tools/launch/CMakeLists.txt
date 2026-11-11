# vehicle_sil_launch: checks a simulation configuration against the ecu_main executables in it
# (vehicle_sil_launch check <file>). Needs vehicle_sil::launch (rapidyaml).
if(NOT TARGET vehicle_sil::launch)
  return()
endif()
add_executable(vehicle_sil_launch main.cpp)
target_link_libraries(vehicle_sil_launch PRIVATE vehicle_sil::launch vehicle_sil::process vehicle_sil_cli)
if(WIN32)
  # CommandLineToArgvW: the command line as UTF-8.
  target_link_libraries(vehicle_sil_launch PRIVATE shell32)
endif()
vehicle_sil_target_defaults(vehicle_sil_launch)
vehicle_sil_copy_runtime_dlls(vehicle_sil_launch)
