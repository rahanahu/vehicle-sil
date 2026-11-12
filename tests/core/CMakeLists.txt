# Core: simulation time helpers and SIL Kit virtual time (in-process registry). The virtual
# time cases measure wall-clock speed and run alone.
vehicle_sil_add_harness_tests(vehicle_sil_core_tests
  PREFIX core LABELS unit core TIMEOUT 120 RUN_SERIAL "^virtual_time_"
  SOURCES test_time_helpers.cpp test_failure_check.cpp test_simulation_watch.cpp test_virtual_time.cpp test_time_gate.cpp
          test_time_gate_model.cpp test_deadline_waiter.cpp test_start_window.cpp
  LINK vehicle_sil::core)
# The gate's arithmetic and its timer are internal to the library.
target_include_directories(vehicle_sil_core_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/core/src)
