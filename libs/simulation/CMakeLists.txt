# vehicle_sil::simulation - runs ECUs written against vehicle_sil::Ecu on local virtual CAN
# buses (vehicle_sil::local) or as SIL Kit participants of this process (vehicle_sil::peer,
# with a registry of this process if none is given), in virtual time, and reports a failed ECU
# as an exception; ecu_main(), the main() of an executable that is one ECU. Internal for now: the
# API may change.
add_library(vehicle_sil_simulation STATIC
  src/ecu_main.cpp
  src/simulation.cpp
)
add_library(vehicle_sil::simulation ALIAS vehicle_sil_simulation)
set_target_properties(vehicle_sil_simulation PROPERTIES EXPORT_NAME simulation)
target_include_directories(vehicle_sil_simulation PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_simulation PUBLIC vehicle_sil::ecu vehicle_sil::peer vehicle_sil::local)
# ecu_main() offers the ECU's variables to the simulation over SIL Kit (no gRPC in an ECU).
target_link_libraries(vehicle_sil_simulation PRIVATE vehicle_sil::control_silkit)
if(WIN32)
  # CommandLineToArgvW: ecu_main() reads --map-network from the wide command line.
  target_link_libraries(vehicle_sil_simulation PRIVATE shell32)
endif()
vehicle_sil_target_defaults(vehicle_sil_simulation)
