# vehicle_sil::eth: frame values, the virtual network and the channel as a step::Member on a
# step::Host. Links only vehicle_sil::eth, so building it checks that the library needs no SIL Kit.
vehicle_sil_add_harness_tests(vehicle_sil_eth_tests
  PREFIX eth LABELS unit eth TIMEOUT 60
  SOURCES test_eth_values_and_bus.cpp test_eth_channel.cpp
  LINK vehicle_sil::eth)
target_include_directories(vehicle_sil_eth_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Counts every allocation of the process (unit/allocation_counter.hpp): its own binary.
vehicle_sil_add_harness_tests(vehicle_sil_eth_allocation_tests
  PREFIX eth LABELS unit eth TIMEOUT 60
  SOURCES test_eth_allocation.cpp
  LINK vehicle_sil::eth)
target_include_directories(vehicle_sil_eth_allocation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# vehicle_sil::eth includes no SIL Kit header, also through the vehicle-sil headers it includes.
add_test(NAME eth.boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/eth
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
set_tests_properties(eth.boundary PROPERTIES LABELS "unit;eth" TIMEOUT 60)

# The SIL Kit port of vehicle_sil::eth: against a fake controller, and on real participants
# (in-process registry), where the records must equal the virtual network's.
vehicle_sil_add_harness_tests(vehicle_sil_eth_silkit_tests
  PREFIX eth_silkit LABELS unit eth TIMEOUT 120
  SOURCES test_silkit_eth_port.cpp test_eth_silkit_runs.cpp
  LINK vehicle_sil::eth_silkit vehicle_sil::core)
target_include_directories(vehicle_sil_eth_silkit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
