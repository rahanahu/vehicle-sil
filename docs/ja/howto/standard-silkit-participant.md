# 標準の SIL Kit participant をつなぐ

[English](../../howto/standard-silkit-participant.md)

vehicle-sil のヘッダーやライブラリを一切使わず、標準の [Vector SIL Kit](https://github.com/vectorgrp/sil-kit) API だけで書いた独立した C++ participant をビルドし、`vehicle_sil_launch` で vehicle-sil の ECU と一緒に動かす手順です。

動かせるサンプルは [examples/standard_silkit_participant/](../../../examples/standard_silkit_participant/) にあります。

## サンプルの構成

| ファイル | 内容 |
|---|---|
| `CMakeLists.txt` | `SilKit::SilKit` だけをリンクする独立した CMake プロジェクト（`find_package(SilKit REQUIRED)`）です。vehicle-sil のリポジトリ内ビルドでは既存の `SilKit::SilKit` ターゲットを再利用し、外部 SDK のダウンロードを不要にします。 |
| `main.cpp` | `#include <silkit/...>` だけを使う最小の C++17 coordinated SIL Kit participant です。CAN フレームを送受信し、終了時に participant を破棄してから最終カウンタを集計します。 |
| `rx_inbox.hpp` | 受信フレームをバッファリングし、`tx_ns < now_ns` だけを取り出して（`tx_ns >= now_ns` は次ステップ向けに保持）正規順序へソートする純粋な C++17 受信キューです。 |
| `simulation.yaml` | `Engine`（`examples/ecu_main`、周期 10 ms）と `StandardPeer`（`standard_silkit_participant`、周期 25 ms）を `J1939_CAN` 上で組み合わせるランチャー設定ファイルです。 |

## 公式 Vector SIL Kit SDK でビルドする

公式の Vector SIL Kit リリースアーカイブ（例: `SilKit-5.0.8-ubuntu-24.04-x86_64-gcc.zip`）を展開し、`include/`、`lib/`、`lib/cmake/SilKit/SilKitConfig.cmake` を含む `SilKit/` ディレクトリを用意します。

vehicle-sil のビルドツリーの外で、展開した SDK を指定してサンプルを configure・ビルドします。

```sh
cmake -S examples/standard_silkit_participant -B /tmp/build-std-peer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/SilKit-5.0.8-ubuntu-24.04-x86_64-gcc/SilKit
cmake --build /tmp/build-std-peer
```

`-DCMAKE_PREFIX_PATH` の代わりに `-DSilKit_DIR=/path/to/SilKit/lib/cmake/SilKit` を指定しても構いません。生成された実行ファイルは `libSilKit.so`（Windows では `SilKit.dll`）とシステムの C++ ランタイムだけに依存し、`vehicle_sil/` のヘッダーやライブラリは一切含みません。

vehicle-sil リポジトリ自体をビルドするとき（既定の `VEHICLE_SIL_BUILD_EXAMPLES=ON`）は、同じサンプルがリポジトリの `SilKit::SilKit` ターゲットを使って `build/<preset>/examples/standard_silkit_participant/standard_silkit_participant` にもビルドされます。

## `vehicle_sil_launch` の設定

標準 participant は `simulation.yaml` で `describe: false` にし、`{name}` と `{registry_uri}` のプレースホルダーで participant 名と registry URI を渡します。

```yaml
vehicle_sil_simulation: 2

time_step_ms: 5
networks:
  - name: J1939_CAN
    kind: can
participants:
  - name: Engine
    command: [../ecu_main/engine/vehicle_sil_engine_ecu]
    step_ms: 10
  - name: StandardPeer
    describe: false
    command:
      - ./standard_silkit_participant
      - --name
      - "{name}"
      - --registry-uri
      - "{registry_uri}"
      - --network
      - J1939_CAN
      - --step-ms
      - "25"
      - --tx-id
      - "0x200"
      - --tx-period-ms
      - "25"
```

設定のポイントです（詳細は [simulation-config.md](../simulation-config.md) を参照）。

| 設定 | 必要な理由 |
|---|---|
| `describe: false` | 標準の SIL Kit participant は vehicle-sil 独自の `--describe` オプションを持ちません。`vehicle_sil_launch check` は実行ファイルの存在だけを確認して（外部ホスト向けに `manual: true` を併用している場合を除く）`--describe` を省き、注記として `not-checked`（そのネットワーク上の `describe: true` の ECU が 1 つだけのときは `single-participant` も）を出して終了コード 0 で通ります。 |
| `manual: true`（外部ホストで動かす場合） | 標準の participant を別の PC で起動する場合（例: Windows のランチャーに Linux から接続する構成）は、`describe: false` と `manual: true` を両方指定します。`command` を省略する（またはランチャー側にローカル実行ファイルがないリモート用コマンドを書く）ことができ、非ループバックの `registry_uri`（例: `silkit://0.0.0.0:8500`）を設定したうえで、リモート側からランチャーのネットワーク URI へ接続して起動します。 |
| `"{name}"` と `"{registry_uri}"` | `describe: false` の participant には、ランチャーが `--name` や `--registry-uri` を自動付与しません。プレースホルダーで設定上の participant 名と実行時の registry URI を渡します。YAML が `{...}` をマッピングとして解釈しないよう、必ず引用符で囲みます。 |
| `required: true`（既定） | coordinated な participant は `required: true` のままにし、SIL Kit のシステムコントローラーが参加を待ってから仮想時間を開始するようにします。 |

`simulation.yaml` のあるディレクトリ（例: `build/linux-gcc-release/examples/standard_silkit_participant`）で `check` と `run` を実行します。

```sh
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml --speed max --stop-at-ms 500
```

公式 SDK で独立ビルドしたバイナリを vehicle-sil の ECU と組み合わせて動かす場合は、`build/<preset>/examples/standard_silkit_participant/` に置いた `simulation.yaml` のコピーで `./standard_silkit_participant` を `/tmp/build-std-peer/standard_silkit_participant` に置き換えます（`/tmp/build-std-peer/simulation.yaml` を使う場合は、`Engine` のパスもビルド済み `vehicle_sil_engine_ecu` に合わせて更新してください）。

## 異周期構成では `time_step_ms` を明示する

すべての participant が `describe: true` のとき、`vehicle_sil_launch` は各 ECU の `--describe` から周期を読み取り、`10` ms、有効な ECU 周期、`--stop-at-ms` の最大公約数（GCD）で `time_step_ms` を自動決定します。

有効な participant に `describe: false` が 1 つでも含まれると、ランチャーはその周期を `--describe` から取得できないため、自動決定は **`time_step_ms: 1`** にフォールバックします。

異周期構成（例: 周期 `10` ms の vehicle-sil ECU と周期 `25` ms の標準 SIL Kit participant）では、全 participant のステップ周期と終了時刻の最大公約数を `time_step_ms` に明示してください。

- `10` ms と `25` ms、`--stop-at-ms 500` の構成では `gcd(10, 25, 500) = 5` なので `time_step_ms: 5` と書きます。
- これにより、`--speed max` 実行時の不要な 1 ms 刻みのコントローラーステップを避け、各 participant のステップ境界をコントローラーの格子に揃えられます。
- `--stop-at-ms` は `time_step_ms` の倍数にしてください。倍数でない場合は `check` と `run` が `stop-not-on-step` を報告します。

## 同時刻タイムスタンプを決定論的に処理する

周期の異なる 2 つの participant（例: `10` ms と `25` ms）では、最小公倍数ごとの時刻（`t = 0, 50, 100, 150, ...` ms）にステップが重なります。同時刻 `T` に相手 A が送った CAN フレームは、IPC のスケジューリングや速度設定（`--speed 1` か `--speed max` か）によって、相手 B の `SetSimulationStepHandler(T)` の実行前に `AddFrameHandler` へ届くことも、実行後に届くこともあります。

実行回数や速度設定にかかわらずビット単位で再現させるため、次のルールで処理します。

1. `AddFrameHandler` ではシミュレーション状態を直接書き換えず、mutex で保護した受信キューへフレームを退避します。
2. `SetSimulationStepHandler(now, duration)` では受信キューを振り分けます。
   - 現在ステップより**厳密に前**に送信されたフレーム（`frame_timestamp < now`）だけをこのステップで処理します。
   - `frame_timestamp >= now` のフレームは**破棄せず**、次ステップ向けにキューへ残します。
3. 取り出したフレームは処理前に正規順序（例: `(timestamp, can_id, flags, length, payload)`）でソートします。

[examples/standard_silkit_participant/rx_inbox.hpp](../../../examples/standard_silkit_participant/rx_inbox.hpp)（[examples/standard_silkit_participant/main.cpp](../../../examples/standard_silkit_participant/main.cpp) から使用）のサンプルはこのパターンを実装しています。

```cpp
std::vector<RxRecord> ready;
{
    std::lock_guard<std::mutex> lock(rx_mutex);
    std::vector<RxRecord> deferred;
    deferred.reserve(rx_inbox.size());
    for (const auto& rec : rx_inbox)
    {
        if (rec.tx_ns < now_ns)
        {
            ready.push_back(rec);
        }
        else
        {
            deferred.push_back(rec);
        }
    }
    rx_inbox.swap(deferred);
}
```

このルールにより、`10` ms 周期の ECU が `t = 50` ms に送ったフレームは、`25` ms 周期の participant の `t = 50` ms ステップでは常に保留され、次の `t = 75` ms ステップで決定論的に処理されます。その結果、`--speed 1` と `--speed max` で同一の CAN トレースが得られます。

## 次回の Run では participant を再起動する

coordinated な SIL Kit participant は、1 回のシミュレーション Run につき 1 つのライフサイクル（`StartLifecycle()` から `ParticipantState::Shutdown` まで）を実行します。

- **`vehicle_sil_launch` が起動を管理する場合（`manual: false`、既定）:** `vehicle_sil_launch run` を再実行するたび、また常駐 `vehicle_sil_launch serve` で次の Run を開始するたびに、新しい participant プロセスが起動され終了時にクリーンに停止します。そのため各 Run は仮想時刻 `0` から始まり、前回の状態や受信フレームは残りません。
- **手動起動する場合（`manual: true`）:** 前回の Run が終了した後、次回の Run ごとに標準 participant の新しいプロセスを起動してください（[participant を自分で起動する](../simulation-config.md#participant-を自分で起動するmanual--manual)）。

複数の Run にまたがって外部 participant を常駐させたり、開始時刻 `t = 0` が揃わない外部 participant を混在させたりする挙動はサポート契約に含まれません。Run ごとに新しい coordinated participant を起動してください。

## 検証範囲（CI と公式 Vector SIL Kit SDK）

| 環境 | 使用する SIL Kit ライブラリ | 検証内容 |
|---|---|---|
| リポジトリ CI（`ctest -R '^standard_silkit_participant\.'`） | リポジトリ内の `SilKit::SilKit` ターゲット（CI での外部 SDK ダウンロードなし） | サンプルに `vehicle_sil/` の include がないこと（`no_vehicle_sil_includes`）、`DeterministicRxInbox` の厳密な同時刻保留・等時刻保持・正規ソートの単体テスト（`rx_inbox_*`）、`10` ms 対 `25` ms の異周期双方向 CAN と `--speed max` での厳密な仮想時刻因果律（`asymmetric_bidirectional_max`、実時間の速度倍率アサーションなし）、`--speed max` と `--speed 1` のバイト単位トレース一致（`simultaneous_timestamp_determinism`）、別タグによる `t = 0` からの Run 間分離（`cross_run_isolation`）、同梱 `simulation.yaml` の `check`・`run` および `Engine` -> `StandardPeer` の CAN 受信（`example_config_smoke`）。 |
| Fedora 44 でのローカル検証 | 公式 Vector SIL Kit v5.0.8 SDK（`SilKit-5.0.8-ubuntu-24.04-x86_64-gcc.zip`） | 公式 SDK のヘッダーと `libSilKit.so` に対する `examples/standard_silkit_participant` の独立した `find_package(SilKit REQUIRED)` CMake 構成・ビルドと、vehicle-sil 実行ファイルとの相互接続実行。 |
