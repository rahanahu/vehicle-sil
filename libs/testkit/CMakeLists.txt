# vehicle_sil::testkit - what tests of the J1939 stack add to vehicle_sil::local (LocalBench):
# recorders of the network's events (Observer) and of raw frames (TapPort), and builders of
# fault injectors. testkit::LocalCan is another name for LocalBench. Uses only the public API of
# vehicle_sil::can, vehicle_sil::j1939 and vehicle_sil::ecu; no SIL Kit. Internal for now: the
# API may change.
add_library(vehicle_sil_testkit STATIC
  src/faults.cpp
  src/recorders.cpp)
add_library(vehicle_sil::testkit ALIAS vehicle_sil_testkit)
set_target_properties(vehicle_sil_testkit PROPERTIES EXPORT_NAME testkit)
target_include_directories(vehicle_sil_testkit PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_testkit PUBLIC vehicle_sil::local vehicle_sil::can vehicle_sil::j1939 vehicle_sil::ecu)
vehicle_sil_target_defaults(vehicle_sil_testkit)
