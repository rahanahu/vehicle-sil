# What the bridge executables share (bridges/common): run_bridge() with fake hooks, and once with
# a real SIL Kit participant (in-process registry on a free port); the message for a registry that
# does not answer, also against what SIL Kit really throws; what a bridge writes for --describe,
# against vehicle_sil::to_json() and vehicle_sil_launch's reader.
vehicle_sil_add_harness_tests(vehicle_sil_bridge_runtime_tests
  PREFIX bridge_runtime LABELS unit bridge_runtime TIMEOUT 60
  SOURCES test_describe.cpp test_nothing_came_hint.cpp test_run_bridge.cpp test_registry_failure.cpp
  LINK vehicle_sil::bridge_runtime vehicle_sil::core vehicle_sil::ecu vehicle_sil::launch)
