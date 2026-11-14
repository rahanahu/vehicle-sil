# Two ECUs that exchange raw Ethernet frames (vehicle_sil::Ecu::ethernet()).
# - vehicle_sil_ethernet_example: both in one process, on the local virtual network or on SIL Kit
#   (--silkit); it checks its own result.
# - vehicle_sil_eth_sensor_ecu, vehicle_sil_eth_display_ecu: each an executable made with
#   vehicle_sil::ecu_main(), for vehicle_sil_launch (simulation.yaml, scenarios/exchange.yaml).
add_executable(vehicle_sil_ethernet_example main.cpp ethernet_ecus.hpp)
target_link_libraries(vehicle_sil_ethernet_example PRIVATE vehicle_sil::simulation)
vehicle_sil_target_defaults(vehicle_sil_ethernet_example)
vehicle_sil_copy_runtime_dlls(vehicle_sil_ethernet_example)

foreach(ecu IN ITEMS sensor display)
  add_executable(vehicle_sil_eth_${ecu}_ecu ${ecu}/main.cpp ethernet_ecus.hpp)
  target_include_directories(vehicle_sil_eth_${ecu}_ecu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(vehicle_sil_eth_${ecu}_ecu PRIVATE vehicle_sil::simulation)
  # Next to simulation.yaml in a directory of its own, as its commands say.
  set_target_properties(vehicle_sil_eth_${ecu}_ecu PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                              ${CMAKE_CURRENT_BINARY_DIR}/${ecu})
  vehicle_sil_target_defaults(vehicle_sil_eth_${ecu}_ecu)
  vehicle_sil_copy_runtime_dlls(vehicle_sil_eth_${ecu}_ecu)
endforeach()
configure_file(simulation.yaml simulation.yaml COPYONLY)
configure_file(scenarios/exchange.yaml scenarios/exchange.yaml COPYONLY)
