# Coordinated virtual-time benchmark determinism smoke test (scripts/benchmark-vtime.py --smoke).
# Verifies determinism, causal CAN TX/RX timestamps, steady-state boundaries, and report structure
# across all 3 canonical cases with ZERO absolute wall-clock speed thresholds.
if(NOT TARGET vehicle_sil_launch OR NOT TARGET vehicle_sil_bench_ecu OR NOT VEHICLE_SIL_J1939_CODEGEN_PYTHON)
  return()
endif()

_vehicle_sil_scaled_ctest_timeout(_bench_timeout 180)
add_test(
  NAME benchmark.smoke_determinism
  COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B
          ${PROJECT_SOURCE_DIR}/scripts/benchmark-vtime.py
          --smoke
          --no-wait
          --build-dir ${CMAKE_BINARY_DIR}
          --launcher $<TARGET_FILE:vehicle_sil_launch>
          --bench-ecu $<TARGET_FILE:vehicle_sil_bench_ecu>
          --out ${CMAKE_CURRENT_BINARY_DIR}/smoke-report.json)
set_tests_properties(
  benchmark.smoke_determinism
  PROPERTIES
    LABELS "integration;benchmark"
    TIMEOUT ${_bench_timeout}
    ${_vehicle_sil_tsan_processors})
_vehicle_sil_tell_ctest_timeout_factor(benchmark.smoke_determinism)
