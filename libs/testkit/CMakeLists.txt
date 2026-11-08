# vehicle_sil::testkit - test setup for the J1939 stack without SIL Kit: a virtual CAN
# bus with J1939 networks stepped on virtual time (LocalCan), recorders of the network's
# events (Observer) and of raw frames (TapPort), and builders of fault injectors. Uses only the
# public API of vehicle_sil::can and vehicle_sil::j1939. Internal for now: the API may
# change.
add_library(vehicle_sil_testkit STATIC
  src/faults.cpp
  src/local_can.cpp
  src/recorders.cpp)
add_library(vehicle_sil::testkit ALIAS vehicle_sil_testkit)
set_target_properties(vehicle_sil_testkit PROPERTIES EXPORT_NAME testkit)
target_include_directories(vehicle_sil_testkit PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_testkit PUBLIC vehicle_sil::can vehicle_sil::j1939)
vehicle_sil_target_defaults(vehicle_sil_testkit)
