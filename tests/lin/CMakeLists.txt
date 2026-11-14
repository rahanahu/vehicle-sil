# The virtual LIN bus alone, with fake nodes (no SIL Kit).
vehicle_sil_add_harness_tests(vehicle_sil_lin_bus_tests
  PREFIX lin_bus LABELS unit lin TIMEOUT 60
  SOURCES test_virtual_lin_bus.cpp
  LINK vehicle_sil::lin)

# LinMaster and LinSlave on the virtual LIN buses of a LocalBench, through Ecu (no SIL Kit): the
# same scenarios and results as the SIL Kit tests below.
vehicle_sil_add_harness_tests(vehicle_sil_lin_local_tests
  PREFIX lin_local LABELS unit lin TIMEOUT 60
  SOURCES test_lin_local.cpp
  LINK vehicle_sil::local)

# vehicle_sil::lin includes no SIL Kit header (it links vehicle_sil::sim_base only); the check
# itself must reject a SIL Kit include.
add_test(NAME lin.boundary
         COMMAND ${CMAKE_COMMAND} -DDIR=${PROJECT_SOURCE_DIR}/libs/lin
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
add_test(NAME lin.boundary_check_rejects_silkit
         COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/boundary_fixtures/silkit_include
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckNoSilKit.cmake)
set_tests_properties(lin.boundary lin.boundary_check_rejects_silkit PROPERTIES LABELS "unit;lin" TIMEOUT 60)
# CMake wraps the first line of the message at a width that depends on the path, so match the
# line that names the file and the include (indented, never wrapped).
set_tests_properties(lin.boundary_check_rejects_silkit PROPERTIES
                     PASS_REGULAR_EXPRESSION "fixture\\.hpp: silkit/")

# LIN: schedule, frames, checksums, diagnostic frames, sleep and wakeup with SIL Kit LIN in
# virtual time (in-process registry), through vehicle_sil::lin_silkit.
vehicle_sil_add_harness_tests(vehicle_sil_lin_tests
  PREFIX lin LABELS unit lin TIMEOUT 120 RUN_SERIAL "wall_clock|every_speed"
  SOURCES
    test_lin_cluster.cpp
    test_lin_protocol.cpp
    test_lin_virtual_time.cpp
  LINK vehicle_sil::lin_silkit)

# LIN cluster across processes: controller + LIN master (5 ms) + two LIN slaves (2 ms, 4 ms):
# schedule, master and slave frames, sleep and wakeup by master, slave and bus idle. The
# launcher runs the example peers, so only with VEHICLE_SIL_BUILD_EXAMPLES.
if(VEHICLE_SIL_BUILD_EXAMPLES)
  add_executable(vehicle_sil_lin_launcher lin_launcher.cpp)
  target_link_libraries(vehicle_sil_lin_launcher PRIVATE vehicle_sil_test_process)
  vehicle_sil_target_defaults(vehicle_sil_lin_launcher)
  add_dependencies(vehicle_sil_lin_launcher sil-kit-registry vehicle_sil_sim_controller vehicle_sil_lin_master
                   vehicle_sil_lin_slave)

  vehicle_sil_add_scenario_tests(vehicle_sil_lin_launcher
    PREFIX lin_cluster LABELS integration lin TIMEOUT 300
    WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/lin_cluster
    SCENARIOS
      cluster
      cluster-master-10ms
      cluster-paced
    ARGS --registry $<TARGET_FILE:sil-kit-registry>
         --controller $<TARGET_FILE:vehicle_sil_sim_controller>
         --master $<TARGET_FILE:vehicle_sil_lin_master>
         --slave $<TARGET_FILE:vehicle_sil_lin_slave>
         --timeout-ms 120000)
endif()
