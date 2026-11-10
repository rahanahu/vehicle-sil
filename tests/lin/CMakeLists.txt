# LIN: schedule, frames, checksums, diagnostic frames, sleep and wakeup with SIL Kit LIN in
# virtual time (in-process registry).
vehicle_sil_add_harness_tests(vehicle_sil_lin_tests
  PREFIX lin LABELS unit lin TIMEOUT 120 RUN_SERIAL "wall_clock|every_speed"
  SOURCES test_lin_virtual_time.cpp test_lin_cluster.cpp test_lin_protocol.cpp
  LINK vehicle_sil::lin)

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
    SCENARIOS cluster cluster-paced cluster-master-10ms
    ARGS --registry $<TARGET_FILE:sil-kit-registry>
         --controller $<TARGET_FILE:vehicle_sil_sim_controller>
         --master $<TARGET_FILE:vehicle_sil_lin_master>
         --slave $<TARGET_FILE:vehicle_sil_lin_slave>
         --timeout-ms 120000)
endif()
