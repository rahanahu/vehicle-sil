# vehicle_sil_process - child processes for the vehicle-sil tools and the process-level tests:
# one that writes to a log file, a run to the end with its standard output and standard error
# captured and a time limit, and one that a launcher watches (its output line by line, a stop
# request, the end of everything it started) (POSIX fork/exec and process groups, Win32
# CreateProcessW, process groups and job objects). Internal; not part of the library API.
add_library(vehicle_sil_process STATIC
  src/child_process.cpp
  src/process.cpp
  src/spawn.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(vehicle_sil_process PUBLIC Threads::Threads)
add_library(vehicle_sil::process ALIAS vehicle_sil_process)
target_include_directories(vehicle_sil_process PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
vehicle_sil_target_defaults(vehicle_sil_process)
