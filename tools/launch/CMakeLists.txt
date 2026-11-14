# vehicle_sil_launch: checks a simulation configuration against the ecu_main executables in it
# (vehicle_sil_launch check <file>) and runs it (run <file>). Needs vehicle_sil::launch (rapidyaml).
if(NOT TARGET vehicle_sil::launch)
  return()
endif()
add_executable(vehicle_sil_launch main.cpp)
target_link_libraries(vehicle_sil_launch PRIVATE vehicle_sil::launch vehicle_sil::process vehicle_sil_cli)
# run --control: the Control Server (gRPC) on the run's hub, only with VEHICLE_SIL_BUILD_CONTROL_SERVER.
if(TARGET vehicle_sil::control_grpc)
  target_sources(vehicle_sil_launch PRIVATE control_server.cpp)
  target_link_libraries(vehicle_sil_launch PRIVATE vehicle_sil::control_grpc vehicle_sil::core)
  target_compile_definitions(vehicle_sil_launch PRIVATE VEHICLE_SIL_LAUNCH_CONTROL_SERVER)
endif()
vehicle_sil_target_defaults(vehicle_sil_launch)
vehicle_sil_copy_runtime_dlls(vehicle_sil_launch)
