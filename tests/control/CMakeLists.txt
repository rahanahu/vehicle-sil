# Control agent: typed variables and timed writes applied in the steps, without a transport;
# driven by vehicle_sil::Peer (in-process registry).
vehicle_sil_add_harness_tests(vehicle_sil_control_tests
  PREFIX control LABELS unit control TIMEOUT 120
  SOURCES test_agent.cpp test_peer_agent.cpp test_hub.cpp
  LINK vehicle_sil::peer_control)
