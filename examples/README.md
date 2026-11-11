# サンプル

やりたいこと別のサンプルです。上から順に読むと、使う層が少しずつ下がります。getting_started、typed_messages、pid_stream、fault_injection、vehicle、low_level は、引数なしで動きます。結果を数行で表示し、期待どおりなら 0、違えば 1 で終了します。CTest にも入っています（ラベル `examples`）。ecu_pair は結果を検査しません。control_demo は止めるまで動き続けます。

実行ファイルはビルドツリーの `examples/` 以下にあります（例: `build/linux-gcc-release/examples/getting_started/vehicle_sil_getting_started`）。

| やりたいこと | 場所と実行ファイル | 読むとき |
|---|---|---|
| 最初の ECU を動かす（送る、受ける） | [getting_started](getting_started) `vehicle_sil_getting_started` | 最初に読みます。`Ecu&` を受け取るクラスで ECU を書き、`Simulation` で仮想バスか SIL Kit に載せます（`--silkit`）。 |
| ECU アプリの部品を使う（Request、監視、識別情報） | [j1939/ecu_pair](j1939/ecu_pair) `vehicle_sil_j1939_ecu_pair` | 部品（`PeriodicSender`、`Requester`、`Monitor` など）で ECU の振る舞いを組み立てるときに読みます。既定は SIL Kit で、`--local` を付けると仮想バスで動きます。 |
| メッセージを YAML で定義して使う | [typed_messages](typed_messages) `vehicle_sil_typed_messages` | ビット位置を書かずに信号を扱いたいとき。スケール、符号、列挙、特別な値、9 バイト以上のメッセージを使います。 |
| 独自のプロトコルを作る（問い合わせを受けて値を定期的に返す） | [pid_stream](pid_stream) `vehicle_sil_pid_stream` | 部品を自分で書く最初の手本です。要求を受けて要求元へ周期的に返す部品と、その使い方を示します。バイト列の形式は差し替えられます。 |
| 独自のプロトコルを作る（セッション、書き込み、長いデータ） | [j1939/param_service](j1939/param_service) `vehicle_sil_j1939_param_service` | 上の例より大きいプロトコル（セッション、再試行、ブロックの転送）を作るとき。 |
| 故障を入れて、通信を観測して試験する | [fault_injection](fault_injection) `vehicle_sil_fault_injection` | 通信断、アドレスの競合、転送の途中の欠落、生フレームの注入を、仮想バスで再現して確かめるとき。 |
| 複数の ECU、複数のネットワーク、LIN を 1 つのシミュレーションにする | [vehicle](vehicle) `vehicle_sil_vehicle` | 2 つの CAN と 1 つの LIN にまたがる構成を作るとき。LIN の Master と Slave も `Ecu&` のクラスの中にあります。エンジンの ECU は外から読み書きする変数を宣言し、`main()` が時刻を指定して書き込みます。既定は SIL Kit で、`--local` を付けると仮想バス（CAN と LIN）で同じ結果になります。 |
| 外から操作する（Control API） | [control](control) `vehicle_sil_control_demo` | 実行中のシミュレーションを Control API で操作するとき。ECU のクラス（`control_apps.hpp`）が変数を宣言し、`AgentClient` が ECU の Agent を Control Server につなぎます。 |
| 層を自分で組み立てる（バス、ホスト、ドライバー、ネットワーク） | [low_level](low_level) `vehicle_sil_low_level` | 新しいバックエンドや試験の土台を作るとき。通常は `Simulation` を使います。 |

## 結合試験の駆動役

次の実行ファイルは、結合試験が別プロセスとして起動する相手です。入門用ではありません。

- [j1939/sender](j1939/sender)、[j1939/receiver](j1939/receiver): J1939 スタックを実時間の `Peer` で動かす送信側と、SIL Kit の CAN で直接受けて照合する受信側。
- [lin](lin): LIN の Master と Slave のピア（`Peer` を使わず、`ParticipantRuntime` と `SilKitLinPort` で組み立てる）。
- [virtual_time](virtual_time): 仮想時間のピア（物理モデル、J1939 ECU、LIN センサー、ボディコントローラー）。

## 外部の CMake プロジェクトから使う

- [fetchcontent_consumer](fetchcontent_consumer): FetchContent で取り込む外部プロジェクトの例。別のビルドとして構成して試験します（ラベル `fetchcontent`）。
