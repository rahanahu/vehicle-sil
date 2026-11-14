# Vector XL CAN Bridge

実機の CAN（Vector の XL Driver Library で使える機器と Vector Virtual CAN）につなぐ人と、ブリッジを直す人向けです。前に読む文書はありません。ブリッジを直す人は、次に [code-map.md](code-map.md) の「Vector XL CAN Bridge」を読みます。

イーサネットのブリッジ（`vehicle_sil_ethernet_bridge`）は [ethernet-bridge.md](ethernet-bridge.md) にあります。

`vehicle_sil_vector_xl_bridge` は、SIL Kit の CAN ネットワークと Vector XL の CAN チャネルの間で、クラシック CAN のフレームを双方向に転送します。XL のチャネルには、Vector Virtual CAN と、CANcase や VN16xx などの物理 CAN を使えます。

> [!WARNING]
> Windows 11 の MSYS2 UCRT64 ビルドと、実際の XL Driver Library を使った Vector Virtual CAN の送受信を確認済みです。VN5620 の 2 つのチャネルをつないだ物理ループバックでも、送受信を確認しています。他の物理 CAN 構成や機器は未確認です。

```
SIL Kit CAN ネットワーク ── ICanController ── CanBridge ── ExternalCanPort ── XlCanPort ── XL Driver ── Virtual CAN / CANcase
                                 （participant は ParticipantRuntime が所有）
```

| クラス | 責務 |
|---|---|
| `vehicle_sil::vector_xl::CanBridge` | SIL Kit の `ICanController` と `ExternalCanPort` の間で、クラシック CAN を双方向に転送する。プラットフォームに依存しない |
| `vehicle_sil::vector_xl::ExternalCanPort` | 外部 CAN のインターフェース。本番の実装は `XlCanPort`、テストではフェイク |
| `vehicle_sil::vector_xl::XlDriver` | `xlOpenDriver` / `xlCloseDriver` を RAII で管理し、`xlGetDriverConfig` からチャネルを列挙する |
| `vehicle_sil::vector_xl::XlCanPort` | XL のチャネル 1 つを開き（`xlOpenPort`）、送信（`xlCanTransmit`）と受信スレッド（`xlReceive` と通知イベント）を担う |
| `vehicle_sil::vector_xl::ExternalPortAdapter` | `ExternalCanPort` を CAN の共通基盤のポート（`RealTime`）にする。プロトコルスタックを XL のチャネルに直接載せるときに使う。プラットフォームに依存しない |

## 使い方

```sh
# XL ドライバが提供する CAN チャネルを、接続先ごとに番号付きで表示する
vehicle_sil_vector_xl_bridge --list-channels

# Vector Virtual CAN の 1 番目のチャネルにつなぐ
vehicle_sil_vector_xl_bridge --registry-uri silkit://127.0.0.1:8500 --network J1939_CAN1 --xl-target virtual --xl-channel 1

# 物理 CAN（CANcase など）の 1 番目のチャネルに、250 kbit/s でつなぐ
vehicle_sil_vector_xl_bridge --registry-uri silkit://127.0.0.1:8500 --network J1939_CAN1 --xl-target physical --xl-channel 1 --bitrate 250000
```

- Ctrl+C（Windows ではコンソールのウィンドウを閉じても）で止まり、転送の統計を出力します。
- 実行中に CAN インターフェースの USB を抜かないでください。ブリッジは止まりますが（終了コード 1）、VN5620 では挿し直しても認識されず、電源の入れ直しが必要なことがありました。
- 終了コードと引数は「[接続先とチャネルの選択（CLI）](#接続先とチャネルの選択cli)」、統計の意味は「[転送の契約](#転送の契約)」にあります。

## 物理 CAN と仮想時間

- 物理 CAN に接続するときは、原則として実時間で動かします。
- Vector Virtual CAN も、SIL Kit の仮想時間には自動で同期しないものとして扱います。
- Vector XL CAN Bridge は、autonomous lifecycle を使い、実時間で動きます。仮想時間のシミュレーションには参加させません。
- 仮想時間の ECU と同じネットワークにブリッジを置くと、ECU はブリッジが転送したフレームを、届いた時点の自分のステップの時刻で受けます（シミュレーションが始まる前に届いたものは時刻 0）。どのステップで受けるかは壁時計しだいなので、結果は決定的ではありません（[internals/can.md](internals/can.md) の「SIL Kit のポート」）。
- プロトコルスタックを XL のチャネルに直接載せる場合（`ExternalPortAdapter`）も実時間だけです。時刻同期で回すホストに登録すると、構成エラーになります。

## プロトコルスタックから使う（`ExternalPortAdapter`）

- `ExternalPortAdapter(external, clock)` は外部ポートを開始し（バスに参加）、破棄か `close()` で止めます。`RealTimeDriver` と同じ時計を渡し、そのドライバーで回します。時刻同期のホストには登録できません（構成エラー）。
- 受信は外部ポートの受信スレッドで到着時刻を付けて受け箱に積みます（ハードウェアのタイムスタンプは別の時計なので使いません）。リモートフレームは捨てて数えます。自分の送信のレシートは捨てます（同じスタックのノードへの配送はスタックが行います）。エラーフレームは `ErrorFrame`、外部の受信キューのあふれは数の分からない `ReceiveOverflow` の事象です。外部ポートが知らせるバス状態（`ExternalEventKind::BusState`）と `Fault` は使わず、`BusStateChanged` も出しません。
- 送信は `transmit()` の同期の結果をそのまま返します（`Ok` は `Accepted`、`QueueFull` と `Dropped` は `QueueFull`）。その後バスに出たかは知らせません。
- 実機での確認はしていません（「検証の範囲」）。

## 転送の契約

- 識別子（11 bit / 29 bit）、IDE、RTR、DLC、データを変えずに転送します。ただし SIL Kit から XL へは、DLC 9〜15（クラシック CAN ではどれもデータ 8 バイト）を DLC 8 として送ります。`xlCanTransmit` が受け付ける DLC は 0〜8 のためです（XL Driver Library のマニュアル）。XL から SIL Kit へは、受け取った DLC のまま転送します。
- CAN FD、CAN XL、不正な ID や DLC のフレームは転送せず、`silkit_rejected` または `xl_rejected` に数えます。XL 側の ID は受信イベントから 11/29 ビットで取り出すので、`xl_rejected` に数えるのは実際には DLC が 15 を超えるフレームです。
- XL のエラーフレームは転送せず、`error_frames` に数えます。XL の受信キューのあふれは `overruns` に数えます。
- 統計値は終了時に 1 行で出力します。対象は次のとおりです: `silkit_to_xl`、`xl_to_silkit`、`own_tx_dropped`、`error_frames`、`overruns`、`silkit_rejected`、`xl_rejected`、`xl_queue_full`、`xl_tx_failures`、`silkit_tx_failures`、`xl_tx_dropped`、`xl_tx_unsent`。

### バスに出たフレームの数え方（送信レシート）

- `silkit_to_xl` は XL の送信キューに渡した数です。バスに出られないフレーム（ACK を返す相手がいない、ビットレートが違う、ケーブルが抜けた）は、XL の送信キューに残り、バスに出られるようになった時点でまとめて送られます。
- `XlCanPort` は送信レシートを有効にし、渡したのにまだレシートの来ないフレームを数えます。`own_tx_dropped`（自分の送信のレシートの数）が、バスに出た数です。
- レシートを使うのは、ポートがレシートを有効にできたとき（init 権限があるとき）か、レシートが 1 つでも届いた後です。init 権限がなく、ほかのアプリケーションがレシートを無効にしているチャネルでは、止めるときの待ちは働かず、`xl_tx_unsent` は 0 のままです。`--tx-timeout-ms` は init 権限があるときだけ使います（下記）。
- 止めるとき（`XlCanPort::stop()`）は、チャネルを無効にする前に、渡したフレームがバスに出るのを最長 1 秒待ちます（`XlCanPortOptions::drain_timeout`）。それでも出なかった数を `xl_tx_unsent` に数えます。チャネルを無効にすると、XL の送信キューに残ったフレームは捨てられるためです。相手のブリッジも同時に止まると ACK が返らなくなるので、待っても出ないことがあります。
- `--tx-timeout-ms <ms>`（100〜60000、`XlCanPortOptions::transmit_timeout`）を指定すると、最後の送信レシートの後に渡したフレームのどれにも、その時間レシートが来ないあいだ（バスが詰まっているあいだ）、新しいフレームを XL に渡さずに捨て、`xl_tx_dropped` に数えます（`TransmitResult::Dropped`）。バスが直ったときにまとめて流れる古いフレームを、その時間ぶんと下の試しのフレームに抑えます。指定しなければ、フレームは何秒でも待ってから送られます。
  - 詰まっているかは、レシートが来た時刻だけで決めます。待っているフレームの数は使いません。数はレシートが受信キューのあふれで失われるとずれますが（あふれたら数を 0 に戻し、詰まりの判定もやり直します）、次のフレームのレシートが来れば詰まりは解けます。
  - 詰まっているあいだも、ときどき 1 フレームだけ試しに XL に渡します。レシートが失われた場合や、ドライバが待っているフレームを捨てた場合（バスオフの後など）に、レシートが二度と来なくなるのを避けるためです。最初の試しは、詰まったと判断してからその時間の後です（コントローラーがエラーワーニング、エラーパッシブ、バスオフを知らせているときは 1 秒以上後）。試すたびに間隔を倍にし、5 秒（その時間のほうが長ければその時間）で止めます。試しのフレームも、バスが直るまでドライバのキューで待ってから送られるので、長く詰まるほどたまらないようにするためです。D 秒詰まったときにたまる試しのフレームは、およそ D / 5 個と数個です。
  - レシートに頼るので、init 権限（`--bitrate`）があるときだけ使います。ポートがレシートを自分で有効にできるからです。init 権限がなければ、ほかのアプリケーションがいつでもレシートを無効にでき、詰まりが解けなくなるので、使わずに起動時に `warning:` を出します。
- XL の送信キューからフレームを消す `xlCanFlushTransmitQueue` は使いません。VN5620 では成功を返しても、キューのフレームはその後バスに出ました。
- シミュレーションがバスより速く進んでも、押し戻しはありません。CAN の帯域を超える速さで回すと、フレームは XL の送信キューにたまり、`--tx-timeout-ms` がなければ止めるときに `xl_tx_unsent` になります。

## 実行中のバスの監視

- `XlCanPort` は受信スレッドで、0.5 秒ごとに `xlCanRequestChipState` で CAN コントローラーのエラー状態（エラーアクティブ、エラーワーニング、エラーパッシブ、バスオフと、送受信のエラーカウンター）を問い合わせ、答えを `ExternalEventKind::BusState` で知らせます（`XlCanPortOptions::bus_state_interval`、0 で無効。実行ファイルでは `--no-bus-monitor`）。問い合わせを止めても、ドライバが自分から送る状態は知らせます。VN5620 では、ACK のないバスで問い合わせなしでもエラー状態が届きました。
- 問い合わせの失敗は、送信キューが満杯の場合を除いて、次のように扱います。
  - 一度も受け付けられないうちの失敗: その構成では問い合わせが使えないとみなし、`ExternalEventKind::Warning` を 1 回知らせて問い合わせをやめます。転送は続けます。実行ファイルは `warning: ... the bus is not monitored, ...` を出します。
  - 一度受け付けられた後、3 回続けての失敗（`XlCanPortOptions::bus_state_failures`）: チャネルが使えなくなった（デバイスが外された）とみなし、`ExternalEventKind::Fault` を理由とともに 1 回知らせて受信をやめます。1 回だけの失敗では止めません。
- ブリッジの実行ファイルは 1 秒ごとに状態を見て、コントローラーのエラー状態が変わったとき（起動直後の最初の答えがエラーアクティブの場合を除く）、または問題を表すカウンター（`error_frames`、`overruns`、`silkit_rejected`、`xl_rejected`、`xl_queue_full`、`xl_tx_failures`、`silkit_tx_failures`、`xl_tx_dropped`）が増えたときに、次の形の行を 1 行出します（`format_health_change()`）。
  ```
  [bridge] XL bus: error passive (tx errors 128, rx errors 0); since the last report: error_frames +3650
  ```
- 問題のカウンターが増えた次の回に何も増えなければ、`; no new problems since the last report` を付けた行を 1 回出します。送信だけをするコントローラーは受信に成功するまで受信エラーカウンターが下がらず、直った後もエラーパッシブのままのことがあるので、この行が回復の目印です。
- バスオフからの戻り: VN5620 の XL ドライバは、チャネルを有効にしたままでは、バスが直った後もバスオフから戻りませんでした。そこで `XlCanPort` は、コントローラーがバスオフを 1 秒知らせ続けたら（`XlCanPortOptions::bus_off_restart`、実行ファイルでは `--bus-off-restart-ms`、0 で無効）、チャネルを無効にしてから有効にし直し（`xlDeactivateChannel`、`xlActivateChannel`）、コントローラーを戻します。
  - init 権限（`--bitrate`）があるときだけです。ほかのアプリケーションが設定しているチャネルで、自分のポートを外したときに何が起きるかは分かっていないためです。init 権限がないのにオプションを指定すると、ブリッジは起動時に `warning:` を出します。
  - XL の送信キューで待っていたフレームはチャネルとともに消えるので、`ExternalEventKind::TransmitUnsent` で知らせ、`xl_tx_unsent` に数えます。
  - `ExternalEventKind::Warning` で知らせ、実行ファイルは `warning: XL channel '...' was bus off for <ms> ms; restarted the channel ...` を出します。行を出すのは、バスが最後に動いて（フレームを受けるか、送ったフレームのレシートが来て）から 1、2、4、8 ... 回目の有効にし直しだけです。
  - バスがまだ壊れていれば、フレームを送っているあいだ、コントローラーはまたバスオフになり、また有効にし直します。そのたびに待つ時間を倍にし、10 秒（指定がそれより長ければ指定の時間）で止めます。バスが動けば、元の時間に戻します。
  - 有効にし直せなければ（無効にするか有効にするのに失敗したら）、`TransmitUnsent` の後に `Fault` として受信をやめます。
  - コントローラーの状態に頼るので、`--no-bus-monitor` では、ドライバが自分から状態を知らせない限り、有効にし直しません。
- `Fault` を受けたら、`FAIL: <理由>` を出し、統計を出して終了コード 1 で終わります。
- `ExternalPortAdapter` は `BusState`、`Fault`、`Warning`、`TransmitUnsent` を使いません。チャネルを有効にし直したことも、利用者には知らせません。

## 転送ループの防止

1. SIL Kit 側の受信ハンドラは、RX 方向だけで登録します。ブリッジが SIL Kit に送ったフレームは、ブリッジ自身には届きません。
2. XL 側は、init 権限があるとき（`--bitrate` を指定したとき）、`xlCanSetChannelMode(tx=1, txrq=0)` で送信レシートを有効にし、送信要求の通知を無効にします（「バスに出たフレームの数え方」）。この関数は init 権限を必要とします（`vxlapi.h`）。init 権限がなければ、ドライバの既定（送信レシートあり）のままです。
3. `XL_CAN_MSG_FLAG_TX_COMPLETED` または `XL_CAN_MSG_FLAG_TX_REQUEST` が付いた受信イベントは、自分の送信として捨て、`own_tx_dropped` に数えます。レシートが有効なら、バスに出たフレームと同じ数になります。

同じ SIL Kit ネットワークと同じ XL のバスを、2 つのブリッジでつなぐ構成ではループを防げません。この構成には対応しません。

## キューとスレッド

- ブリッジは独自の転送キューを持ちません。
  - SIL Kit から XL へは、SIL Kit の受信スレッドで `xlCanTransmit` を呼びます。XL の送信キューが満杯ならフレームを捨て、`xl_queue_full` に数えます。
  - XL から SIL Kit へは、`XlCanPort` の受信スレッドで `ICanController::SendFrame` を呼びます。
- XL の受信キューの容量は `--rx-queue-size` で決まります（2 のべき乗、16〜32768、既定 16384）。ドライバが実際に保持できるのは、その値から 1 を引いたイベント数です。
- `xlReceive` は 1 回に 1 イベントずつ呼びます。Vector のサンプルも 1 件ずつ受け取っており、複数件を一度に受け取る使い方は確かめていないためです。
- チャネルは `XL_ACTIVATE_NONE` で有効にします。ブリッジはタイムスタンプを使わず、`XL_ACTIVATE_RESET_CLOCK` は Vector の新しい時刻同期プロトコルと併用できないためです。
- 1 つのポートへの XL の呼び出しは、mutex で直列化します。
- ランチャーは `required: false` のブリッジがシミュレーションに加わるのを待ちません。ブリッジが加わる前に ECU が送ったフレーム（起動直後のアドレスクレームなど）は、物理のバスに届きません。物理のバスの相手と最初からやり取りする試験では、送信を少し遅らせて始めます。
- **起動順**: `CanBridge::start()` は、participant の lifecycle を開始する（`ParticipantRuntime::start()`）前に呼びます。ほかの参加者は、ブリッジが Running になったのを見た時点で送信を始めます。その前にハンドラとコントローラが動いていないと、最初のフレームが失われます。
- 終了は次の順で行います。
  1. SIL Kit のハンドラを解除する（XL へ渡すフレームがなくなる）。
  2. XL の送信を待ち、受信を止める（受信スレッドを join し、チャネルを非アクティブにする）。
  3. SIL Kit のコントローラを止める。
  4. lifecycle を止める。
  5. ポートを閉じる。
  6. ドライバを閉じる。

## 接続先とチャネルの選択（CLI）

```
vehicle_sil_vector_xl_bridge --list-channels
vehicle_sil_vector_xl_bridge --registry-uri <uri> --xl-target <virtual|physical> --xl-channel <n> [--bitrate <bit/s>] [--network <name>] [--participant <name>] [--rx-queue-size <n>] [--tx-timeout-ms <ms>] [--bus-off-restart-ms <ms>] [--no-bus-monitor]
```

- `--xl-target virtual` は Vector Virtual CAN（`XL_HWTYPE_VIRTUAL`）を選びます。`physical` は、それ以外のハードウェアの CAN チャネル（CANcase、VN16xx など）を選びます。
- `--xl-channel` は、そのターゲットの CAN 対応チャネルの中での 1 始まりの番号です。並びはドライバの設定順で、`--list-channels` の表示と同じです。CAN に対応していないチャネル（LIN 専用など）は数えません。
- 存在しない番号を指定すると、使えるチャネルの一覧を出して終了コード 3 で終わります。
- チャネルとデバイスは、起動のときにこの CLI で決めます。Control API と構成ファイル（`vehicle_sil_launch`）には、チャネルとデバイスを選ぶ項目はありません。ほかの口（Control API、構成ファイル、GUI）に選ぶ手段を足すときは、CLI と同じモデル（`vehicle_sil::vector_xl::XlTarget` とチャネル番号）を使います。
- `--bitrate` は 15000〜1000000 bit/s です（`xlCanSetChannelBitrate` の範囲）。指定するときは、init 権限が必要です。ほかのアプリケーションが init 権限を持っている場合は、黙って別のビットレートで動かずに、起動を失敗させます。指定しなければ、チャネルの設定を変えません。
- 終了コードは次のとおりです: 0（Ctrl+C、Ctrl+Break、SIGTERM、コンソールのウィンドウを閉じたときの正常終了）、1（実行中の失敗。SIL Kit の participant が止まる途中か終わった状態になった（registry の停止や接続断）、起動の途中のまま Running にならない、または XL のチャネルが使えなくなった（デバイスを外した））、3（XL ドライバ、チャネル選択、SIL Kit の失敗）、4（引数の誤り）。
- 実行中の participant の状態は、次のように扱います。
  - 失敗にするのは、止まる途中か終わった状態（Stopping、Stopped、Error、ShuttingDown、Shutdown、Aborting）だけです。`FAIL: SIL Kit participant left Running (state <状態>)`（Running になる前なら `FAIL: SIL Kit participant stopped before it reached Running (state <状態>)`）を出します。
  - 起動の途中の状態（ReadyToRun など）は失敗にしません。監視を始めてから 1 秒たっても Running にならなければ `waiting for the SIL Kit participant to reach Running (state <状態>; at most 15000 ms)` を 1 回出し、15 秒たっても Running にならなければ `FAIL: SIL Kit participant did not reach Running within 15000 ms (state <状態>); its SIL Kit log may say why` を出して終了コード 1 で終わります。
  - Paused は失敗にしません（ブリッジは自分では一時停止しません）。
- Windows でコンソールのウィンドウを閉じたとき（ログオフ、シャットダウンも）は、`SetConsoleCtrlHandler` のハンドラーが停止を求め、統計を出し終わるまで最長 4 秒待ってから、ブリッジの終了コードでプロセスを終えます。Windows はハンドラーが戻るとプロセスを終わらせ、待つのも 5 秒ほどまでだからです。Windows の約 5 秒は変えられないので、統計は SIL Kit の participant を止める前に出し、participant の停止を待つのは最長 2 秒にしています。それでも 4 秒を超えると、Windows がプロセスを終わらせ、終了コードは 0 になりません（統計は出た後です）。
- 止める要求の受け方、監視のループ、止める順（統計を SIL Kit の participant の停止より先に出す）、終了コード、コンソールを閉じたときの 4 秒は、`bridges/common` の `run_bridge()`（`vehicle_sil::bridge_runtime`）にあります。ブリッジの実行ファイルはこれを共有します。

## ビルド

- XL バックエンド（`vehicle_sil::vector_xl`）と実行ファイルは Windows 専用です。Vector の XL Driver Library（`vxlapi.h`、`vxlapi64.lib`）が必要です。
- XL Driver Library は vehicle-sil に含めません。`VEHICLE_SIL_VECTOR_XL_DIR` で場所を指定します。
- 見つからない場合は、ブリッジをビルドせずに続けます。`VEHICLE_SIL_REQUIRE_VECTOR_XL=ON` を指定すると、その場合に構成を失敗させます。
- Linux のビルドに XL Driver Library は不要です。

[XL Driver Library](https://www.vector.com/)（`vxlapi.h` と `vxlapi64.lib`）は Vector Informatik GmbH のソフトウェアで、vehicle-sil には含まれていません。利用者が Vector から入手し、Vector のライセンス条件に従って使います。ビルドでは、その場所を指定します。

```sh
cmake --preset windows-mingw-release -DVEHICLE_SIL_VECTOR_XL_DIR="C:/path/to/XL Driver Library/bin"
cmake --build --preset windows-mingw-release
```


XL Driver Library が見つかれば、`vxlapi64.dll` を実行ファイルの隣にコピーします。

## 検証の範囲

- XL バックエンドは 2 つに分かれます。判断と状態を持つロジック（`xl_driver.cpp`）は、vehicle-sil 独自の内部インターフェース（`src/xl_backend.hpp`）だけに依存し、どのプラットフォームでもビルドします。XL Driver Library を呼ぶのは `xl_backend_vector.cpp` だけで、このファイルが行うのは型と値の変換だけです（Windows で XL Driver Library があるときだけビルドします）。
- 自動テストは、内部インターフェースを実装したテスト専用のフェイク（`tests/vector_xl/fake_xl`）に対して、本番と同じ `xl_driver.cpp` と `main.cpp` を動かします。フェイクは、チャネルごとのバス、送信レシート、init 権限、受信キューのあふれ、エラーフレーム、コントローラーのエラー状態の問い合わせ、デバイスの喪失を模擬します（送信レシートの設定とビットレートは init 権限が必要、送信の DLC は 0〜8、受信キューは指定した容量から 1 を引いた数、受信は 1 回に 1 件）。
- `xl_backend_vector.cpp` の変換（関数、定数、構造体のメンバーの対応）は自動テストの対象外です。公式の XL Driver Library でのビルドとリンクで確かめます。
- Windows 11 の MSYS2 UCRT64 で XL Driver Library を使ったビルドとチャネル列挙を確認しています。Vector Virtual CAN の Channel 1 と 2 は同じ仮想バスにつながり、別々の SIL Kit ネットワーク間での送受信、`--bitrate` による init access、送信レシートの扱いを確認しています。VN5620 の 2 つの物理チャネルをループバック接続した構成で、別々の SIL Kit ネットワーク間での送受信も確認しています。
- VN5620 のループバックで、次の点も確認しています。
  - J1939 の 8 バイトを超えるメッセージ（BAM と RTS/CTS、RTS/CTS は最大の 1785 バイトまで）が、2 つのブリッジと物理のバスを通って、中断や再送要求なしに全部届くこと。CTS の往復がブリッジと物理のバスを通る分、1 件にかかる時間は SIL Kit だけの場合より長くなります。
  - 物理チャネルでも、送信レシートが init 権限の有無によらず返り、`own_tx_dropped` が送った数と同じになること。CAN の帯域を超える速さで回したとき、止める時点で送れなかったフレームが `xl_tx_unsent` に数えられること。
  - ACK を返す相手がいないとき、`--tx-timeout-ms` で新しいフレームを捨て（`xl_tx_dropped`）、XL に渡したフレームは相手が現れた後に全部バスに出ること（`silkit_to_xl` と `own_tx_dropped` が一致）。
  - 同じチャネルを別のアプリケーション（init 権限を持つ別のブリッジ）と使うと、そのアプリケーションが送ったフレームは、ブリッジに通常の受信として届き、SIL Kit へ転送されます。後から `--bitrate` 付きで開いた側は、init 権限がないので起動に失敗します。
  - バスの監視（「実行中のバスの監視」）。ACK を返す相手がいないとき、ビットレートが違うとき、ケーブルを抜いたときに、エラーパッシブとエラーフレームの増加を 1 秒ごとに表示し、直ると「no new problems」を表示します。送信だけをするコントローラーは、受信に成功するまで受信エラーカウンターが下がらないので、直った後もエラーパッシブのままのことがあります。
  - 実行中に USB を抜くと、`xlCanRequestChipState` が `XL_ERR_INVALID_ACCESS` で失敗し続け、ブリッジは終了コード 1 で終わります。ただし VN5620 は、挿し直しても USB のデバイスとして現れないことがあり、電源の入れ直しが必要でした。実行中には USB を抜かないでください。
  - Vector Virtual CAN でも、init 権限のない物理チャネルでも、状態の問い合わせは受け付けられます（`warning:` は出ません）。
  - 終端を外して CAN_H と CAN_L を短絡するとバスオフになり、XL ドライバは、直した後もバスオフのままであること。ブリッジがチャネルを有効にし直すと、直ったバスで転送が戻ること（init 権限ありの場合）。終端を外しただけでは、受信エラーでエラーパッシブになり、バスオフにはなりません。
  - 受信キューのあふれ（受信側のプロセスを止めて起こした）で、受信のフレームは失われ、その後も送信が続くこと。この試験では送信レシートは失われませんでしたが、あふれたときにレシートが失われないとまでは確かめていません。
  - コンソールのウィンドウを閉じたときの統計の出力と終了コード 0、Ctrl+C での停止
  - `windows-mingw-static` のブリッジとランチャーが、`PATH` に MinGW がなくても動くこと（取り込む DLL は Windows、UCRT、`vxlapi64.dll` だけ）
- 次の点は、Vector のドライバとハードウェアでまだ確かめていません。
  - 他の物理 CAN 構成や機器での送受信と送信レシート
  - チャネルを有効にし直さなくても、有効にするだけでバスオフから戻るか。チャネルを無効にしたとき、受信キューに残ったイベント（遅れて届くレシート）が消えるか
  - CAN FD モードで動いているチャネルを、クラシック CAN のポート（`XL_INTERFACE_VERSION`）で開いたときの挙動
  - `tests/vector_xl/xl_bridge_it_launcher` の往復の E2E テストは、Windows では bridge を強制終了するため、統計と終了コードを検査しません。止める要求（POSIX は SIGTERM、Windows はプロセスグループへの Ctrl+Break）で止めたときの統計の 1 行と終了コード 0 は、`stop-request` のシナリオが確かめます。Windows では、試験の実行ファイルとブリッジが共有するコンソールがないと Ctrl+Break を送れないので、このシナリオは飛ばされます（`device-lost` のシナリオは、ブリッジが自分で終わるので、どこでも検査します）。
  - `ExternalPortAdapter` で XL のチャネルにプロトコルスタックを載せた動作（自動テストは `ExternalCanPort` のフェイクに対してだけです）
