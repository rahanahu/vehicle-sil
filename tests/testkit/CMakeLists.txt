# The local buses (vehicle_sil::local, through the test kit's name testkit::LocalCan) and the test
# kit (vehicle_sil::testkit): J1939 networks on a local virtual bus, recorders, fault injection.
# Links no SIL Kit.
vehicle_sil_add_harness_tests(vehicle_sil_testkit_tests
  PREFIX testkit LABELS unit testkit j1939 TIMEOUT 60
  SOURCES test_testkit.cpp test_local_ecus.cpp
  LINK vehicle_sil::testkit)

# The ECU application API as its users write it (examples/j1939/ecu_pair and parts of one's own)
# on the local virtual bus. The example's typed messages come from its YAML (generated here too
# when the examples are not built).
if(NOT TARGET vehicle_sil_ecu_pair_messages)
  vehicle_sil_add_j1939_codec(vehicle_sil_ecu_pair_messages
                              ${PROJECT_SOURCE_DIR}/examples/j1939/ecu_pair/ecu_pair_messages.yaml)
endif()
vehicle_sil_add_harness_tests(vehicle_sil_ecu_api_tests
  PREFIX ecu_api LABELS unit testkit j1939 TIMEOUT 60
  SOURCES test_ecu_personas.cpp
  LINK vehicle_sil::testkit vehicle_sil_ecu_pair_messages vehicle_sil::j1939_diagnostics
       vehicle_sil::j1939_periodic_sender vehicle_sil::j1939_requester vehicle_sil::j1939_monitor
       vehicle_sil::j1939_address_commander)
target_include_directories(vehicle_sil_ecu_api_tests PRIVATE ${PROJECT_SOURCE_DIR}/examples/j1939/ecu_pair)

# A protocol of one's own as its developer tests it: the parameter service of
# examples/j1939/param_service (built here too when the examples are not built) on the local
# virtual bus, and the example's demo. The example uses the stack's public API only.
if(NOT TARGET vehicle_sil_param_service)
  set(_param_service ${PROJECT_SOURCE_DIR}/examples/j1939/param_service)
  vehicle_sil_add_j1939_codec(vehicle_sil_param_service_messages ${_param_service}/param_service_messages.yaml)
  add_library(vehicle_sil_param_service STATIC ${_param_service}/param_service.cpp)
  target_include_directories(vehicle_sil_param_service PUBLIC ${_param_service})
  target_link_libraries(vehicle_sil_param_service PUBLIC vehicle_sil::j1939_exchange vehicle_sil_param_service_messages)
  vehicle_sil_target_defaults(vehicle_sil_param_service)
endif()
vehicle_sil_add_harness_tests(vehicle_sil_param_service_tests
  PREFIX param_service LABELS unit testkit j1939 TIMEOUT 60
  SOURCES test_param_service.cpp
  LINK vehicle_sil::testkit vehicle_sil_param_service)
add_test(NAME param_service.boundary
         COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${PROJECT_SOURCE_DIR}/examples/j1939/param_service -DSOURCES_ONLY=ON
                 -DFORBID_OBSERVER=ON -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
set_tests_properties(param_service.boundary PROPERTIES LABELS "unit;testkit;j1939" TIMEOUT 60)
if(TARGET vehicle_sil_j1939_param_service)
  add_test(NAME param_service.demo COMMAND vehicle_sil_j1939_param_service)
  set_tests_properties(param_service.demo PROPERTIES LABELS "unit;testkit;j1939" TIMEOUT 60)
endif()
