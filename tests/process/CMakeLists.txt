# vehicle_sil::process: child processes with captured output, a time limit and odd paths, with a
# helper executable that writes, sleeps and exits as its arguments say (also used by the tests
# of vehicle_sil_launch).
add_executable(vehicle_sil_process_helper process_helper.cpp)
if(WIN32)
  target_link_libraries(vehicle_sil_process_helper PRIVATE shell32)
endif()
vehicle_sil_target_defaults(vehicle_sil_process_helper)

vehicle_sil_add_harness_tests(vehicle_sil_process_tests
  PREFIX process LABELS unit process TIMEOUT 120
  SOURCES test_child_process.cpp test_process.cpp
  LINK vehicle_sil::process)
add_dependencies(vehicle_sil_process_tests vehicle_sil_process_helper)
target_compile_definitions(vehicle_sil_process_tests PRIVATE
  VEHICLE_SIL_PROCESS_HELPER="$<TARGET_FILE:vehicle_sil_process_helper>"
  VEHICLE_SIL_PROCESS_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch")
