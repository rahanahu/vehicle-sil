# The CAN bridge core (bridges/can): CanBridge against a fake SIL Kit controller and a fake
# external port, and ExternalPortAdapter. They run on every platform. The fake external port
# (fake_external_port.hpp) and the frame matrix (frame_matrix.hpp) are also used by the tests of
# the Vector XL bridge (tests/vector_xl).
vehicle_sil_add_harness_tests(vehicle_sil_can_bridge_tests
  PREFIX can_bridge LABELS can_bridge unit TIMEOUT 60
  SOURCES
    test_can_bridge.cpp
    test_external_port_adapter.cpp
  LINK vehicle_sil::can_bridge_core vehicle_sil::can_external vehicle_sil::core)
