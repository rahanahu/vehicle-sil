# What an ECU reports about itself in JSON (to_json(EcuDescription)), without SIL Kit.
vehicle_sil_add_harness_tests(vehicle_sil_ecu_tests
  PREFIX ecu LABELS unit ecu TIMEOUT 60
  SOURCES test_describe_json.cpp
  LINK vehicle_sil::ecu)
target_compile_definitions(vehicle_sil_ecu_tests PRIVATE VEHICLE_SIL_TEST_PROJECT_VERSION="${vehicle_sil_VERSION}")

# vehicle_sil::ecu, and the headers of vehicle_sil::simulation (simulation.hpp, ecu_main.hpp),
# include no SIL Kit header, also through the vehicle-sil headers they include.
add_test(NAME ecu.no_silkit_boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/ecu
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
add_test(NAME ecu.simulation_headers_boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/simulation/include
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
# The check follows the vehicle-sil headers: one that includes vehicle_sil/peer/peer.hpp is
# rejected, with the chain of includes to SIL Kit.
add_test(NAME ecu.boundary_check_rejects_a_peer_include
         COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/boundary_fixtures/peer_include
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
# ... and a vehicle_sil/ include it cannot find (it could hide SIL Kit) unless it is allowed.
add_test(NAME ecu.boundary_check_rejects_an_unresolved_include
         COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/boundary_fixtures/unresolved
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
add_test(NAME ecu.boundary_check_allows_a_listed_unresolved_include
         COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/boundary_fixtures/unresolved
                 -DALLOW_UNRESOLVED=vehicle_sil/nowhere/missing.hpp
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
set_tests_properties(ecu.no_silkit_boundary ecu.simulation_headers_boundary ecu.boundary_check_rejects_a_peer_include
                     ecu.boundary_check_rejects_an_unresolved_include ecu.boundary_check_allows_a_listed_unresolved_include
                     PROPERTIES LABELS "unit;ecu" TIMEOUT 60)
set_tests_properties(ecu.boundary_check_rejects_an_unresolved_include PROPERTIES
                     PASS_REGULAR_EXPRESSION "fixture\\.hpp: vehicle_sil/nowhere/missing\\.hpp \\(not found")
# CMake wraps the first line of the message; match the indented line that names the chain.
set_tests_properties(ecu.boundary_check_rejects_a_peer_include PROPERTIES
                     PASS_REGULAR_EXPRESSION "fixture\\.hpp: vehicle_sil/peer/peer\\.hpp -> [^\n]*silkit")
