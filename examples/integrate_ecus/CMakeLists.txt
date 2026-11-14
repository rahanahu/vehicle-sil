# The example of the guide docs/howto/integrate-ecus.md: the engine ECU of examples/ecu_main and a
# dashboard ECU that names the bus differently (dashboard_v1), with the three simulation
# configurations the guide goes through. Run them where CMake puts them
# (build/<preset>/examples/integrate_ecus/): their relative paths find the ECUs from there.
# The test launch_docs.integrate_ecus compares the files and the launcher's outputs with the guide.
add_executable(vehicle_sil_dashboard_v1_ecu dashboard_v1/main.cpp)
set_target_properties(vehicle_sil_dashboard_v1_ecu PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/dashboard_v1)
target_include_directories(vehicle_sil_dashboard_v1_ecu PRIVATE ../ecu_main)
target_link_libraries(vehicle_sil_dashboard_v1_ecu PRIVATE vehicle_sil::simulation)
vehicle_sil_target_defaults(vehicle_sil_dashboard_v1_ecu)
vehicle_sil_copy_runtime_dlls(vehicle_sil_dashboard_v1_ecu)
# simulation.yaml: as first written (check finds the two names). simulation.two-buses.yaml: the name
# added to networks (check passes, and nothing reaches the dashboard). simulation.fixed.yaml: with the
# dashboard ECU built again under the agreed name.
foreach(file simulation.yaml simulation.two-buses.yaml simulation.fixed.yaml scenarios/engine_speed.yaml)
  configure_file(${file} ${file} COPYONLY)
endforeach()
