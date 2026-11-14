# What the bridge executables share (bridges/common): run_bridge() with fake hooks, and once with
# a real SIL Kit participant (in-process registry on a free port).
vehicle_sil_add_harness_tests(vehicle_sil_bridge_runtime_tests
  PREFIX bridge_runtime LABELS unit bridge_runtime TIMEOUT 60
  SOURCES test_run_bridge.cpp
  LINK vehicle_sil::bridge_runtime vehicle_sil::core)
