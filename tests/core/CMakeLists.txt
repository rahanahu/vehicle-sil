# Core: simulation time helpers and SIL Kit virtual time (in-process registry). The virtual
# time cases measure wall-clock speed and run alone.
vehicle_sil_add_harness_tests(vehicle_sil_core_tests
  PREFIX core LABELS unit core TIMEOUT 120 RUN_SERIAL "^virtual_time_"
  SOURCES
    test_communication_ready_gate.cpp
    test_deadline_waiter.cpp
    test_failure_check.cpp
    test_participant_configuration.cpp
    test_participant_start.cpp
    test_simulation_watch.cpp
    test_start_window.cpp
    test_time_gate.cpp
    test_time_gate_model.cpp
    test_time_helpers.cpp
    test_virtual_time.cpp
  LINK vehicle_sil::core)
if(WIN32)
  # GetExtendedTcpTable(): where the test's participants listen.
  target_link_libraries(vehicle_sil_core_tests PRIVATE iphlpapi ws2_32)
endif()
# The gate's arithmetic and its timer are internal to the library.
target_include_directories(vehicle_sil_core_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/core/src)

# Every SIL Kit participant configuration that the product and its tests make from a text goes
# through vehicle-sil's defaults (loopback only for a loopback registry; TCP only on Windows); the
# check itself must reject a text given directly.
add_test(NAME core.silkit_configuration_defaults
         COMMAND ${CMAKE_COMMAND}
                 -DDIRS=${PROJECT_SOURCE_DIR}/libs,${PROJECT_SOURCE_DIR}/tools,${PROJECT_SOURCE_DIR}/bridges,${PROJECT_SOURCE_DIR}/tests
                 -DEXCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/configuration_fixtures
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckSilKitConfiguration.cmake)
add_test(NAME core.silkit_configuration_check_rejects_a_direct_text
         COMMAND ${CMAKE_COMMAND} -DDIRS=${CMAKE_CURRENT_SOURCE_DIR}/configuration_fixtures/direct
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckSilKitConfiguration.cmake)
set_tests_properties(core.silkit_configuration_defaults core.silkit_configuration_check_rejects_a_direct_text
                     PROPERTIES LABELS "unit;core" TIMEOUT 60)
set_tests_properties(core.silkit_configuration_check_rejects_a_direct_text PROPERTIES
                     PASS_REGULAR_EXPRESSION "fixture\\.cpp: ParticipantConfigurationFromString")
