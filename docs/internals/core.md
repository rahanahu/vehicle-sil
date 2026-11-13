# participant の土台（`vehicle_sil::core`）

vehicle-sil 自体を直す人と、`ParticipantRuntime` を直接使う人向けです。SIL Kit の participant と lifecycle を持つ `ParticipantRuntime` の契約、所有と終了の順序、スレッドとキュー、実時間モードの時刻を扱います。仮想時間の進み方と時間ゲートは [virtual-time.md](virtual-time.md) にあります。

## 公開 API の契約

- `ParticipantRuntime` のコンストラクタはレジストリに接続します。失敗すると `SilKit::SilKitError` を投げます。
- バスのコントローラは `start()` の前に作ります。`start()` が戻った時点で、接続済みの相手はそのコントローラの購読を確認済みです。
- `start(timeout)` は lifecycle が Running になるまで待ちます。タイムアウトや失敗のときは `std::runtime_error` を投げます。
- `set_communication_ready_handler_async(handler)` と `complete_communication_ready()`: ハンドラーが戻った後も、`complete_communication_ready()` を呼ぶまで、participant は最初のステップの前（CommunicationInitialized）にとどまります。Coordinated では全員がそこで待つので、その間に publish/subscribe でやりとりできます（ハンドラーの中ではできません）。完了は、ハンドラーが起こした別のスレッドから呼びます（ハンドラーの中からは呼べません。ハンドラーが呼ばれる前と、2 回目は何もせずに偽を返します）。完了するまで誰のステップも始まらないので、窓の用が失敗したときも必ず完了させます。`SimulationController::set_start_window()` がこれを使います（[control.md](control.md) の「SIL Kit の上の運び手」）。
- `set_peer_status_observer(observer)`: ほかの participant（と自分）のライフサイクルの状態がわかるたびに、名前と状態を渡して呼びます。呼ぶのは、それを知ったスレッド（SIL Kit のスレッドか、状態を読み直すスレッド）で、1 つずつです。同じ状態でもう一度呼ぶことがあります。短く、`peer_status()` のほかはこの runtime を呼ばないこと。`start()` の前に登録します。
- `stop()` は何度呼んでもよく、例外を投げません。
- participant が失敗すると（ステップのハンドラ、実時間のドライバー、`report_failure()`）、自分の `stop()` と `request_stop()` は、Error になるまで lifecycle に停止を渡しません（先にふつうの停止が渡ると、Coordinated ではシミュレーション全体が正常に止まり、controller から失敗が見えないためです）。失敗の印を立てることと SIL Kit に Error を報告することは 1 つのロックの下で行うので、失敗が見えた後の停止が報告を追い越すことはありません。SIL Kit が後回しにした処理を実行する順序には頼りません。`stop()` の待ちは Error と Shutdown を合わせて `shutdown_timeout` までで、過ぎれば Error の前でも停止を渡します。次の場合は Error を経ないか、Stopping の後で Error になります。失敗より前に届いた `request_stop()`（失敗するステップのハンドラが始まる前、`report_failure()` の前）は、ふつうの停止です。controller の中断やほかの participant の停止が先に届いたときも同じです。`report_failure()` は、`start()` が lifecycle を始めるまでは何もしません。
- `wait_for_peer()` は、相手の状態が条件を満たすまで待ちます。待っている間は、SIL Kit から状態を定期的に読み直します（[docs/dependencies.md](../dependencies.md) の既知の不具合を参照）。SIL Kit のハンドラの中からは呼べません。
- コントローラを参照するもの（`SilKitCanPort`、`SilKitLinPort`）はコントローラを所有しません。participant より先に閉じるか破棄します。`LinMaster` と `LinSlave` はポートを所有しないので、ポートより先に破棄します。
- J1939 スタックはプロセス全体の状態を持ちません。1 つのプロセスに、いくつでも `Network` と `Peer` を置けます。

## 所有権と終了順序

```
ParticipantRuntime ──owns──> SilKit::IParticipant ──owns──> ICanController、ILinController
SilKitCanPort、SilKitLinPort ──refers to──> コントローラ（所有しない）
LinMaster、LinSlave ──refers to──> LinPort（所有しない）
Host ──refers to──> ポートと Network（所有しない）
```

`Peer` は次の順で終了します（自分で組み立てるときも同じ順にします）。

1. ステップを止める（実時間では `RealTimeDriver`、続いて lifecycle。仮想時間では lifecycle）。
2. SIL Kit のポートを閉じる（ハンドラを外し、コントローラを止める）。
3. LIN のノード（それぞれのポートより先）、`Network`（部品が先）、ポート、ホストを破棄する。
4. participant を破棄する。

## スレッドとキュー

| スレッド | 所属 | 処理 |
|---|---|---|
| SIL Kit I/O | SIL Kit | `SilKitCanPort` の受信ハンドラがフレームに時刻を付けて受け箱に積む。満杯なら捨てて `receive_overflows` に数え、`ReceiveOverflow` の事象にする。仮想時間では、ステップハンドラもこのスレッドで呼ばれる |
| ドライバー | vehicle-sil（実時間のみ） | `RealTimeDriver` がホストのステップ（受信、プロトコル処理、アプリ、送信）を回す |

プロトコル処理（`Network`、部品、ハンドラ）はロックを持たず、ステップを回すスレッドだけが触ります。仮想時間では SIL Kit のステップハンドラ、実時間ではドライバーのスレッドです。ほかのスレッドからは、ホストの `post()` で処理を渡します。

キューの容量は、ホストの `post()` の待ち行列を除いて固定です。生成時に確保します。`post()` の待ち行列は上限がなく、呼ぶたびに確保します（外部からの操作のための口で、ステップごとに使うものではありません）。

| キュー | 既定の容量 | 備考 |
|---|---|---|
| `SilKitCanPort` の受け箱 | 1024 フレーム | `SilKitCanPortOptions::inbox_capacity` |
| `SilKitCanPort` の事象の受け箱 | 1024 | `SilKitCanPortOptions::event_capacity` |
| `Network` の 1 ステップの受信数 | 1024 フレーム | `NetworkOptions::receive_capacity`。ほかの上限は [j1939.md](j1939.md) の「ステップ幅と資源の上限」 |
| サンプル Receiver の受信箱 | 256 | |

## 時刻（実時間モード）

`ParticipantRuntimeOptions::virtual_time`（`PeerOptions::virtual_time`）を指定しない場合（J1939 の最小ピアと Vector XL CAN Bridge）は、SIL Kit の時刻同期を使いません。autonomous lifecycle で、すべて実時間で動きます。

- J1939 のタイマー（Address Claim の後の待ちなど）は、ドライバーの時計（`RealTimeClock`）で進みます。
- 受信フレームの時刻は、ポートが時計で付けた到着時刻です。時刻同期をしない participant の SIL Kit のタイムスタンプは使いません（[can.md](can.md) の「実時間での駆動」）。
