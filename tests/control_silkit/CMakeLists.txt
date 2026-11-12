# The control agent over SIL Kit's publish/subscribe (vehicle_sil::control_silkit), with real
# participants and an in-process registry: the values before a pause are complete, a write made
# while the simulation stands still takes effect in the next step, a write made in the window
# before the start is seen in the first step whatever the order of the starts, and what happens
# when a hello does not come, is of another version, or a participant leaves.
vehicle_sil_add_harness_tests(vehicle_sil_control_silkit_tests
  PREFIX control_silkit LABELS unit control TIMEOUT 300
  SOURCES test_transport.cpp
  LINK vehicle_sil::control_silkit vehicle_sil::peer)
# The names of the topics are internal to the library (a test publishes a hello of another version).
target_include_directories(vehicle_sil_control_silkit_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/control_silkit/src)
