# Unit tests: command line.
vehicle_sil_add_harness_tests(vehicle_sil_unit_tests
  PREFIX unit LABELS unit TIMEOUT 60
  SOURCES test_cli_args.cpp
  LINK vehicle_sil_cli)
