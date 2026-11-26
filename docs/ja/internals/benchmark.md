# 協調仮想時間ベンチマークスイート

[English](../../internals/benchmark.md)

このベンチマークスイート（`scripts/benchmark-vtime.py` と `tools/bench_ecu/` の `vehicle_sil_bench_ecu`）は、外部のプロプライエタリな実行ファイルや実機ハードウェアなしで、SIL Kit の協調マルチプロセス仮想時間実行を再現可能に計測します。すべての実行でビット一致の決定論的状態ダイジェストと時刻付き CAN 送受信因果トレースを検証し、ビルドメタデータから実際のコンパイラと最適化フラグを抽出し、対象と区間を明示したプロセス CPU を記録して、スキーマ検証済みの JSON レポート（`schema/benchmark-report.schema.json`）を出力します。

このスイートは開発・評価用の内部ツールです。`vehicle_sil_bench_ecu` は `VEHICLE_SIL_BUILD_BENCHMARKS`（トップレベルビルドで既定 `ON`、FetchContent 利用側では `OFF`）で制御され、リリースアーカイブ（`scripts/package.py`）には含めません。

| 節 | 内容 |
|---|---|
| [ツールの使い分け](#ツールの使い分け) | 本ベンチマーク、Control トランスポートメトリクス、`VEHICLE_SIL_TRACE` の選び方 |
| [標準ケースと固定ワークロード](#標準ケースと固定ワークロード) | Case A、B、C、任意の Case D、`--smoke`、境界整列ルール |
| [計測境界](#計測境界) | 定常区間 `[W, W + M)`、テール余白、CAN 因果カットオフ |
| [誤解釈を防ぐ契約](#誤解釈を防ぐ契約) | `LocalBench`、参加者数とプロセス数、非加算性、`NetworkSimulator` との関係 |
| [プロセス CPU と取得不可時の扱い](#プロセス-cpu-と取得不可時の扱い) | Linux、macOS、Windows におけるスコープ付き CPU 記録 |
| [ローカルと CI での実行](#ローカルと-ci-での実行) | ビルド手順、CLI オプション、JSON レポート要約、CI ワークフロー |

## ツールの使い分け

| ツール | 計測対象 | 使いどころ |
|---|---|---|
| `scripts/benchmark-vtime.py` | 標準マルチプロセス構成における協調仮想時間の定常ウォール時間、プロセス CPU、決定論的 CAN/Control スループット | Release ビルドでの再現可能なスケーリング基準計測と変更前後比較 |
| `vehicle_sil_launch run --control-metrics <file>` | `HubTransport` 生存期間全体を通じたコントローラー側 `HubTransport::stats()` のメッセージ数・バイト数・ハンドラー時間 | 任意の実シミュレーションまたは本ベンチマーク併用時（`--control-metrics auto\|on`）の Control トランスポート負荷確認 |
| `VEHICLE_SIL_TRACE`（[プロファイリングのトレース](trace.md)） | ステップごとのクリティカルパス、同期待ち、シナリオ書き込み往復の内訳 | ステップが遅い「原因」の特定（速度基準の計測時に `VEHICLE_SIL_TRACE` は有効にしない） |

## 標準ケースと固定ワークロード

すべての標準ケースは `--speed max` で同一の固定仮想ワークロードを実行します（`warmup_sim_ms = 200`、`measure_sim_ms = 20000`、`tail_sim_ms = 10`、`stop_at_ms = 20210`、`declared_vars_per_ecu = 250`、`ecu_main`/`Peer` の組み込み変数 4 個を含む `wire_vars_per_ecu = 254`、`can_period_ms = 10`、`can_frames = 1`、`step_work = 0`、既定 `sample_period_ms = 10`）。

| ケース ID | 種別 | ECU 構成（`step_ms`） | `ecu_count` / `silkit_participant_count` / `os_process_count` | 解決 `time_step_ms` | 実効 `sample_period_ms` | `[W, W + M)`（20 秒）のバス負荷 | `[W, W + M)` の ECU ステップ数 |
|---|---|---|---|---|---|---|---|
| `case_a_1ecu_1ms` | 標準（Case A） | `ecu0`（1 ms） | `1` / `2` / `2` | `1`（自動 GCD） | `10` | `2,000` TX、`0` RX | `20,000` |
| `case_b_5ecu_1ms` | 標準（Case B） | `ecu0..ecu4`（1 ms × 5） | `5` / `6` / `6` | `1`（自動 GCD） | `10` | `10,000` TX、`40,000` RX | `100,000` |
| `case_c_5ecu_mixed` | 標準（Case C） | `ecu0`（10 ms）+ `ecu1..ecu4`（1 ms × 4） | `5` / `6` / `6` | `1`（自動 GCD） | `10` | `10,000` TX、`40,000` RX | `82,000` |
| `case_d_5ecu_1ms_scenario` | 任意（`--cases all`） | `ecu0..ecu4`（1 ms × 5）+ 決定論的シナリオ | `5` / `6` / `6` | `1`（自動 GCD） | `1`（L-03 `--sample-every-step`） | `10,000` TX、`40,000` RX + シナリオ `set`/`expect` | `100,000` |

- `ecu0` はオフセット `0 ms`（`0, 10, 20, ...`）、`ecu1..ecu4` はオフセット `1..4 ms` で送信します。Case C ではこの単一スケジュールで、遅→速（`10 ms -> 1 ms`）、速→速（`1 ms -> 1 ms`）、速→遅バッチ受信（`1 ms -> 10 ms`）をすべて同時に検証しつつ、バスメッセージ総数を Case B と同一に保ちます。
- `--smoke` は 3 つの標準ケースを `warmup_iterations = 0`、`iterations = 2`、`warmup_sim_ms = 50`、`measure_sim_ms = 200`、`tail_sim_ms = 10`（`stop_at_ms = 260`）で実行します。
- **事前整列チェック:** `scripts/benchmark-vtime.py`（終了コード `2`）と `vehicle_sil_bench_ecu`（終了コード `4`）の双方が実行前に区間整列を検証します。`warmup_ms >= 0`、`measure_ms > 0`、双方が `lcm(全 ECU の step_ms, can_period_ms)`（標準ケースでは `10 ms`）の整数倍、かつ `tail_sim_ms >= max(step_ms)` でなければなりません。

## 計測境界

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

- **定常ウォール時間:** 各 ECU は `t = W` と `t = W + M` のちょうど 2 回だけ `std::chrono::steady_clock::now()` と自プロセス CPU を取得し、`W < now < W + M` の間はベンチマーク追加のシステムコールもヒープ確保も行いません。全 ECU 横断の定常ウォール時間は `wall_ns = max(t_end_i) - min(t_start_i)` です。
- **実行単位ウォームアップと一時停止ガード:** 既定の `--warmup 1` 回の実行は `warmup_iterations[]` に隔離され、`iterations[]` と `summary` からは厳格に除外されます。`timing.paused_ms != 0` または `timing.paced_steps != 0` の実行は検証失敗となります。起動・終了時間やランチャー全体の所要時間は `non_steady_state` に分離記録します。
- **決定論的 CAN カットオフ（`{tx_ms < W + M, rx_ms <= W + M}`）:** `vehicle_sil_bench_ecu` は `now >= W + M` でベンチマーク CAN 送信を停止し、`now <= W + M` までの受信を記録します。`tail_sim_ms >= max(step_ms)` により全 ECU が `stop_at_ms` より厳密に手前の `W + M` ステップを実行するため、SIL Kit 標準のトリビアル CAN バスでの因果到着（`rx_ms == ((tx_ms // P_r) + 1) * P_r`、Case C で `ecu1..4` の最終送信が `ecu0` に `rx_ms == W + M` で到着する分を含む）が、`t == stop_at_ms` の実行有無に依存せず完全に決定論的に回収されます。
- **反復間の決定論性:** 各 ECU は受信フレームを `(tx_ns, id, payload_u64)` で正規化ソートし、純粋な 64 ビット整数演算のみで状態ダイジェストを更新します。`scripts/benchmark-vtime.py` は全送受信イベントを照合し、全反復で `output_sha256` と `comm_trace_sha256` が完全一致することを検証します。

## 誤解釈を防ぐ契約

`schema/benchmark-report.schema.json` は `interpretation_contract` と `workload.communication_load` で次の定数を強制します。

1. **`LocalBench` で代替しない（`local_bench_substitutable: false`）:** `execution_backend` は常に `"silkit_coordinated_multiprocess"` です。プロセス内実行の `LocalBench` には SIL Kit IPC もプロセス間同期バリアもなく、協調 SIL Kit 計測の代替にはなりません。
2. **ECU 数・参加者数・プロセス数を混同しない（`process_count_equals_participant_count_assumed: false`）:** `topology` は `ecu_count`、`silkit_participant_count`（`SimulationController` を含む）、`os_process_count` を別々に記録します。
3. **ウォール時間・CPU 時間・ハンドラー時間を合算しない（`wall_cpu_handler_summable: false`）:** `steady_state.wall_ms`（`[W, W + M)` の経過ウォール時間）、`process_cpu.*`（マルチコア累積 CPU 時間）、`control_transport_metrics.counters.handler_time_ns`（`HubTransport` 全生存期間のコントローラー SIL Kit スレッド累積ハンドラー時間）は直交する別指標・別区間です。
4. **トリビアル CAN と `NetworkSimulator` を混同しない（`can_simulation_mode: "silkit_trivial"`, `network_simulator: null`）:** 標準ケースは `vehicle_sil_can_bus_simulator` や Vector `sil-kit-net-sim` を使わない SIL Kit 標準の遅延ゼロ・トリビアル CAN ブロードキャストを計測します。`NetworkSimulator` を追加するとステップバリア上の協調参加者と OS プロセスが `+1` され、CPU 帰属が変わり、調停後の到着時刻が `tx.end > tx_ms` となってトリビアル CAN の `rx_ms` 式が成立しなくなります。

## プロセス CPU と取得不可時の扱い

| フィールド | 対象プロセス | 区間 | 取得方式 | 取得不可時 |
|---|---|---|---|---|
| `process_cpu.ecu_steady_state` | `["ecu0", ...]`（`excludes: ["vehicle_sil_launch"]`） | `"steady_state_window"`（`[W, W + M]`） | POSIX は `getrusage(RUSAGE_SELF)`、Windows は `GetProcessTimes` | `status: "unavailable"`、`reason` を記録し、`user_ms`/`sys_ms`/`total_ms` は `null`（`0` にしない） |
| `process_cpu.launcher_tree_total` | `["vehicle_sil_launch", "<reaped_children>"]` | `"full_process_lifetime"`（`includes_non_steady_state: true`） | POSIX は直接 `os.wait4(proc.pid, 0)`（macOS は `ru_maxrss // 1024`） | Windows（`os.name == "nt"`）や `wait4` 失敗時は `status: "unavailable"`、数値と `max_rss_kb` は `null`（`0` にしない） |

## ローカルと CI での実行

Release バイナリ（`vehicle_sil_launch` と `vehicle_sil_bench_ecu`）をビルドして実行します。

```sh
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release --target vehicle_sil_launch vehicle_sil_bench_ecu
scripts/benchmark-vtime.py --preset linux-gcc-release --out build/logs/benchmark-vtime.json
```

短時間の決定論性スモーク確認（CTest `benchmark.smoke_determinism` と同等）:

```sh
scripts/benchmark-vtime.py --preset linux-gcc-debug --smoke --no-wait
ctest --preset linux-gcc-debug -R '^benchmark\.smoke_determinism$'
```

- **ビルドフラグの出所記録:** `scripts/benchmark-vtime.py` はプリセット名から推測せず、`<build-dir>/compile_commands.json`（最後のフラグ優先、`CMakeCache.txt` フォールバックおよび `vehicle_sil_bench_ecu --bench-build-info` との突き合わせ）から実際のコンパイラと最適化・サニタイザフラグを抽出します。
- **Control トランスポートメトリクス連携:** `--control-metrics auto`（既定）は `vehicle_sil_launch version --json` の `"control-metrics"` 機能（`formats.control_metrics == 1`）を確認します。未対応ランチャーでは `status: "unavailable"`、`unavailable_reason: "launcher_feature_unsupported"` を記録します。必須にする場合は `--control-metrics on`（未対応なら終了コード `2`）、無効化する場合は `--control-metrics off`（`status: "not_requested"`）を指定します。
- **CI 統合:** 通常 CI は `benchmark.smoke_determinism`（ラベル `integration;benchmark`、絶対速度しきい値なし）を Linux、TSan、ASan、Windows で実行します。Release ベンチマーク結果の手動収集には `.github/workflows/benchmark.yml`（`workflow_dispatch` 専用）を使用します。
