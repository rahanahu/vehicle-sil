# vehicle_sil_bench_ecu: deterministic synthetic ECU executable for coordinated virtual-time
# benchmarking (scripts/benchmark-vtime.py). Internal; gated by VEHICLE_SIL_BUILD_BENCHMARKS and
# excluded from release archives.
add_executable(vehicle_sil_bench_ecu main.cpp)
target_link_libraries(vehicle_sil_bench_ecu PRIVATE
  vehicle_sil::simulation
  vehicle_sil::process
  vehicle_sil::json
  vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_bench_ecu)
vehicle_sil_copy_runtime_dlls(vehicle_sil_bench_ecu)
