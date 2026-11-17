# CAN・イーサネット・LIN のバス

[English](../code-map-buses.md)

この分野を変更するときの地図です。全体の入口は [code-map.md](code-map.md)、テストの共通ルールは [code-map-testing.md](code-map-testing.md) にあります。

関連する仕様と変更範囲: [internals/can.md](internals/can.md) / [internals/ethernet.md](internals/ethernet.md) / [internals/lin.md](internals/lin.md)

## テストのラベル

### `can`

`unit` ラベルも付きます。

| テスト | 対象 |
|---|---|
| `can.*` | `vehicle_sil::can` の単体テスト（SIL Kit なしでビルドする） |
| `can_silkit.*` | SIL Kit のポート |
| `can_netsim.*` | 簡易 CAN Network Simulator |

確認すること:

- フレームの検証
- 仮想バスの受信順序とあふれ
- ホストの段階の順序と失敗、ステップドライバーの順序
- 送信時間と調停（11 ビット / 29 ビット）、混雑、送信キュー
- 受信側のステップ幅によらないこと
- ポートのイベント（送信の結果、受信キューのあふれ、バス状態、エラーフレーム）
- 故障注入と、ECU 単位の通信断
- `post()`、時間の領域の混在の拒否
- 実時間のドライバー（差し替えた時計での決定的なテストと、壁時計での到着と順序）

簡易 CAN Network Simulator では、プロセス内の participant と実行ファイルの両方で、次を確認します。

- 仮想バスと同じ受信の時刻になること
- 繰り返しても同じ結果になること
- 送信時刻のない送信者
- コントローラーのモード（始めていないコントローラーの送信を捨てること、`Reset()` での送信の取り消し、止めたコントローラーへの配送）
- 設定ファイルの読み込み

プロセス内のレジストリでは、次を確認します。

- 同じシナリオのトレースが、仮想バスと SIL Kit で一致すること（ステップ幅の組み合わせを変えて）
- 送信の続く中で、participant を決まった順序で止めて破棄できること

### `eth`

`unit` ラベルも付きます。

| テスト | 対象 |
|---|---|
| `eth.*` | `vehicle_sil::eth` の単体テスト（SIL Kit なしでビルドする） |
| `eth_silkit.*` | SIL Kit のポート |
| `eth.steps_do_not_allocate` | ステップの中でメモリを確保しないこと（専用の実行ファイル） |
| `eth.boundary` | `libs/eth` が SIL Kit のヘッダーを直接にも間接にも含まないこと |

確認すること:

- フレームと MAC アドレスの型
- 仮想ネットワーク: ハブとしてのフレームの配信、次のステップで受信されること、短いフレームの埋め、自分のフレームを受信しないこと、時刻の順、あふれ
- チャネル: `step::Host` と `StepDriver` の上で動くこと、ゲートウェイ、同じ時刻のフレームを送信元のアドレスで並べること
- チャネル: `on_frame()` と `receive()`、送信の段階、上限、絞り込み、ポートのイベント、選択肢のエラー
- 偽のコントローラーに対するポート（時刻同期していない送信側のフレームの時刻、あふれた送信の確認を含む）
- プロセス内のレジストリで、同じシナリオの記録が仮想ネットワークと SIL Kit で一致すること（ステップ幅の組み合わせを変えて）

最後の項目は、3 つの送信側が同じ時刻に送信する場合を含みます。アドレスを登録の順とずらし、仮想ネットワークの側でもアドレスの順を確認します。

### `lin` (単体テスト)

仮想時間の単体テストです。豊富な LIN の検査はプロセス内のレジストリを使うテストにもあり、`unit` ラベルも付きます。

- LIN のスケジュール、応答、Master のフレームの受信、休止と起床、スケジュールの切り替え

| LIN のテスト | 内容 |
|---|---|
| `lin_local.*` | 主なシナリオを、仮想 LIN バスの上でも確認する |
| `lin_bus.*` | 仮想 LIN バスだけのテスト |
| `lin.boundary` | `vehicle_sil::lin` が SIL Kit を含まないことの検査 |

### `lin`（`lin_cluster.*`）

registry、controller、LIN の Master と 2 つの Slave（5 / 2 / 4 ms）を別プロセスで動かします。`integration` ラベルも付きます。

- トレースを照合して、スケジュール、両方向のフレーム、Master・Slave・bus idle による休止と起床を確認します。
- Slave の 1 つを実時間に結合しても、トレースが一致することを確認します。

## ディレクトリごとのコードとテスト

### `libs/can`

| 名前 | 場所 |
|---|---|
| `Frame` | `can/frame.hpp` |
| `Port` | `can/port.hpp` |
| `Host`、`Steppable` | `can/host.hpp` |
| `StepDriver`、`RealTimeDriver`、時計 | `can/step_driver.hpp`、`can/real_time_driver.hpp`、`can/real_time_clock.hpp` |
| `RealTimeReceiver` | `can/real_time_receiver.hpp` |
| `VirtualCanBus` | `can/virtual_can_bus.hpp` |
| `CanBusModel` | `can/bus_model.hpp` |
| `TapPort` | `can/tap_port.hpp` |
| `RawChannel` | `can/raw_channel.hpp` |

`Host` は `step::Host` の上にあります。`StepDriver`、`RealTimeDriver`、時計は、`vehicle_sil::step` のものの別名です。

- テスト: `tests/can`
- `ctest -R`: `^can\.`

### `libs/can_silkit`

| 名前 | 場所 |
|---|---|
| `SilKitCanPort`、`bind_host()` の別名 | `can_silkit/silkit_can_port.hpp` |

`bind_host()` の本体は `core/bind_host.hpp` です。

- テスト: `tests/can`
- `ctest -R`: `^can_silkit\.`

### `libs/can_netsim`、`tools/can_bus_simulator`

| 名前 | 場所 |
|---|---|
| `CanBusSimulator` | `can_netsim/can_bus_simulator.hpp` |
| `NetworkSimulatorConfig` | `can_netsim/network_simulator_config.hpp` |

- テスト: `tests/can`
- `ctest -R`: `^can_netsim\.`

### `libs/eth`、`libs/eth_silkit`

| 名前 | 場所 |
|---|---|
| `Frame`、`MacAddress` | `eth/frame.hpp` |
| `Port`、`PortEvent` | `eth/port.hpp` |
| `Channel` | `eth/channel.hpp` |
| `VirtualEthernetBus` | `eth/virtual_ethernet_bus.hpp` |
| `SilKitEthPort` | `eth_silkit/silkit_eth_port.hpp` |

- テスト: `tests/eth`
- `ctest -R`: `^eth\.`、`^eth_silkit\.`

### `libs/lin`、`libs/lin_silkit`

| 名前 | 場所 |
|---|---|
| `LinMaster` | `lin/lin_master.hpp` |
| `LinSlave` | `lin/lin_slave.hpp` |
| `LinPort` | `lin/lin_port.hpp` |
| `VirtualLinBus` | `lin/virtual_lin_bus.hpp` |
| `SilKitLinPort` | `lin_silkit/silkit_lin_port.hpp` |

- テスト: `tests/lin`
- `ctest -R`: `^lin_bus\.`（仮想 LIN バス）、`^lin_local\.`（`LocalBench` の上）、`^lin\.`（SIL Kit の上）、`^lin_cluster\.`（`examples/lin` のプロセス）
