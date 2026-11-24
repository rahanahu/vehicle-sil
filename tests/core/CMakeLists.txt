# Core: simulation time helpers and SIL Kit virtual time (in-process registry). Only the cases
# that assert wall-clock pacing, deadline accuracy, or upper bounds on elapsed time run alone.
set(_core_wall_clock_serial_regex
  "^virtual_time_(deadline_waiter_wakes_close_to_the_deadline|different_steps_stay_synchronised_and_run_faster_than_real_time|pacing_runs_at_the_requested_speed|gate_speed_paces_and_changes|gate_speed_change_releases_a_step_held_for_pacing|gate_setting_the_same_speed_does_not_restart_the_wait|gate_targets_in_the_past_and_resume|gate_counts_the_steps_that_fall_behind_the_wall_clock|gate_does_not_count_the_time_it_stood_still_as_late|gate_a_speed_set_before_the_start_paces_from_the_first_step|gate_measures_the_step_at_stop_at|gate_counts_a_stall_while_it_lasts_and_when_the_run_is_stopped_in_it)$")
vehicle_sil_add_harness_tests(vehicle_sil_core_tests
  PREFIX core LABELS unit core TIMEOUT 120 RUN_SERIAL "${_core_wall_clock_serial_regex}"
  SOURCES
    test_communication_ready_gate.cpp
    test_deadline_waiter.cpp
    test_failure_check.cpp
    test_participant_create.cpp
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
# The texts of SIL Kit's errors that create_participant() goes by must still be in its sources.
target_compile_definitions(vehicle_sil_core_tests PRIVATE
                           VEHICLE_SIL_SILKIT_SOURCE_DIR="${VEHICLE_SIL_EXTERNAL_DIR}/sil-kit")

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
