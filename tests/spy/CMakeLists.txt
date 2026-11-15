# vehicle_sil::spy on a LocalBench (public API only), its allocation in the steps (a binary of its
# own: it replaces the global operator new) and its boundary. The prefix j1939_spy puts the tests
# among those of the J1939 stack ('^j1939').
vehicle_sil_add_harness_tests(vehicle_sil_spy_tests
  PREFIX j1939_spy LABELS unit j1939 TIMEOUT 60
  SOURCES test_spy.cpp
  LINK vehicle_sil::spy vehicle_sil::local vehicle_sil::simulation)
target_include_directories(vehicle_sil_spy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/j1939)

# Replaces the global operator new to count allocations in steady-state steps: its own binary.
vehicle_sil_add_harness_tests(vehicle_sil_spy_allocation_tests
  PREFIX j1939_spy LABELS unit j1939 TIMEOUT 60
  SOURCES test_spy_allocation.cpp
  LINK vehicle_sil::spy vehicle_sil::local)
target_include_directories(vehicle_sil_spy_allocation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                           ${PROJECT_SOURCE_DIR}/tests/j1939)

# libs/spy uses the public headers of the engine and of Ecu only, like the parts.
add_test(NAME j1939_spy.boundary
         COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${PROJECT_SOURCE_DIR}/libs/spy -DSOURCES_ONLY=ON
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
set_tests_properties(j1939_spy.boundary PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
