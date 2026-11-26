# Coordinated virtual-time benchmark suite

[日本語](../ja/internals/benchmark.md)

The benchmark suite (`scripts/benchmark-vtime.py` and `vehicle_sil_bench_ecu` under `tools/bench_ecu/`) reproducibly measures coordinated SIL Kit multi-process virtual-time execution without external proprietary binaries or physical hardware. It verifies bit-exact deterministic state digests and timestamped CAN TX/RX causal traces on every run, records actual compiler and optimization flags from build metadata, reports explicitly scoped process CPU, and writes a schema-validated JSON report (`schema/benchmark-report.schema.json`).

This suite is an internal development and evaluation tool. `vehicle_sil_bench_ecu` is gated by `VEHICLE_SIL_BUILD_BENCHMARKS` (default `ON` for top-level builds, `OFF` for FetchContent consumers) and is excluded from release archives (`scripts/package.py`).

| Section | Contents |
|---|---|
| [When to use which tool](#when-to-use-which-tool) | Choosing between this benchmark, Control transport metrics, and `VEHICLE_SIL_TRACE` |
| [Canonical cases and fixed workload](#canonical-cases-and-fixed-workload) | Cases A, B, C, optional Case D, `--smoke`, and alignment rules |
| [Measurement boundaries](#measurement-boundaries) | Steady-state window `[W, W + M)`, tail cushion, and causal CAN cutoff |
| [Anti-misinterpretation rules](#anti-misinterpretation-rules) | `LocalBench`, participant/process counts, non-summability, and `NetworkSimulator` |
| [Process CPU and unavailable semantics](#process-cpu-and-unavailable-semantics) | Scoped CPU records on Linux, macOS, and Windows |
| [Running locally and in CI](#running-locally-and-in-ci) | Build commands, CLI options, JSON report summary, and CI workflows |

## When to use which tool

| Tool | What it measures | When to use |
|---|---|---|
| `scripts/benchmark-vtime.py` | End-to-end coordinated virtual-time steady-state wall time, process CPU, and deterministic CAN/Control throughput across canonical multi-process topologies | Reproducible scaling baselines and before/after comparisons on Release builds |
| `vehicle_sil_launch run --control-metrics <file>` | Controller `HubTransport::stats()` message/byte/handler-time counters across `HubTransport` lifetime | Inspecting Control transport load on any real simulation or alongside this benchmark (`--control-metrics auto\|on`) |
| `VEHICLE_SIL_TRACE` ([Profiling trace](trace.md)) | Per-step critical path, sync wait, and scenario write round-trip breakdowns | Diagnosing *why* a step is slow (never enable `VEHICLE_SIL_TRACE` during speed baselines) |

## Canonical cases and fixed workload

All canonical cases run the same fixed virtual workload at `--speed max` (`warmup_sim_ms = 200`, `measure_sim_ms = 20000`, `tail_sim_ms = 10`, `stop_at_ms = 20210`, `declared_vars_per_ecu = 250`, `wire_vars_per_ecu = 254` including 4 built-in `ecu_main`/`Peer` variables, `can_period_ms = 10`, `can_frames = 1`, `step_work = 0`, default `sample_period_ms = 10`).

| Case ID | Profile | ECUs (`step_ms`) | `ecu_count` / `silkit_participant_count` / `os_process_count` | Resolved `time_step_ms` | Effective `sample_period_ms` | Bus load in `[W, W + M)` (20 s) | ECU steps in `[W, W + M)` |
|---|---|---|---|---|---|---|---|
| `case_a_1ecu_1ms` | Canonical (Case A) | `ecu0` (1 ms) | `1` / `2` / `2` | `1` (auto GCD) | `10` | `2,000` TX, `0` RX | `20,000` |
| `case_b_5ecu_1ms` | Canonical (Case B) | `ecu0..ecu4` (1 ms × 5) | `5` / `6` / `6` | `1` (auto GCD) | `10` | `10,000` TX, `40,000` RX | `100,000` |
| `case_c_5ecu_mixed` | Canonical (Case C) | `ecu0` (10 ms) + `ecu1..ecu4` (1 ms × 4) | `5` / `6` / `6` | `1` (auto GCD) | `10` | `10,000` TX, `40,000` RX | `82,000` |
| `case_d_5ecu_1ms_scenario` | Optional (`--cases all`) | `ecu0..ecu4` (1 ms × 5) + deterministic scenario | `5` / `6` / `6` | `1` (auto GCD) | `1` (L-03 `--sample-every-step`) | `10,000` TX, `40,000` RX + scenario `set`/`expect` | `100,000` |

- `ecu0` transmits at offset `0 ms` (`0, 10, 20, ...`); `ecu1..ecu4` transmit at offsets `1..4 ms`. In Case C, this single schedule exercises slow-to-fast (`10 ms -> 1 ms`), fast-to-fast (`1 ms -> 1 ms`), and fast-to-slow batching (`1 ms -> 10 ms`) simultaneously while holding bus message volume equal to Case B.
- `--smoke` runs the 3 canonical cases with `warmup_iterations = 0`, `iterations = 2`, `warmup_sim_ms = 50`, `measure_sim_ms = 200`, `tail_sim_ms = 10` (`stop_at_ms = 260`).
- **Up-front alignment rules:** Both `scripts/benchmark-vtime.py` (exit `2`) and `vehicle_sil_bench_ecu` (exit `4`) reject unaligned windows before starting a simulation: `warmup_ms >= 0`, `measure_ms > 0`, both must be integer multiples of `lcm(all ECU step_ms, can_period_ms)` (`10 ms` for canonical cases), and `tail_sim_ms >= max(step_ms)`.

## Measurement boundaries

```text
[Setup: check/--describe/spawn/join/hello]           <-- EXCLUDED from steady_state
   |
   v  t = 0 ms
[In-Run Warm-up: 0 <= t < W]                         <-- EXCLUDED from steady_state
   |
   v  t = W (warmup_sim_ms)                          <== STEADY-STATE START (t_start_i)
[Measured Steady-State Window: W <= t < W + M]       <== MEASURED (zero benchmark-added syscalls)
   |
   v  t = W + M                                      <== STEADY-STATE END (t_end_i); stop CAN TX
[Tail Cushion: W + M <= t < W + M + tail]            <-- EXCLUDED from steady_state
   |
   v  t = stop_at_ms = W + M + tail
[Shutdown: stop/last_word_wait/reap/write outputs]   <-- EXCLUDED from steady_state
```

- **Steady-state wall time:** Each ECU snapshots `std::chrono::steady_clock::now()` and self-CPU exactly twice (at `t = W` and `t = W + M`), executing zero benchmark-added syscalls and zero heap allocations during `W < now < W + M`. Cross-ECU steady-state wall time is `wall_ns = max(t_end_i) - min(t_start_i)`.
- **Run-level warm-up and hold guards:** Default `--warmup 1` full run is recorded in `warmup_iterations[]` and strictly excluded from `iterations[]` and `summary`. Any run with `timing.paused_ms != 0` or `timing.paced_steps != 0` fails verification. Setup/shutdown and full launcher wall times are quarantined under `non_steady_state`.
- **Exact deterministic CAN cutoff (`{tx_ms < W + M, rx_ms <= W + M}`):** `vehicle_sil_bench_ecu` stops benchmark CAN TX at `now >= W + M` and records RX through `now <= W + M`. Because `tail_sim_ms >= max(step_ms)` guarantees every ECU executes step `W + M` strictly before `stop_at_ms`, every causal delivery under SIL Kit's trivial CAN bus (`rx_ms == ((tx_ms // P_r) + 1) * P_r`, including `ecu1..4` frames arriving at `ecu0` at `rx_ms == W + M` in Case C) is captured deterministically regardless of whether any participant executes `t == stop_at_ms`.
- **Cross-iteration determinism:** Every ECU canonically sorts received CAN frames by `(tx_ns, id, payload_u64)` and updates its state digest using pure 64-bit integer arithmetic. `scripts/benchmark-vtime.py` verifies every TX/RX event and asserts identical `output_sha256` and `comm_trace_sha256` across all iterations.

## Anti-misinterpretation rules

`schema/benchmark-report.schema.json` enforces these constants under `interpretation_contract` and `workload.communication_load`:

1. **Never substitute `LocalBench` (`local_bench_substitutable: false`):** `execution_backend` is always `"silkit_coordinated_multiprocess"`. In-process `LocalBench` runs have no SIL Kit IPC or cross-process synchronization barrier and cannot substitute for coordinated SIL Kit measurements.
2. **Do not conflate ECU, participant, and process counts (`process_count_equals_participant_count_assumed: false`):** `topology` reports `ecu_count`, `silkit_participant_count` (which includes `SimulationController`), and `os_process_count` separately.
3. **Never sum wall time, CPU time, and handler time (`wall_cpu_handler_summable: false`):** `steady_state.wall_ms` (elapsed wall time over `[W, W + M)`), `process_cpu.*` (cumulative multi-core CPU time), and `control_transport_metrics.counters.handler_time_ns` (cumulative controller SIL Kit thread handler time over full `HubTransport` lifetime) measure orthogonal quantities and scopes.
4. **Do not conflate trivial CAN with `NetworkSimulator` (`can_simulation_mode: "silkit_trivial"`, `network_simulator: null`):** Canonical cases measure SIL Kit's built-in trivial zero-latency CAN broadcast without `vehicle_sil_can_bus_simulator` or Vector `sil-kit-net-sim`. Adding a `NetworkSimulator` adds `+1` coordinated participant and `+1` OS process on the step barrier, shifts CPU attribution, and stamps arbitrated frames at `tx.end > tx_ms` (invalidating the trivial-CAN `rx_ms` formula).

## Process CPU and unavailable semantics

| Field | Target processes | Interval | Mechanism | When unavailable |
|---|---|---|---|---|
| `process_cpu.ecu_steady_state` | `["ecu0", ...]` (`excludes: ["vehicle_sil_launch"]`) | `"steady_state_window"` (`[W, W + M]`) | `getrusage(RUSAGE_SELF)` on POSIX; `GetProcessTimes` on Windows | `status: "unavailable"`, `reason` set, `user_ms`/`sys_ms`/`total_ms`: `null` (never `0`) |
| `process_cpu.launcher_tree_total` | `["vehicle_sil_launch", "<reaped_children>"]` | `"full_process_lifetime"` (`includes_non_steady_state: true`) | Direct `os.wait4(proc.pid, 0)` on POSIX (`ru_maxrss // 1024` on macOS) | On Windows (`os.name == "nt"`) or `wait4` error: `status: "unavailable"`, numeric fields and `max_rss_kb`: `null` (never `0`) |

## Running locally and in CI

Build the Release binaries (`vehicle_sil_launch` and `vehicle_sil_bench_ecu`) and run the suite:

```sh
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release --target vehicle_sil_launch vehicle_sil_bench_ecu
scripts/benchmark-vtime.py --preset linux-gcc-release --out build/logs/benchmark-vtime.json
```

Quick determinism smoke check (matching CTest `benchmark.smoke_determinism`):

```sh
scripts/benchmark-vtime.py --preset linux-gcc-debug --smoke --no-wait
ctest --preset linux-gcc-debug -R '^benchmark\.smoke_determinism$'
```

- **Build flag provenance:** `scripts/benchmark-vtime.py` extracts actual compiler and optimization/sanitizer flags from `<build-dir>/compile_commands.json` (last-flag-wins semantics, with `CMakeCache.txt` fallback and `vehicle_sil_bench_ecu --bench-build-info` cross-check), never guessing from the preset name.
- **Optional Control transport metrics:** `--control-metrics auto` (default) probes `vehicle_sil_launch version --json` for feature `"control-metrics"` (`formats.control_metrics == 1`). On launchers without it, the report records `status: "unavailable"` with `unavailable_reason: "launcher_feature_unsupported"`. Pass `--control-metrics on` to require it (exit `2` if unsupported) or `--control-metrics off` to skip it (`status: "not_requested"`).
- **CI integration:** Normal CI runs `benchmark.smoke_determinism` (labels `integration;benchmark`, zero wall-clock speed thresholds) across Linux, TSan, ASan, and Windows. Manual Release benchmark artifact collection is provided by `.github/workflows/benchmark.yml` (`workflow_dispatch` only).
