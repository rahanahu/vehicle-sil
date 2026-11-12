# vehicle_sil::Simulation: the same ECUs (written against vehicle_sil::Ecu) on the local virtual
# buses and on SIL Kit (in-process registry), also LIN, failures, the rules of a run, a controller
# elsewhere and other participants; what an ECU reports about itself (Ecu::describe()) and the
# main() of an ECU executable (ecu_main()), called as a function.
vehicle_sil_add_harness_tests(vehicle_sil_simulation_tests
  PREFIX simulation LABELS unit simulation TIMEOUT 180
  SOURCES test_simulation.cpp test_lin_simulation.cpp test_describe.cpp test_ecu_main.cpp
  LINK vehicle_sil::simulation vehicle_sil::testkit vehicle_sil::control_silkit)
# simulation_internal.hpp: the tests shorten a wait of the library.
target_include_directories(vehicle_sil_simulation_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/simulation/src)

# The first example, on the virtual buses and on SIL Kit (registry of the process).
if(TARGET vehicle_sil_getting_started)
  add_test(NAME getting_started.demo COMMAND vehicle_sil_getting_started)
  add_test(NAME getting_started.silkit_demo COMMAND vehicle_sil_getting_started --silkit)
  set_tests_properties(getting_started.demo getting_started.silkit_demo
                       PROPERTIES LABELS "examples;simulation" TIMEOUT 120)
endif()

# Examples that run on the virtual buses and check their own result.
if(TARGET vehicle_sil_typed_messages)
  add_test(NAME typed_messages.demo COMMAND vehicle_sil_typed_messages)
  set_tests_properties(typed_messages.demo PROPERTIES LABELS "examples;simulation" TIMEOUT 120)
endif()
if(TARGET vehicle_sil_pid_stream)
  add_test(NAME pid_stream.demo COMMAND vehicle_sil_pid_stream)
  set_tests_properties(pid_stream.demo PROPERTIES LABELS "examples;simulation" TIMEOUT 120)
endif()
if(TARGET vehicle_sil_fault_injection)
  add_test(NAME fault_injection.demo COMMAND vehicle_sil_fault_injection)
  set_tests_properties(fault_injection.demo PROPERTIES LABELS "examples;simulation" TIMEOUT 120)
endif()
if(TARGET vehicle_sil_vehicle)
  add_test(NAME vehicle.silkit_demo COMMAND vehicle_sil_vehicle)
  add_test(NAME vehicle.local_demo COMMAND vehicle_sil_vehicle --local)
  set_tests_properties(vehicle.silkit_demo vehicle.local_demo PROPERTIES LABELS "examples;simulation" TIMEOUT 120)
endif()
# The layers under Simulation, assembled by hand.
if(TARGET vehicle_sil_low_level)
  add_test(NAME low_level.demo COMMAND vehicle_sil_low_level)
  set_tests_properties(low_level.demo PROPERTIES LABELS "examples" TIMEOUT 120)
endif()

# ECU executables made with ecu_main() (examples/ecu_main), as processes: --describe against
# the JSON in docs/ecu-executable.md, --local, both ECUs on SIL Kit under
# vehicle_sil_sim_controller, the usage, a stop request before and after the start.
if(TARGET vehicle_sil_engine_ecu)
  add_executable(vehicle_sil_ecu_main_launcher ecu_main_launcher.cpp)
  target_link_libraries(vehicle_sil_ecu_main_launcher PRIVATE vehicle_sil_test_process vehicle_sil::core)
  vehicle_sil_target_defaults(vehicle_sil_ecu_main_launcher)
  add_dependencies(vehicle_sil_ecu_main_launcher sil-kit-registry vehicle_sil_sim_controller vehicle_sil_engine_ecu
                   vehicle_sil_dashboard_ecu)
  vehicle_sil_add_scenario_tests(vehicle_sil_ecu_main_launcher
    PREFIX ecu_main LABELS integration simulation TIMEOUT 180
    WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/ecu_main
    SCENARIOS describe local silkit usage signal
    ARGS --registry $<TARGET_FILE:sil-kit-registry>
         --controller $<TARGET_FILE:vehicle_sil_sim_controller>
         --engine $<TARGET_FILE:vehicle_sil_engine_ecu>
         --dashboard $<TARGET_FILE:vehicle_sil_dashboard_ecu>
         --docs ${PROJECT_SOURCE_DIR}/docs/ecu-executable.md
         --timeout-ms 120000)
endif()
