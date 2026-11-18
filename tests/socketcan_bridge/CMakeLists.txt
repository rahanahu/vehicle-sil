# SocketCAN bridge tests (Linux only).
#
#   socketcan_bridge.*         in-process: the command line, the netlink answers and the lists, the
#                              error frames, and SocketCanPort on a fake CanDevice (frames, receipts,
#                              error frames, bus-off, ENOBUFS, ENETDOWN, ENODEV).
#   socketcan_bridge.netns.*   the executable on a virtual CAN interface (vcan) in a network namespace
#                              of a user namespace (bridge_netns.py says what it checks), with the SIL
#                              Kit peer of the Vector XL bridge's tests. Skipped (77), with the reason,
#                              where the namespace or vcan0 cannot be made; with
#                              VEHICLE_SIL_REQUIRE_SOCKETCAN=1 that fails the test.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()

vehicle_sil_add_harness_tests(vehicle_sil_socketcan_tests
  PREFIX socketcan_bridge LABELS socketcan_bridge unit TIMEOUT 60
  SOURCES
    test_bridge_options.cpp
    test_can_interfaces.cpp
    test_socketcan_port.cpp
  LINK vehicle_sil::socketcan_bridge_core vehicle_sil::core)

vehicle_sil_find_python(_socketcan_netns_python)
if(NOT _socketcan_netns_python)
  message(STATUS "vehicle-sil: no Python 3.11+ for socketcan_bridge.netns.* (tried: ${_socketcan_netns_python_TRIED})")
  return()
endif()
# The peer of the Vector XL bridge's integration tests: it sends the frame matrix of tests/can_bridge
# and expects the answers of a device (the script).
foreach(_scenario roundtrip link refused shared)
  add_test(NAME socketcan_bridge.netns.${_scenario}
    COMMAND "${_socketcan_netns_python}" ${CMAKE_CURRENT_SOURCE_DIR}/bridge_netns.py --scenario ${_scenario}
            --bridge $<TARGET_FILE:vehicle_sil_socketcan_bridge>
            --peer $<TARGET_FILE:vehicle_sil_vector_xl_it_peer>
            --registry $<TARGET_FILE:sil-kit-registry>)
  set_tests_properties(socketcan_bridge.netns.${_scenario}
    PROPERTIES LABELS "integration;socketcan_bridge" TIMEOUT 300 SKIP_RETURN_CODE 77)
endforeach()

# launch: vehicle_sil_launch run --connect J1939_CAN=vcan0 starts the bridge itself, beside the engine ECU.
if(TARGET vehicle_sil_launch AND TARGET vehicle_sil_engine_ecu)
  add_test(NAME socketcan_bridge.netns.launch
    COMMAND "${_socketcan_netns_python}" ${CMAKE_CURRENT_SOURCE_DIR}/bridge_netns.py --scenario launch
            --bridge $<TARGET_FILE:vehicle_sil_socketcan_bridge>
            --peer $<TARGET_FILE:vehicle_sil_vector_xl_it_peer>
            --registry $<TARGET_FILE:sil-kit-registry>
            --launch $<TARGET_FILE:vehicle_sil_launch>
            --ecu $<TARGET_FILE:vehicle_sil_engine_ecu>)
  set_tests_properties(socketcan_bridge.netns.launch
    PROPERTIES LABELS "integration;socketcan_bridge" TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()
