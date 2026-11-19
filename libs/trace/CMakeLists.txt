# vehicle_sil::trace - an internal trace of steps and writes for profiling (docs/internals/trace.md):
# off unless VEHICLE_SIL_TRACE names a directory, then per-thread rings written per process as
# Chrome Trace Event JSON. Standard library only and no SIL Kit, so that the libraries below and
# above the SIL Kit line (control, core, control_silkit, runner) can all record.
find_package(Threads REQUIRED)
add_library(vehicle_sil_trace STATIC src/trace.cpp)
add_library(vehicle_sil::trace ALIAS vehicle_sil_trace)
set_target_properties(vehicle_sil_trace PROPERTIES EXPORT_NAME trace)
target_include_directories(vehicle_sil_trace PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_trace PUBLIC Threads::Threads)
vehicle_sil_target_defaults(vehicle_sil_trace)
