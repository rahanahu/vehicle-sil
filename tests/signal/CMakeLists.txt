# vehicle_sil::signal: the values of the waveforms at given times (the corners, the ends of a
# period, phase, duty 0 and 100), that the noise and the bounces depend on the time alone, and the
# numbers of the random sequence, which must be the same on every platform. Links only
# vehicle_sil::signal, so building it checks that the header needs nothing else.
vehicle_sil_add_harness_tests(vehicle_sil_signal_tests
  PREFIX signal LABELS unit signal TIMEOUT 60
  SOURCES test_waveform.cpp
  LINK vehicle_sil::signal)

# vehicle_sil::signal includes no SIL Kit header (it includes no vehicle-sil header at all).
add_test(NAME signal.boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/signal
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
set_tests_properties(signal.boundary PROPERTIES LABELS "unit;signal" TIMEOUT 60)
