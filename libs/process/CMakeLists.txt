# vehicle_sil_process - child processes for the vehicle-sil tools and the process-level tests:
# one that writes to a log file, and a run to the end with its standard output and standard
# error captured and a time limit (POSIX posix_spawn, Win32 CreateProcessW). Internal; not part
# of the library API.
add_library(vehicle_sil_process STATIC src/child_process.cpp)
add_library(vehicle_sil::process ALIAS vehicle_sil_process)
target_include_directories(vehicle_sil_process PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
vehicle_sil_target_defaults(vehicle_sil_process)
