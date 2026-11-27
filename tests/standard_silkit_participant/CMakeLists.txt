# Tests for the standalone standard SIL Kit participant sample
# (examples/standard_silkit_participant):
# - boundary check confirming zero vehicle_sil/ header includes in the sample;
# - deterministic unit tests for DeterministicRxInbox (strict-before deferral without dropping
#   equal-timestamp frames and canonical ordering);
# - asymmetric-period bidirectional CAN communication and causality with --speed max;
# - deterministic simultaneous-timestamp handling (--speed max vs --speed 1 trace identity);
# - cross-run isolation across restarted runs;
# - smoke test of the shipped examples/standard_silkit_participant/simulation.yaml.
if(NOT TARGET vehicle_sil_launch OR NOT TARGET standard_silkit_participant OR NOT TARGET vehicle_sil::simulation)
  return()
endif()

add_test(NAME standard_silkit_participant.no_vehicle_sil_includes
         COMMAND ${CMAKE_COMMAND}
                 -DROOT=${PROJECT_SOURCE_DIR}
                 -DDIR=examples/standard_silkit_participant
                 -DFORBIDDEN_PREFIXES=vehicle_sil/
                 -DONLY_PREFIXES=ON
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckIncludes.cmake)
set_tests_properties(standard_silkit_participant.no_vehicle_sil_includes PROPERTIES
                     LABELS "unit;launch" TIMEOUT 60)

vehicle_sil_add_harness_tests(vehicle_sil_std_peer_inbox_tests
  PREFIX standard_silkit_participant LABELS unit launch TIMEOUT 60
  SOURCES
    test_rx_inbox.cpp)
target_include_directories(vehicle_sil_std_peer_inbox_tests PRIVATE
                           ${PROJECT_SOURCE_DIR}/examples/standard_silkit_participant)

add_executable(vehicle_sil_std_peer_test_ecu test_can_ecu.cpp)
target_link_libraries(vehicle_sil_std_peer_test_ecu PRIVATE vehicle_sil::simulation vehicle_sil::process vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_std_peer_test_ecu)
vehicle_sil_copy_runtime_dlls(vehicle_sil_std_peer_test_ecu)

add_executable(vehicle_sil_std_peer_launcher launcher.cpp)
target_link_libraries(vehicle_sil_std_peer_launcher PRIVATE vehicle_sil_test_process)
vehicle_sil_target_defaults(vehicle_sil_std_peer_launcher)
add_dependencies(vehicle_sil_std_peer_launcher
                 vehicle_sil_launch
                 standard_silkit_participant
                 vehicle_sil_std_peer_test_ecu)
if(TARGET vehicle_sil_engine_ecu)
  add_dependencies(vehicle_sil_std_peer_launcher vehicle_sil_engine_ecu)
endif()

set(_std_peer_scenarios
  asymmetric_bidirectional_max
  cross_run_isolation
  simultaneous_timestamp_determinism)
if(TARGET vehicle_sil_engine_ecu)
  list(APPEND _std_peer_scenarios example_config_smoke)
endif()
list(SORT _std_peer_scenarios)

vehicle_sil_add_scenario_tests(vehicle_sil_std_peer_launcher
  PREFIX standard_silkit_participant LABELS integration launch TIMEOUT 180
  WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/work
  SCENARIOS ${_std_peer_scenarios}
  ARGS --launch $<TARGET_FILE:vehicle_sil_launch>
       --standard-peer $<TARGET_FILE:standard_silkit_participant>
       --test-ecu $<TARGET_FILE:vehicle_sil_std_peer_test_ecu>
       --example-dir $<TARGET_FILE_DIR:standard_silkit_participant>)
