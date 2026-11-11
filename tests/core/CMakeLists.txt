# Core: simulation time helpers and SIL Kit virtual time (in-process registry). The virtual
# time cases measure wall-clock speed and run alone.
vehicle_sil_add_harness_tests(vehicle_sil_core_tests
  PREFIX core LABELS unit core TIMEOUT 120 RUN_SERIAL "^virtual_time_"
  SOURCES test_time_helpers.cpp test_failure_check.cpp test_simulation_watch.cpp test_virtual_time.cpp test_time_gate.cpp
  LINK vehicle_sil::core)
