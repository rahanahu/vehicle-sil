# vehicle_sil::Simulation: the same ECUs (written against vehicle_sil::Ecu) on the local virtual
# buses and on SIL Kit (in-process registry), failures, the rules of a run, a controller
# elsewhere and other participants.
vehicle_sil_add_harness_tests(vehicle_sil_simulation_tests
  PREFIX simulation LABELS unit simulation TIMEOUT 180
  SOURCES test_simulation.cpp
  LINK vehicle_sil::simulation)

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
