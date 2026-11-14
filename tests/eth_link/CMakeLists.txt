# vehicle_sil::eth_link alone (no SIL Kit): MAC addresses, the frame filter, the MAC side table,
# and the fake link the Ethernet bridge tests use.

# TEST ONLY: an in-memory ExternalLink with a receive thread, and the wire between fake links.
add_library(vehicle_sil_eth_link_fake STATIC fake_link/fake_link.cpp)
target_include_directories(vehicle_sil_eth_link_fake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/fake_link)
target_link_libraries(vehicle_sil_eth_link_fake PUBLIC vehicle_sil::eth_link)
vehicle_sil_target_defaults(vehicle_sil_eth_link_fake)

vehicle_sil_add_harness_tests(vehicle_sil_eth_link_tests
  PREFIX eth_link LABELS unit eth_link TIMEOUT 60
  SOURCES test_mac_address.cpp test_frame_filter.cpp test_mac_side_table.cpp test_fake_link.cpp
  LINK vehicle_sil_eth_link_fake)

# vehicle_sil::eth_link includes no SIL Kit header, also through the vehicle-sil headers it
# includes (it links nothing of vehicle-sil).
add_test(NAME eth_link.boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/eth_link
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
set_tests_properties(eth_link.boundary PROPERTIES LABELS "unit;eth_link" TIMEOUT 60)
