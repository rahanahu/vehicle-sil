# vehicle_sil::trace: the rings, the file's shape, one ring per thread, before start() and after
# flush(). The launcher's and the ECUs' files of a whole run: launch_run.trace_files (tests/launch).
vehicle_sil_add_harness_tests(vehicle_sil_trace_tests
  PREFIX trace LABELS unit trace TIMEOUT 60
  SOURCES test_trace.cpp
  LINK vehicle_sil::trace)

# The trace started by the environment: a relative directory becomes absolute (and is passed on so),
# and a VEHICLE_SIL_TRACE_EVENTS that is not a number from 1 to 2^24 gives the default.
foreach(_events_case "-1;262144" "16777217;262144" "x;262144" "1000;1024")
  list(GET _events_case 0 _events)
  list(GET _events_case 1 _expected)
  string(MAKE_C_IDENTIFIER "${_events}" _name)
  set(_test trace.started_by_the_environment.events_${_name})
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${_test})
  add_test(NAME ${_test} COMMAND vehicle_sil_trace_tests started_by_the_environment
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${_test})
  set_tests_properties(${_test} PROPERTIES LABELS "unit;trace" TIMEOUT 60
                       ENVIRONMENT "VEHICLE_SIL_TRACE=relative/trace;VEHICLE_SIL_TRACE_EVENTS=${_events};TRACE_EXPECT_EVENTS=${_expected}")
endforeach()

# Counts every allocation of the process (unit/allocation_counter.hpp): its own binary.
vehicle_sil_add_harness_tests(vehicle_sil_trace_allocation_tests
  PREFIX trace LABELS unit trace TIMEOUT 60
  SOURCES test_trace_allocation.cpp
  LINK vehicle_sil::trace)

# vehicle_sil::trace includes no SIL Kit header: vehicle_sil::control, which has none, records too.
add_test(NAME trace.boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/trace
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
set_tests_properties(trace.boundary PROPERTIES LABELS "unit;trace" TIMEOUT 60)
