# A simulation over two computers, made as two Linux network namespaces in a user namespace: the
# launcher on one, an ECU on the other (two_hosts.py says what it checks). Skipped (77) where the
# namespaces cannot be made, with the reason. Not on Windows (no namespaces there).
if(WIN32 OR NOT TARGET vehicle_sil_launch OR NOT TARGET vehicle_sil_dashboard_ecu)
  return()
endif()
vehicle_sil_find_python(_multi_host_python)
if(NOT _multi_host_python)
  message(STATUS "vehicle-sil: no Python 3.11+ for multi_host.two_hosts (tried: ${_multi_host_python_TRIED})")
  return()
endif()
add_test(NAME multi_host.two_hosts
  COMMAND "${_multi_host_python}" ${CMAKE_CURRENT_SOURCE_DIR}/two_hosts.py
          --launch $<TARGET_FILE:vehicle_sil_launch>
          --example-dir $<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..)
set_tests_properties(multi_host.two_hosts PROPERTIES LABELS "integration;launch" TIMEOUT 300 SKIP_RETURN_CODE 77)
