# Unit tests: command line, and the factor on the tests' upper bounds of waiting.
vehicle_sil_add_harness_tests(vehicle_sil_unit_tests
  PREFIX unit LABELS unit TIMEOUT 60
  SOURCES
    test_cli_args.cpp
    test_cli_console.cpp
    test_timeout_scale.cpp
  LINK vehicle_sil_cli)

add_test(NAME unit.timeout_scale_cmake
         COMMAND ${CMAKE_COMMAND} -DHELPER=${PROJECT_SOURCE_DIR}/cmake/VehicleSilTestHelpers.cmake
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/test_timeout_scale_cmake.cmake)
set_tests_properties(unit.timeout_scale_cmake PROPERTIES LABELS "unit" TIMEOUT 60)

if(VEHICLE_SIL_J1939_CODEGEN_PYTHON)
  add_test(NAME unit.timeout_scale_python
           COMMAND "${VEHICLE_SIL_J1939_CODEGEN_PYTHON}" -B "${CMAKE_CURRENT_SOURCE_DIR}/test_timeout_scale.py")
  set_tests_properties(unit.timeout_scale_python PROPERTIES LABELS "unit;python" TIMEOUT 60)
endif()
