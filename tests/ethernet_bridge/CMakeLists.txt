# The Ethernet bridge (bridges/ethernet) on every platform: EthernetBridge against a fake SIL Kit
# controller and fake links, the statistics and monitor lines, the command line, and the bridge
# with real SIL Kit (in-process registry on a free port) and a fake link.
vehicle_sil_add_harness_tests(vehicle_sil_ethernet_bridge_tests
  PREFIX ethernet_bridge LABELS unit ethernet_bridge TIMEOUT 60
  SOURCES test_ethernet_bridge.cpp test_bridge_format.cpp test_bridge_options.cpp test_bridge_silkit.cpp
  LINK vehicle_sil::ethernet_bridge_core vehicle_sil_eth_link_fake vehicle_sil::core)
