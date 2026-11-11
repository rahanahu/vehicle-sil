# vehicle_sil::can (base of the J1939 stack): frame values, virtual bus, host and step
# driver. Links only vehicle_sil::can, so building it checks that the library needs no SIL Kit.
vehicle_sil_add_harness_tests(vehicle_sil_can_tests
  PREFIX can LABELS unit can TIMEOUT 60
  SOURCES test_can_values_and_bus.cpp test_can_host.cpp test_can_bus_model.cpp
          test_can_real_time.cpp test_can_faults.cpp test_can_tap_port.cpp
          test_can_raw_channel.cpp
  LINK vehicle_sil::can)
target_include_directories(vehicle_sil_can_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The SIL Kit port of vehicle_sil::can: against a fake controller, and on real participants
# (in-process registry), where the traces must equal the virtual bus's.
vehicle_sil_add_harness_tests(vehicle_sil_can_silkit_tests
  PREFIX can_silkit LABELS unit can TIMEOUT 120
  SOURCES test_silkit_can_port.cpp test_can_silkit_runs.cpp
  LINK vehicle_sil::can_silkit)
target_include_directories(vehicle_sil_can_silkit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The simple CAN network simulator (vehicle_sil::can_netsim) on real participants (in-process
# registry), compared with the virtual bus; its configuration file; and the executable.
if(TARGET vehicle_sil_can_bus_simulator)
  vehicle_sil_add_harness_tests(vehicle_sil_can_netsim_tests
    PREFIX can_netsim LABELS unit can TIMEOUT 120
    SOURCES test_can_netsim.cpp
    LINK vehicle_sil::can_netsim vehicle_sil::can_silkit vehicle_sil::process)
  target_include_directories(vehicle_sil_can_netsim_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(vehicle_sil_can_netsim_tests
    PRIVATE VEHICLE_SIL_CAN_BUS_SIMULATOR="$<TARGET_FILE:vehicle_sil_can_bus_simulator>")
  add_dependencies(vehicle_sil_can_netsim_tests vehicle_sil_can_bus_simulator)
endif()
