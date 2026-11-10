# Virtual time across processes: controller + physics (1 ms) + ECU with an existing tick()
# model and J1939 (10 ms) + LIN sensor (2 ms) + LIN body controller (5 ms). The scenarios
# measure wall-clock speed and run alone. Added only with VEHICLE_SIL_BUILD_EXAMPLES.
add_executable(vehicle_sil_vt_launcher vt_launcher.cpp)
target_link_libraries(vehicle_sil_vt_launcher PRIVATE vehicle_sil_test_process)
vehicle_sil_target_defaults(vehicle_sil_vt_launcher)
add_dependencies(vehicle_sil_vt_launcher sil-kit-registry vehicle_sil_sim_controller vehicle_sil_vt_physics
                 vehicle_sil_vt_ecu vehicle_sil_vt_lin_sensor vehicle_sil_vt_body_controller)

vehicle_sil_add_scenario_tests(vehicle_sil_vt_launcher
  PREFIX virtual_time LABELS integration virtual_time TIMEOUT 300 RUN_SERIAL
  WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}
  SCENARIOS fast slow-tick paced-fast paced-slow peer-failure
  ARGS --registry $<TARGET_FILE:sil-kit-registry>
       --controller $<TARGET_FILE:vehicle_sil_sim_controller>
       --physics $<TARGET_FILE:vehicle_sil_vt_physics>
       --ecu $<TARGET_FILE:vehicle_sil_vt_ecu>
       --lin-sensor $<TARGET_FILE:vehicle_sil_vt_lin_sensor>
       --body-controller $<TARGET_FILE:vehicle_sil_vt_body_controller>
       --end-ms 1000
       --timeout-ms 120000)
