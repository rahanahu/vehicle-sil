# vehicle_sil::trace: the rings, the file's shape, one ring per thread, before start() and after
# flush(). The launcher's and the ECUs' files of a whole run: launch_run.trace_files (tests/launch).
vehicle_sil_add_harness_tests(vehicle_sil_trace_tests
  PREFIX trace LABELS unit trace TIMEOUT 60
  SOURCES test_trace.cpp
  LINK vehicle_sil::trace)

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
