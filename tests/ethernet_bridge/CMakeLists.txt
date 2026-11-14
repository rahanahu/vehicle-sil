# The Ethernet bridge (bridges/ethernet) on every platform: EthernetBridge against a fake SIL Kit
# controller and fake links, the statistics and monitor lines, the command line, the bridge
# with real SIL Kit (in-process registry on a free port) and a fake link, and two bridges on pcap
# links on one device of the fake pcap library (tests/eth_link/fake_pcap).
vehicle_sil_add_harness_tests(vehicle_sil_ethernet_bridge_tests
  PREFIX ethernet_bridge LABELS unit ethernet_bridge TIMEOUT 60
  SOURCES test_ethernet_bridge.cpp test_bridge_format.cpp test_bridge_options.cpp test_bridge_silkit.cpp
          test_bridge_app.cpp test_bridge_on_pcap.cpp
  LINK vehicle_sil::ethernet_bridge_core vehicle_sil_eth_link_fake vehicle_sil::core)
# The pcap link's table, to load the fake library as Npcap on every platform.
target_include_directories(vehicle_sil_ethernet_bridge_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/eth_link/src/pcap)
target_compile_definitions(vehicle_sil_ethernet_bridge_tests PRIVATE VEHICLE_SIL_FAKE_PCAP_DIR="${VEHICLE_SIL_FAKE_PCAP_DIR}")
add_dependencies(vehicle_sil_ethernet_bridge_tests vehicle_sil_fake_pcap)

# The production bridge executable (bridges/ethernet/app/main.cpp) with a factory of fake links
# instead of the links of the operating system.
set(_bridge_dir "${PROJECT_SOURCE_DIR}/bridges/ethernet")
add_executable(vehicle_sil_ethernet_bridge_on_fake ${_bridge_dir}/app/main.cpp fake_link_factory.cpp)
target_include_directories(vehicle_sil_ethernet_bridge_on_fake PRIVATE ${_bridge_dir}/app)
target_link_libraries(vehicle_sil_ethernet_bridge_on_fake
  PRIVATE vehicle_sil::ethernet_bridge_core vehicle_sil_eth_link_fake vehicle_sil::bridge_runtime vehicle_sil::core
          vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_ethernet_bridge_on_fake)
vehicle_sil_copy_runtime_dlls(vehicle_sil_ethernet_bridge_on_fake)

# The production bridge executable with its own factory of the links of the operating system, and
# the fake pcap library (tests/eth_link/fake_pcap) in place of the installed one: only where the
# library is loaded from differs (fake_pcap_library.cpp).
add_executable(vehicle_sil_ethernet_bridge_on_fake_pcap
  ${_bridge_dir}/app/main.cpp ${_bridge_dir}/app/os_link_factory.cpp fake_pcap_library.cpp)
target_include_directories(vehicle_sil_ethernet_bridge_on_fake_pcap PRIVATE ${_bridge_dir}/app)
target_link_libraries(vehicle_sil_ethernet_bridge_on_fake_pcap
  PRIVATE vehicle_sil::ethernet_bridge_core vehicle_sil::bridge_runtime vehicle_sil::core vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_ethernet_bridge_on_fake_pcap)
vehicle_sil_copy_runtime_dlls(vehicle_sil_ethernet_bridge_on_fake_pcap)
add_dependencies(vehicle_sil_ethernet_bridge_on_fake_pcap vehicle_sil_fake_pcap vehicle_sil_fake_pcap_old)

# The SIL Kit side of the process tests: SIL Kit's own Ethernet controller.
add_executable(vehicle_sil_ethernet_bridge_it_peer eth_bridge_it_peer.cpp)
target_link_libraries(vehicle_sil_ethernet_bridge_it_peer PRIVATE vehicle_sil::core vehicle_sil::eth_link vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_ethernet_bridge_it_peer)
vehicle_sil_copy_runtime_dlls(vehicle_sil_ethernet_bridge_it_peer)

add_executable(vehicle_sil_ethernet_bridge_it_launcher eth_bridge_it_launcher.cpp)
target_link_libraries(vehicle_sil_ethernet_bridge_it_launcher PRIVATE vehicle_sil_test_process)
vehicle_sil_target_defaults(vehicle_sil_ethernet_bridge_it_launcher)
add_dependencies(vehicle_sil_ethernet_bridge_it_launcher
  sil-kit-registry vehicle_sil_ethernet_bridge_on_fake vehicle_sil_ethernet_bridge_on_fake_pcap vehicle_sil_ethernet_bridge
  vehicle_sil_ethernet_bridge_it_peer)

vehicle_sil_add_scenario_tests(vehicle_sil_ethernet_bridge_it_launcher
  PREFIX ethernet_bridge.integration LABELS ethernet_bridge integration TIMEOUT 120
  WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/integration
  SCENARIOS echo-roundtrip list-interfaces bad-usage no-filter no-interface link-fault stop-request backend-not-here
            pcap-roundtrip pcap-list pcap-no-library pcap-old-library pcap-no-interface pcap-fault
  ARGS --registry $<TARGET_FILE:sil-kit-registry>
       --bridge $<TARGET_FILE:vehicle_sil_ethernet_bridge_on_fake>
       --real-bridge $<TARGET_FILE:vehicle_sil_ethernet_bridge>
       --pcap-bridge $<TARGET_FILE:vehicle_sil_ethernet_bridge_on_fake_pcap>
       --fake-pcap-dir ${VEHICLE_SIL_FAKE_PCAP_DIR}
       --peer $<TARGET_FILE:vehicle_sil_ethernet_bridge_it_peer>
       --timeout-ms 60000)

# The real executable on a TAP device and on a veth pair, in a network namespace of a user
# namespace (bridge_netns.py says what it checks). Linux only. Skipped (77), with the reason,
# where the namespace or the interfaces cannot be made.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()
vehicle_sil_find_python(_bridge_netns_python)
if(NOT _bridge_netns_python)
  message(STATUS "vehicle-sil: no Python 3.11+ for ethernet_bridge.netns.* (tried: ${_bridge_netns_python_TRIED})")
  return()
endif()
foreach(_scenario tap packet pcap)
  add_test(NAME ethernet_bridge.netns.${_scenario}
    COMMAND "${_bridge_netns_python}" ${CMAKE_CURRENT_SOURCE_DIR}/bridge_netns.py --scenario ${_scenario}
            --bridge $<TARGET_FILE:vehicle_sil_ethernet_bridge>
            --peer $<TARGET_FILE:vehicle_sil_ethernet_bridge_it_peer>
            --registry $<TARGET_FILE:sil-kit-registry>)
  set_tests_properties(ethernet_bridge.netns.${_scenario}
    PROPERTIES LABELS "integration;ethernet_bridge" TIMEOUT 300 SKIP_RETURN_CODE 77)
endforeach()
