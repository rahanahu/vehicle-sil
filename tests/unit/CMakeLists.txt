# Unit tests: command line, and the factor on the tests' upper bounds of waiting.
vehicle_sil_add_harness_tests(vehicle_sil_unit_tests
  PREFIX unit LABELS unit TIMEOUT 60
  SOURCES
    test_cli_args.cpp
    test_cli_console.cpp
    test_timeout_scale.cpp
  LINK vehicle_sil_cli)
