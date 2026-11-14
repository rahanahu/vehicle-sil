# vehicle_sil::step: the host with members that are not CAN, the step driver and the real-time
# driver (on a clock the test moves). Links only vehicle_sil::step, so building it checks that the
# library needs neither CAN nor SIL Kit.
vehicle_sil_add_harness_tests(vehicle_sil_step_tests
  PREFIX step LABELS unit step TIMEOUT 60
  SOURCES test_step_host.cpp
  LINK vehicle_sil::step)

# vehicle_sil::step includes no SIL Kit header, also through the vehicle-sil headers it includes.
add_test(NAME step.boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/step
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
set_tests_properties(step.boundary PROPERTIES LABELS "unit;step" TIMEOUT 60)
