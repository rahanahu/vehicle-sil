# J1939 integration: real sil-kit-registry, sender and receiver processes. Added only with
# VEHICLE_SIL_BUILD_EXAMPLES (the launcher runs the example peers).
add_executable(vehicle_sil_it_launcher launcher.cpp)
target_link_libraries(vehicle_sil_it_launcher PRIVATE vehicle_sil_test_process)
vehicle_sil_target_defaults(vehicle_sil_it_launcher)
# The launcher starts these executables, so they must be built with it.
add_dependencies(vehicle_sil_it_launcher sil-kit-registry vehicle_sil_j1939_sender vehicle_sil_j1939_receiver)

vehicle_sil_add_scenario_tests(vehicle_sil_it_launcher
  PREFIX integration LABELS integration TIMEOUT 120
  WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}
  SCENARIOS address-claim-and-pgn receiver-timeout receiver-detects-wrong-name receiver-detects-wrong-payload
  ARGS --registry $<TARGET_FILE:sil-kit-registry>
       --sender $<TARGET_FILE:vehicle_sil_j1939_sender>
       --receiver $<TARGET_FILE:vehicle_sil_j1939_receiver>
       --timeout-ms 60000)
