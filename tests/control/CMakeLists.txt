# Control agent: typed variables and timed writes applied in the steps, without a transport;
# owned and driven by an ECU (vehicle_sil::Ecu: Peer with an in-process registry, LocalBench,
# Simulation on both).
vehicle_sil_add_harness_tests(vehicle_sil_control_tests
  PREFIX control LABELS unit control TIMEOUT 120
  SOURCES test_agent.cpp test_peer_agent.cpp test_ecu_variables.cpp test_hub.cpp
  LINK vehicle_sil::simulation)

# A transport that outlives its ECU ends the process with a message (not a use after free).
add_executable(vehicle_sil_transport_outlives_ecu transport_outlives_ecu.cpp)
target_link_libraries(vehicle_sil_transport_outlives_ecu PRIVATE vehicle_sil::simulation)
vehicle_sil_target_defaults(vehicle_sil_transport_outlives_ecu)
vehicle_sil_copy_runtime_dlls(vehicle_sil_transport_outlives_ecu)
add_test(NAME control.transport_outliving_its_ecu_terminates COMMAND vehicle_sil_transport_outlives_ecu)
set_tests_properties(control.transport_outliving_its_ecu_terminates PROPERTIES
  LABELS "unit;control" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "Engine.*still use its control agent.*\n.*terminate handler reached"
  FAIL_REGULAR_EXPRESSION "without terminate")
