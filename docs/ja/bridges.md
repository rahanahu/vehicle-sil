# Vector XL CAN Bridge

[English](../bridges.md)

`vehicle_sil_vector_xl_bridge` は、SIL Kit の CAN ネットワークと Vector XL の CAN チャネルの間で、クラシック CAN のフレームを双方向に転送します。このドキュメントには、このブリッジだけのこととして、接続の手順、引数、転送の仕様、ビルド、検証の範囲が書いてあります。すべてのブリッジがランチャーと共有すること（ランチャーが渡す引数、`--describe`、終了コード、ブリッジが出す行、デバイスの印）は、[bridge-contract.md](bridge-contract.md) にあります。

| 接続するもの | `--xl-target` | 必要なもの | 確認済み | 未確認 |
|---|---|---|---|---|
| Vector Virtual CAN | `virtual` | Windows と Vector の XL Driver Library | Windows 11 の MSYS2 UCRT64 ビルドと、実際の XL Driver Library を使った送受信 | — |
| 物理 CAN（CANcase、VN16xx など） | `physical` | 上に加えて、Vector の CAN インターフェース | VN5620 の 2 つのチャネルを接続した物理ループバックでの送受信 | 他の物理 CAN 構成や機器 |

> [!WARNING]
> Windows 11 の MSYS2 UCRT64 ビルドと、実際の XL Driver Library を使った Vector Virtual CAN の送受信を確認済みです。VN5620 の 2 つのチャネルを接続した物理ループバックでも、送受信を確認しています。他の物理 CAN 構成や機器は未確認です。

イーサネットのブリッジ（`vehicle_sil_ethernet_bridge`）は [ethernet-bridge.md](ethernet-bridge.md) に、Linux の SocketCAN のブリッジ（`vehicle_sil_socketcan_bridge`）は [socketcan-bridge.md](socketcan-bridge.md) にあります。`vehicle_sil_launch run --connect <network>=xl:virtual:1`（`=virtual:1`、`=physical:<n>` でも）がこのブリッジを起動します。Linux では、XL のチャンネルでない CAN の先（`=can0`）が SocketCAN のブリッジを起動します（simulation-config.md の「[`--connect`](simulation-config.md#--connect)」）。

## 最初の接続

Vector Virtual CAN の 1 番目のチャネルに接続する手順です。ブリッジは Windows でだけ動きます。

| 手順 | すること | 確認 |
|---|---|---|
| 1 | XL Driver Library の場所を指定してビルドします（「[ビルド](#ビルド)」）。 | `vehicle_sil_vector_xl_bridge` ができます。 |
| 2 | 使えるチャネルを表示します。 | 接続先ごとに、番号の付いたチャネルの一覧が表示されます。 |
| 3 | SIL Kit の registry を起動してから、ブリッジを起動します。 | `bridging SIL Kit network ...` の行が表示されます。 |
| 4 | Ctrl+C で止めます。 | 統計の 1 行が表示され、終了コード 0 で終わります。 |

手順 2 のコマンドと、表示の形です。`--xl-channel` に続く番号を、手順 3 で使います。

```sh
vehicle_sil_vector_xl_bridge --list-channels
```

```
virtual CAN channels (--xl-target virtual):
  --xl-channel 1  <チャネルの名前>  hwType=<n> hwIndex=<n> hwChannel=<n> serial=<n> <トランシーバーの名前>
physical CAN channels (--xl-target physical):
  (none)
```

手順 3 のコマンドです。

```sh
# Vector Virtual CAN の 1 番目のチャネルに接続する
vehicle_sil_vector_xl_bridge --registry-uri silkit://127.0.0.1:8500 --network J1939_CAN1 --xl-target virtual --xl-channel 1
```

接続できると、次の形の 3 行が表示されます。

```
[bridge] XL channel: virtual 1 = "<チャネルの名前>" (hwType=<n>, serial=<n>)
[bridge] XL port open, init access: <yes|no>, bit rate unchanged
[bridge] bridging SIL Kit network 'J1939_CAN1' (participant 'VectorXlBridge', silkit://127.0.0.1:8500) <-> XL '<チャネルの名前>'; Ctrl+C to stop
```

物理 CAN に接続するときは、`--xl-target physical` を指定します。次の例は、1 番目のチャネルに 250 kbit/s で接続します。

```sh
vehicle_sil_vector_xl_bridge --registry-uri silkit://127.0.0.1:8500 --network J1939_CAN1 --xl-target physical --xl-channel 1 --bitrate 250000
```

ブリッジは、Ctrl+C のほか、Windows でコンソールのウィンドウを閉じたときにも止まり、転送の統計を出力します。

> [!CAUTION]
> 実行中に CAN インターフェースの USB を抜かないでください。ブリッジは止まりますが（終了コード 1）、VN5620 では挿し直しても認識されず、電源の入れ直しが必要なことがありました。

うまくいかないときは、次の表で探します。

| 症状 | 原因 | 直し方 |
|---|---|---|
| `error: ...` と表示され、終了コード 4 で終わる | 引数のエラーです。 | 「[引数](#引数)」を見て直します。 |
| `FAIL setup: cannot connect to the SIL Kit registry ...` | registry に接続できません。 | メッセージが示すとおりにします（「[registry に接続できないときのメッセージ](bridge-contract.md#registry-に接続できないときのメッセージ)」）。 |
| 使えるチャネルの一覧が表示され、終了コード 3 で終わる | `--xl-channel` の番号のチャネルがありません。 | `--list-channels` の番号を指定します。 |
| `--bitrate` を付けると起動に失敗する | ほかのアプリケーションが init 権限を持っています。 | `--bitrate` を指定しなければ、チャネルの設定を変えずに接続します。 |

## 引数

```
vehicle_sil_vector_xl_bridge --list-channels [--json]
vehicle_sil_vector_xl_bridge --registry-uri <silkit://host:port> --xl-target <virtual|physical> --xl-channel <n> [--bitrate <bit/s>] [--network <name>] [--name <name>] [--start-timeout-ms <ms>] [--stats-every-ms <ms>] [--rx-queue-size <n>] [--tx-timeout-ms <ms>] [--bus-off-restart-ms <ms>] [--no-bus-monitor]
vehicle_sil_vector_xl_bridge --describe --xl-target <virtual|physical> --xl-channel <n> [options]
```

| 引数 | 意味 |
|---|---|
| `--list-channels` | XL ドライバが提供する CAN チャネルを、接続先ごとに番号付きで表示して終わります。 |
| `--json` | `--list-channels` と一緒のときだけ使えます。一覧を JSON で標準出力に書きます。プログラムが読むためのものです（「[チャネルの一覧の JSON](#チャネルの一覧の-json--json)」）。 |
| `--registry-uri <uri>` | SIL Kit の registry のアドレスです（必須）。 |
| `--xl-target <virtual\|physical>` | `virtual` は Vector Virtual CAN（`XL_HWTYPE_VIRTUAL`）、`physical` はそれ以外のハードウェアの CAN チャネル（CANcase、VN16xx など）を選びます（必須）。 |
| `--xl-channel <n>` | そのターゲットの CAN 対応チャネルの中での、1 始まりの番号です（必須）。 |
| `--bitrate <bit/s>` | XL のチャネルのビットレートを 15000〜1000000 bit/s（`xlCanSetChannelBitrate` の範囲）で設定します。 |
| `--network <name>` | SIL Kit の CAN ネットワークの名前です（既定 `J1939_CAN1`）。 |
| `--name <name>` | participant の名前です（既定 `VectorXlBridge`）。 |
| `--participant <name>` | `--name` と同じです（どちらか 1 つを書きます）。 |
| `--start-timeout-ms <ms>` | SIL Kit で participant が Running になるのを待つ上限です（1〜10^12、既定 15000）。なれなければ終了コード 3 で終わります。 |
| `--stats-every-ms <ms>` | 動いている間にも統計の行を出します。最初の間隔で 1 回、その後は数が変わった間隔ごとに出します（100〜3600000）。指定しなければ、終わるときに 1 回だけです（「[統計](#統計)」）。 |
| `--describe` | ブリッジが何かを JSON で標準出力に書いて終わります（「[設定ファイルから起こす](#設定ファイルから起こす)」を参照）。 |
| `--rx-queue-size <n>` | XL の受信キューの容量です（2 のべき乗、16〜32768、既定 16384）。 |
| `--tx-timeout-ms <ms>` | バスが詰まっているあいだ、新しいフレームを捨てます（100〜60000。「[バスに出たフレームの数え方（送信レシート）](#バスに出たフレームの数え方送信レシート)」）。 |
| `--bus-off-restart-ms <ms>` | バスオフがこの時間続いたら、チャネルを有効にし直します（既定 1000、0 で無効、100〜60000。「[実行中のバスの監視](#実行中のバスの監視)」）。 |
| `--no-bus-monitor` | CAN コントローラーのエラー状態を、XL ドライバに問い合わせません。 |
| `--help` | 使い方を表示します。 |

引数ごとの補足です。

- `--xl-channel` の番号の並びはドライバの設定順で、`--list-channels` の表示と同じです。CAN に対応していないチャネル（LIN 専用など）は数えません。
- `--bitrate` を指定するときは、init 権限が必要です。ほかのアプリケーションが init 権限を持っている場合は、黙って別のビットレートで動かずに、起動を失敗させます。指定しなければ、チャネルの設定を変えません。
- `--rx-queue-size` でドライバが実際に保持できるのは、指定した値から 1 を引いたイベント数です。
- `--tx-timeout-ms` と `--bus-off-restart-ms` は、init 権限（`--bitrate`）があるときだけ使います。init 権限がないのに指定すると、ブリッジは起動時に `warning:` を出力します。
- Windows では、引数をワイド文字のコマンドラインから UTF-8 で読みます。そのため、`--name` と `--network` に ASCII 以外の文字を使えます。

チャネルとデバイスは、起動のときにこの CLI で決めます。Control API と設定ファイル（`vehicle_sil_launch`）には、チャネルとデバイスを選ぶ項目はありません。Control API、設定ファイル、GUI に選ぶ手段を追加するときは、CLI と同じモデル（`vehicle_sil::vector_xl::XlTarget` とチャネル番号）を使います。

### チャネルの一覧の JSON（`--json`）

`--list-channels --json` は、チャネルを bridge-contract.md の「[デバイスの一覧](bridge-contract.md#デバイスの一覧)」の形で書きます。ここには違う所だけを書きます。

```json
{
  "vehicle_sil_targets": 1,
  "tool": "vehicle_sil_vector_xl_bridge",
  "backend": null,
  "source": null,
  "targets": [
    {
      "value": "virtual:1",
      "label": "Virtual Channel 1",
      "detail": "virtual 1; hwType=1 hwIndex=0 hwChannel=0 serial=0 Virtual CAN",
      "usable": true,
      "reason": null,
      "in_use": false,
      "in_use_by": [],
      "in_use_count": 0,
      "can": {
        "target": "virtual",
        "number": 1,
        "name": "Virtual Channel 1",
        "hw_type": 1,
        "hw_index": 0,
        "hw_channel": 0,
        "serial": 0,
        "transceiver": "Virtual CAN"
      }
    }
  ],
  "notes": [],
  "error": null
}
```

| キー | 内容 |
|---|---|
| `tool` | `vehicle_sil_vector_xl_bridge` です。 |
| `backend`、`source` | `null` です。 |
| `targets` | CAN のチャネルです。virtual が先、physical が後で、番号は `--xl-channel` と同じです。CAN のないチャネルは入りません。 |
| `targets[].value` | `<target>:<number>` です。`:` の前を `--xl-target` に、番号を `--xl-channel` に渡します。 |
| `targets[].label` | ドライバが返すチャネルの名前です。 |
| `targets[].detail` | ターゲットと番号、続けて文の一覧の行の内容です。 |
| `targets[].usable` | いつも `true` です。使用中のチャネルも同じです。 |
| `targets[].in_use`、`in_use_by`、`in_use_count` | 約束のページのとおりです。Vector Virtual CAN のチャネルは 1 つのバスなので、どれか 1 つにブリッジがあれば、全部が使用中になります。 |
| `targets[].can` | `target`、`number`、`name`、`hw_type`、`hw_index`、`hw_channel`、`serial`、`transceiver`（ドライバが返さないときは `null`）です。 |
| `notes` | 空です。 |

一覧を作れないときの `error.code` は、XL ドライバを開けないときは `driver-missing`、ほかは `failed` です。

## 設定ファイルから起こす

`vehicle_sil_launch` は、シミュレーションのほかの participant といっしょにブリッジを起こせます。設定ファイルには ECU と同じように書き、`role: bridge` と `required: false` を付けます（ランチャーが `command` に足すものと `--describe` は、bridge-contract.md の「[自作のブリッジを起こす](bridge-contract.md#自作のブリッジを起こすrole-bridge)」にあります）。

```yaml
participants:
  - name: XlBridge
    role: bridge
    required: false
    command: [vehicle_sil_vector_xl_bridge, --network, VehicleCAN, --xl-target, physical, --xl-channel, "1"]
```

ネットワーク（`VehicleCAN`）は、`networks` に `kind: can` で書きます。

- `--describe` のとき、ブリッジは XL ドライバを開きませんが、実行ファイルは `vxlapi64.dll` がないと起動しません。Windows が実行ファイルといっしょに読み込むためです（ビルドは、見つけたときに実行ファイルの隣にコピーします）。それがない PC では、`check` は、要る DLL がなくてブリッジが起動できなかったと `describe-failed` で報告します。その PC では `--disable XlBridge` でブリッジを外します。
- `required: false`: シミュレーションはブリッジを待たず（「[起動の順序](#起動の順序)」）、実行が終わるとランチャーがブリッジに止める要求を送ります。

ファイルに書かずにランチャーにブリッジを起こさせるなら、`check` と `run` に `--connect <network>=<target>` を渡します（simulation-config.md の「[`--connect`](simulation-config.md#--connect)」）。アダプターやチャンネルは PC ごとに違うので、共有のファイルには書かず、起動するときに渡します。`run` は、ECU より先にブリッジを起こし、動き出すのを待つので、最初のフレームもバスに出ます。ブリッジが終わると実行を失敗にします（simulation-run.md の「[バスを外につなぐ](simulation-run.md#バスを外につなぐ--connect)」）。

## 終了コード

終了コード（0、1、3、4）、participant の状態の判定、コンソールのウィンドウを閉じたときの扱いは、すべてのブリッジで同じです（bridge-contract.md の「[終了コード](bridge-contract.md#終了コード)」）。このブリッジでは、終了コード 3 は準備のときの XL ドライバ、チャネルの選択、SIL Kit の失敗で、終了コード 1 には XL のチャネルが使えなくなった（デバイスを外した）ことも入ります。

### 実行中の participant の状態

participant の状態の判定と、そのときに出す文は、すべてのブリッジで同じです（bridge-contract.md の「[実行中の participant の状態](bridge-contract.md#実行中の-participant-の状態)」）。

## エラーメッセージ

引数のエラー、registry に接続できないときのメッセージ、出力の文字コードは、すべてのブリッジで同じです（bridge-contract.md の「[ブリッジが出す行](bridge-contract.md#ブリッジが出す行)」）。

### チャネルがないとき

存在しない番号を `--xl-channel` に指定すると、使えるチャネルの一覧を出力して終了コード 3 で終わります。

## 統計

行の形と各キーの意味は、bridge-contract.md の「[統計の行](bridge-contract.md#統計の行)」にあります。このブリッジは、その最初の例のキーを出します。`ext_tx_unsent` は、このブリッジでは、XL に渡したのに、バスに出ないまま失われたフレーム（止めるとき、チャネルを有効にし直したとき）です。

`ext_to_silkit` と `ext_from_devices`（このブリッジでは同じ値）に数えるのは、バスのほかのノードからのフレームだけです。自分の送信のレシートは `own_tx_dropped` です（レシートが有効なら、バスに出たフレームと同じ数）。ブリッジがチャンネルへフレームを送信し、最初の送信から 10 秒たってもバスからフレームが 1 つも来ないとき（`ext_from_devices` が 0 のまま）、ブリッジはヒントを 1 回だけ警告します。

```
nothing has come from a device on XL 'Virtual Channel 1' in the 10 s since the bridge first sent a frame there: if a device is meant to answer, check that it is on, its cable and termination, the bit rate, and that this is the channel it is on.
```

それより前にバスから来たフレームも、ヒントを止めます。レスポンスを待たない使い方（送信するだけ）でも出るので、文は条件つきです。イーサネットのブリッジも同じヒントを出します（[ethernet-bridge.md](ethernet-bridge.md#機器から何も来ないときのヒント) の「機器から何も来ないときのヒント」）。

## リンクの行

行の形は、bridge-contract.md の「[リンクの行](bridge-contract.md#リンクの行)」にあります。このブリッジは `link bus=<値>` を出します。

```
[bridge] link bus=active device=552dd508e2f03efe marked=1 shared=0
```

| 値 | 意味 |
|---|---|
| `active` | エラーアクティブ、またはエラーウォーニング（エラーカウンターが警告の値に達したが、コントローラーはエラーアクティブのまま） |
| `passive` | エラーパッシブ |
| `bus-off` | バスオフ |
| `unknown` | エラー状態の問い合わせの最初の答えがまだ来ていない、または答えが来ない（`--no-bus-monitor`、または答えないドライバ。「[エラー状態の問い合わせ](#エラー状態の問い合わせ)」） |

- 最初の行は、XL のポートを開いてから、ブリッジが SIL Kit のライフサイクルを始める前に出します。最初の答えが来るまでは `unknown` で、答えはあとから来ます。
- `link_line_value()` が値を作ります。

## 別のブリッジが使っているデバイス

このブリッジは、bridge-contract.md の「[別のブリッジが使っているデバイス](bridge-contract.md#別のブリッジが使っているデバイス)」の印を置き、数えます。このブリッジのデバイスは、Vector のハードウェアの CAN チャネルで、XL ドライバが報告するハードウェアの種類、インデックス、シリアル番号、チャネルで区別します（`xl-<hwType>-<hwIndex>-<serial>-<hwChannel>`）。または Vector Virtual CAN で、全部のチャネルが 1 つのデバイスです（`xl-virtual`）。Vector の 1 つの機器の 2 つのチャネルは、2 つのデバイスです。このブリッジの警告は次のとおりです。

```
[bridge] warning: XL 'Virtual Channel 1' (Vector Virtual CAN, whose channels are all one bus) is also used by another vehicle-sil bridge (process 4321): the frames of both mix there: if that is not meant, connect them to different devices, or stop a bridge left over from an earlier run
```

## 物理 CAN と仮想時間

- 物理 CAN に接続するときは、原則として実時間で動かします。`vehicle_sil_launch` は既定でそうします。`--connect` のある実行と、ファイルに有効な `role: bridge` の participant のいる実行は、速さ 1 で動きます（[simulation-run.md](simulation-run.md#外につながる実行の時間--time-policy) の「外につながる実行の時間（`--time-policy`）」）。
- こういう実行の間、`vehicle_sil_launch` は、止める、ステップで進める、速さを変えるコマンドを断ります（理由 `outside-real-time`）。標準入力、Control Server のクライアント、GUI のどれからでも同じです。`--time-policy free` では受け付けます。その間もブリッジは両方向にフレームを運び続け、実行は注記 `outside-time-changed` を付けて終わります。
- Vector Virtual CAN も、SIL Kit の仮想時間には自動で同期しないものとして扱います。
- Vector XL CAN Bridge は、autonomous lifecycle を使い、実時間で動きます。仮想時間のシミュレーションには参加させません。
- プロトコルスタックを XL のチャネルに直接載せる場合（`ExternalPortAdapter`）も実時間だけです。時刻同期で動かすホストに登録すると、構成エラーになります。

仮想時間の ECU と同じネットワークにブリッジを置くと、ECU はブリッジが転送したフレームを、受信した時点の自分のステップの時刻で受け取ります。シミュレーションが始まる前に受信したものは時刻 0 です。どのステップで受け取るかは壁時計しだいなので、結果は決定的ではありません（[internals/can.md](internals/can.md) の「SIL Kit のポート」）。

ブリッジ越しでは、時刻だけでなく順も崩れます。1 つのステップの間に届いたフレームは全部同じ時刻になり、J1939 の `Network` は同じ時刻のフレームを送り元ごとに識別子の小さい順で並べるので、実機のバスでの前後とは違う順で扱われることがあります。実機どうしの転送を `j1939::Listener` や `TransferSniffer` で聞くと、最後のパケットより先に「受け取った」（EndOfMsgACK）が来る、次の告知が前の「受け取った」より先に来る、といったことが起きます。前者は時刻が進むまで待って決めるので拾えますが、後者は両方を聞きこぼします。聞く participant のステップ幅を短くし、聞きこぼしの統計（`incomplete`、`replaced`）を変数に出して確認します（[internals/j1939.md](internals/j1939.md) の「ほかの participant が言ったことを聞く（`Listener`）」）。

## 転送の仕様

| 対象 | ブリッジの動作 |
|---|---|
| クラシック CAN のフレーム | 識別子（11 bit / 29 bit）、IDE、RTR、DLC、データを変えずに転送します。 |
| SIL Kit から XL への DLC 9〜15 | DLC 8 として送信します。 |
| CAN FD、CAN XL、不正な ID や DLC のフレーム | 転送せず、`silkit_rejected` または `ext_rejected` に数えます。 |
| XL のエラーフレーム | 転送せず、`error_frames` に数えます。 |
| XL の受信キューのあふれ | `overruns` に数えます。 |

- DLC 9〜15 を DLC 8 にするのは、`xlCanTransmit` が受け付ける DLC が 0〜8 のためです（XL Driver Library のマニュアル）。クラシック CAN では、DLC 9〜15 はどれもデータ 8 バイトです。XL から SIL Kit へは、受信した DLC のまま転送します。
- XL 側の ID は受信イベントから 11/29 ビットで取り出すので、`ext_rejected` に数えるのは、実際には DLC が 15 を超えるフレームです。

XL から SIL Kit への転送は、participant が SIL Kit の CommunicationReady に達してから始めます。ブリッジは、そこで SIL Kit のコントローラーを開始します（SIL Kit が勧めている時点です）。それまでに XL から受信したフレームは転送せず、数えません（起動の直後の短い間です）。SIL Kit から XL への転送は、その前から行います。

### バスに出たフレームの数え方（送信レシート）

`silkit_to_ext` は XL の送信キューに渡した数です。バスに出られないフレーム（ACK を返す相手がいない、ビットレートが違う、ケーブルが抜けた）は、XL の送信キューに残り、バスに出られるようになった時点でまとめて送信されます。

`XlCanPort` は送信レシートを有効にし、渡したのにまだレシートの来ないフレームを数えます。`own_tx_dropped`（自分の送信のレシートの数）が、バスに出た数です。

レシートを使うのは、ポートがレシートを有効にできたとき（init 権限があるとき）か、レシートを 1 つでも受信した後です。init 権限がなく、ほかのアプリケーションがレシートを無効にしているチャネルでは、止めるときの待ちは働かず、`ext_tx_unsent` は 0 のままです。

止めるとき（`XlCanPort::stop()`）は、チャネルを無効にする前に、渡したフレームがバスに出るのを最長 1 秒待ちます（`XlCanPortOptions::drain_timeout`）。それでも出なかった数を `ext_tx_unsent` に数えます。チャネルを無効にすると、XL の送信キューに残ったフレームは捨てられるためです。相手のブリッジも同時に止まると ACK が返らなくなるので、待っても出ないことがあります。

シミュレーションがバスより速く進んでも、バックプレッシャーはありません。CAN の帯域を超える速度で動かすと、フレームは XL の送信キューにたまり、`--tx-timeout-ms` がなければ止めるときに `ext_tx_unsent` になります。

XL の送信キューからフレームを消す `xlCanFlushTransmitQueue` は使いません。VN5620 では成功を返しても、キューのフレームはその後バスに出ました。

#### `--tx-timeout-ms`（バスが詰まっているあいだフレームを捨てる）

`--tx-timeout-ms <ms>`（100〜60000、`XlCanPortOptions::transmit_timeout`）を指定すると、バスが詰まっているあいだ、新しいフレームを XL に渡さずに捨て、`ext_tx_dropped` に数えます（`TransmitResult::Dropped`）。バスが詰まっているとは、最後の送信レシートの後に渡したフレームのどれにも、指定した時間レシートが来ない状態です。バスが直ったときにまとめて流れる古いフレームを、その時間ぶんと下の試しのフレームに抑えます。指定しなければ、フレームは何秒でも待ってから送信されます。

詰まっているかは、レシートが来た時刻だけで決めます。待っているフレームの数は使いません。数は、レシートが受信キューのあふれで失われるとずれるためです（あふれたら数を 0 に戻し、詰まりの判定もやり直します）。次のフレームのレシートが来れば、詰まりは解けます。

詰まっているあいだも、ときどき 1 フレームだけ試しに XL に渡します。レシートが失われた場合や、ドライバが待っているフレームを捨てた場合（バスオフの後など）に、レシートが二度と来なくなるのを避けるためです。試しのフレームの間隔は次のとおりです。

| 試し | 間隔 |
|---|---|
| 最初 | 詰まったと判断してから、指定した時間の後（コントローラーがエラーワーニング、エラーパッシブ、バスオフを通知しているときは 1 秒以上後） |
| 2 回目から | 試すたびに倍にし、5 秒（指定した時間のほうが長ければその時間）で止める |

間隔を伸ばすのは、試しのフレームも、バスが直るまでドライバのキューで待ってから送信されるので、長く詰まるほどたまらないようにするためです。D 秒詰まったときにたまる試しのフレームは、およそ D / 5 個と数個です。

この引数はレシートに頼るので、init 権限（`--bitrate`）があるときだけ使います。ポートがレシートを自分で有効にできるからです。init 権限がなければ、ほかのアプリケーションがいつでもレシートを無効にでき、詰まりが解けなくなるので、使わずに起動時に `warning:` を出力します。

## 実行中のバスの監視

### エラー状態の問い合わせ

`XlCanPort` は受信スレッドで、0.5 秒ごとに `xlCanRequestChipState` で CAN コントローラーのエラー状態を問い合わせ、レスポンスを `ExternalEventKind::BusState` で通知します。エラー状態は、エラーアクティブ、エラーワーニング、エラーパッシブ、バスオフと、送受信のエラーカウンターです。間隔は `XlCanPortOptions::bus_state_interval` で、0 にすると問い合わせません（実行ファイルでは `--no-bus-monitor`）。問い合わせを止めても、ドライバが自分から送る状態は通知します。VN5620 では、ACK のないバスで問い合わせなしでもエラー状態を受信しました。

問い合わせの失敗は、送信キューが満杯の場合を除いて、次のように扱います。

| 失敗のしかた | みなすこと | 動作 |
|---|---|---|
| 一度も受け付けられないうちの失敗 | その構成では問い合わせが使えない | `ExternalEventKind::Warning` を 1 回通知して問い合わせをやめ、転送は続けます。 |
| 一度受け付けられた後、3 回続けての失敗（`XlCanPortOptions::bus_state_failures`） | チャネルが使えなくなった（デバイスが外された） | `ExternalEventKind::Fault` を理由とともに 1 回通知して、受信をやめます。 |

1 つ目の場合、実行ファイルは `warning: ... the bus is not monitored, ...` を出力します。1 回だけの失敗では止めません。

### 監視の行

ブリッジの実行ファイルは 1 秒ごとに状態を見て、次のどちらかのときに 1 行出力します（`format_health_change()`）。

- コントローラーのエラー状態が変わったとき（起動直後の最初のレスポンスがエラーアクティブの場合を除く）
- 問題を表すカウンター（`error_frames`、`overruns`、`silkit_rejected`、`ext_rejected`、`ext_queue_full`、`ext_tx_failures`、`silkit_tx_failures`、`ext_tx_dropped`）が増えたとき

```
[bridge] XL bus: error passive (tx errors 128, rx errors 0); since the last report: error_frames +3650
```

問題のカウンターが増えた次の回に何も増えなければ、`; no new problems since the last report` を付けた行を 1 回出力します。送信だけをするコントローラーは受信に成功するまで受信エラーカウンターが下がらず、直った後もエラーパッシブのままのことがあるので、この行が回復の目印です。

`Fault` を受け取ったら、`FAIL: <理由>` を出力し、統計を出力して終了コード 1 で終わります。

### バスオフからの復帰

VN5620 の XL ドライバは、チャネルを有効にしたままでは、バスが直った後もバスオフから戻りませんでした。そこで `XlCanPort` は、コントローラーがバスオフを 1 秒通知し続けたら、チャネルを無効にしてから有効にし直し（`xlDeactivateChannel`、`xlActivateChannel`）、コントローラーを戻します。時間は `XlCanPortOptions::bus_off_restart`（実行ファイルでは `--bus-off-restart-ms`）で、0 にすると無効です。

- 有効にし直すのは、init 権限（`--bitrate`）があるときだけです。ほかのアプリケーションが設定しているチャネルで、自分のポートを無効にしたときに何が起きるかは分かっていないためです。init 権限がないのにオプションを指定すると、ブリッジは起動時に `warning:` を出力します。
- XL の送信キューで待っていたフレームはチャネルとともに消えるので、`ExternalEventKind::TransmitUnsent` で通知し、`ext_tx_unsent` に数えます。
- 有効にし直したことは `ExternalEventKind::Warning` で通知し、実行ファイルは `warning: XL channel '...' was bus off for <ms> ms; restarted the channel ...` を出力します。行を出力するのは、バスが最後に動いて（フレームを受信するか、送信したフレームのレシートが来て）から 1、2、4、8 ... 回目の有効にし直しだけです。
- バスがまだ壊れていれば、フレームを送信しているあいだ、コントローラーはまたバスオフになり、また有効にし直します。そのたびに待つ時間を倍にし、10 秒（指定がそれより長ければ指定の時間）で止めます。バスが動けば、元の時間に戻します。
- 有効にし直せなければ（無効にするか有効にするのに失敗したら）、`TransmitUnsent` の後に `Fault` を通知して受信をやめます。
- コントローラーの状態に頼るので、`--no-bus-monitor` では、ドライバが自分から状態を通知しない限り、有効にし直しません。

`ExternalPortAdapter` は `BusState`、`Fault`、`Warning`、`TransmitUnsent` を使いません。チャネルを有効にし直したことも、利用者には通知しません。

## 転送ループの防止

1. SIL Kit 側の受信ハンドラは、RX 方向だけで登録します。ブリッジが SIL Kit に送信したフレームを、ブリッジ自身は受信しません。
2. XL 側は、init 権限があるとき（`--bitrate` を指定したとき）、`xlCanSetChannelMode(tx=1, txrq=0)` で送信レシートを有効にし、送信要求の通知を無効にします（「[バスに出たフレームの数え方（送信レシート）](#バスに出たフレームの数え方送信レシート)」）。この関数は init 権限を必要とします（`vxlapi.h`）。init 権限がなければ、ドライバの既定（送信レシートあり）のままです。
3. `XL_CAN_MSG_FLAG_TX_COMPLETED` または `XL_CAN_MSG_FLAG_TX_REQUEST` が付いた受信イベントは、自分の送信として捨て、`own_tx_dropped` に数えます。レシートが有効なら、バスに出たフレームと同じ数になります。

同じ SIL Kit ネットワークと同じ XL のバスを、2 つのブリッジで接続する構成ではループを防げません。この構成には対応しません。

## キューとスレッド

ブリッジは独自の転送キューを持ちません。

| 向き | 呼び出すスレッド | 呼び出す関数 |
|---|---|---|
| SIL Kit から XL へ | SIL Kit の受信スレッド | `xlCanTransmit` |
| XL から SIL Kit へ | `XlCanPort` の受信スレッド | `ICanController::SendFrame` |

- XL の送信キューが満杯ならフレームを捨て、`ext_queue_full` に数えます。
- XL の受信キューの容量は `--rx-queue-size` で決まります（2 のべき乗、16〜32768、既定 16384）。ドライバが実際に保持できるのは、その値から 1 を引いたイベント数です。
- `xlReceive` は 1 回に 1 イベントずつ呼びます。Vector のサンプルも 1 件ずつ受け取っており、複数件を一度に受け取る使い方は確認していないためです。
- チャネルは `XL_ACTIVATE_NONE` で有効にします。ブリッジはタイムスタンプを使わず、`XL_ACTIVATE_RESET_CLOCK` は Vector の新しい時刻同期プロトコルと併用できないためです。
- 1 つのポートへの XL の呼び出しは、mutex で直列化します。

### 起動の順序

`CanBridge::start()` は、participant の lifecycle を開始する（`ParticipantRuntime::start()`）前に呼びます。ほかの participant は、ブリッジが Running になったのを見た時点で送信を始めます。その前にハンドラとコントローラが動いていないと、最初のフレームが失われます。

ランチャーは、設定ファイルに書いた `required: false` のブリッジがシミュレーションに参加するのを待ちません。ブリッジが参加する前に ECU が送信したフレーム（起動直後のアドレスの要求など）は、物理のバスに出ません。物理のバスの相手と最初からやり取りするテストでは、送信を少し遅らせて始めるか、`--connect` でランチャーにブリッジを起こさせます。ランチャーは、ブリッジが動き出してから ECU を起こします（simulation-run.md の「[バスを外につなぐ](simulation-run.md#バスを外につなぐ--connect)」）。

### 終了の順序

| 順 | すること |
|---|---|
| 1 | SIL Kit のハンドラを解除します（XL へ渡すフレームがなくなります）。 |
| 2 | XL の送信を待ち、受信を止めます（受信スレッドを join し、チャネルを非アクティブにします）。 |
| 3 | SIL Kit のコントローラを止めます。 |
| 4 | lifecycle を止めます。 |
| 5 | ポートを閉じます。 |
| 6 | ドライバを閉じます。 |

## プロトコルスタックから使う（`ExternalPortAdapter`）

`ExternalPortAdapter(external, clock)` は、外部ポートを開始し（バスに参加）、破棄か `close()` で止めます。`RealTimeDriver` と同じ時計を渡し、そのドライバーで動かします。時刻同期のホストには登録できません（構成エラー）。

| 外部ポートから受け取るもの | 扱い |
|---|---|
| 受信したフレーム | 外部ポートの受信スレッドで、到着時刻を付けて受信キューに積みます。 |
| リモートフレーム | 捨てて数えます。 |
| 自分の送信のレシート | 捨てます。 |
| エラーフレーム | `ErrorFrame` のイベントにします。 |
| 外部の受信キューのあふれ | 数の分からない `ReceiveOverflow` のイベントにします。 |
| バス状態（`ExternalEventKind::BusState`）と `Fault` | 使わず、`BusStateChanged` も出しません。 |

- ハードウェアのタイムスタンプは別の時計なので、到着時刻には使いません。
- 自分の送信のレシートを捨てるのは、同じスタックのノードへの配送をスタックが行うためです。
- 送信は、`transmit()` の同期の結果をそのまま返します（`Ok` は `Accepted`、`QueueFull` と `Dropped` は `QueueFull`）。その後バスに出たかは通知しません。
- 実機での確認はしていません（「[検証の範囲](#検証の範囲)」）。

## ビルド

- XL バックエンド（`vehicle_sil::vector_xl`）と実行ファイルは Windows 専用です。Vector の XL Driver Library（`vxlapi.h`、`vxlapi64.lib`）が必要です。
- XL Driver Library は vehicle-sil に含めません。`VEHICLE_SIL_VECTOR_XL_DIR` で場所を指定します。
- 見つからない場合は、ブリッジをビルドせずに続けます。`VEHICLE_SIL_REQUIRE_VECTOR_XL=ON` を指定すると、その場合に configure を失敗させます。
- Linux のビルドに XL Driver Library は不要です。
- ビルド済みのツールのアーカイブには、このブリッジは入っていません。XL Driver Library がないとビルドできないためです（アーカイブにあるのは Ethernet のブリッジだけです）。ビルドしたブリッジを `vxlapi64.dll` といっしょに、`vehicle_sil_launch` の隣か `PATH` の通ったディレクトリに置きます。`vehicle_sil_launch check --connect` と `run --connect` は、そこを探します（[simulation-config.md](simulation-config.md) の「ネットワークを外につなぐ（`external`、`--connect`）」）。

[XL Driver Library](https://www.vector.com/)（`vxlapi.h` と `vxlapi64.lib`）は Vector Informatik GmbH のソフトウェアで、vehicle-sil には含まれていません。利用者が Vector から入手し、Vector のライセンス条件に従って使います。ビルドでは、その場所を指定します。

```sh
cmake --preset windows-mingw-release -DVEHICLE_SIL_VECTOR_XL_DIR="C:/path/to/XL Driver Library/bin"
cmake --build --preset windows-mingw-release
```

XL Driver Library が見つかれば、`vxlapi64.dll` を実行ファイルの隣にコピーします。

## クラスの構成

フレームは、SIL Kit の CAN ネットワークから XL Driver までを、`ICanController`、`CanBridge`、`ExternalCanPort`（本番の実装は `XlCanPort`）の順に通ります。participant は `ParticipantRuntime` が所有します。XL Driver の先が、Virtual CAN か CANcase などの機器です。

| クラス | 責務 |
|---|---|
| `vehicle_sil::can_bridge::CanBridge` | SIL Kit の `ICanController` と `ExternalCanPort` の間で、クラシック CAN を双方向に転送する。プラットフォームに依存しない |
| `vehicle_sil::can_bridge::ExternalCanPort` | 外部 CAN のインターフェース。本番の実装は `XlCanPort`、テストではフェイク |
| `vehicle_sil::vector_xl::XlDriver` | `xlOpenDriver` / `xlCloseDriver` を RAII で管理し、`xlGetDriverConfig` からチャネルを列挙する |
| `vehicle_sil::vector_xl::XlCanPort` | XL のチャネル 1 つを開き（`xlOpenPort`）、送信（`xlCanTransmit`）と受信スレッド（`xlReceive` と通知イベント）を担う |
| `vehicle_sil::can_bridge::ExternalPortAdapter` | `ExternalCanPort` を CAN の共通基盤のポート（`RealTime`）にする。プロトコルスタックを XL のチャネルに直接載せるときに使う。プラットフォームに依存しない |

ソースの場所とテストは、[code-map-bridges.md](code-map-bridges.md#vector-xl-can-bridge) の「Vector XL CAN Bridge」にあります。

## 検証の範囲

### 自動テストの対象

XL バックエンドは 2 つに分かれます。

| 部分 | 内容 | ビルドする所 | 確認の方法 |
|---|---|---|---|
| ロジック（`xl_driver.cpp`） | 判断と状態を持ち、vehicle-sil 独自の内部インターフェース（`src/xl_backend.hpp`）だけに依存します。 | どのプラットフォームでも | 自動テスト |
| XL Driver Library の呼び出し（`xl_backend_vector.cpp`） | 型と値の変換だけを行います。 | Windows で XL Driver Library があるときだけ | 公式の XL Driver Library でのビルドとリンク |

- 自動テストは、内部インターフェースを実装したテスト専用のフェイク（`tests/vector_xl/fake_xl`）に対して、本番と同じ `xl_driver.cpp` と `main.cpp` を動かします。
- フェイクは、チャネルごとのバス、送信レシート、init 権限、受信キューのあふれ、エラーフレーム、コントローラーのエラー状態の問い合わせ、デバイスの喪失を模擬します。
- フェイクは、XL の次の制約も模擬します: 送信レシートの設定とビットレートは init 権限が必要、送信の DLC は 0〜8、受信キューは指定した容量から 1 を引いた数、受信は 1 回に 1 件。
- `xl_backend_vector.cpp` の変換（関数、定数、構造体のメンバーの対応）は自動テストの対象外です。
- `tests/vector_xl/xl_bridge_it_launcher` の `no-answers` のシナリオは、後ろにノードのないチャンネルへ `--stats-every-ms 100` で送信し、動いている間に統計の行が、変わったときだけ、すべてのキーを順に並べて出て、`ext_to_silkit=0` であること、リンクの行が開始の行より前に出て、値ごとに 1 回であることを確かめます。何も来ないときのヒントは、単体試験（`bridge_runtime.nothing_came_hint_*`）でだけ、テストが時刻を与えて確かめます。
- `device-shared` のシナリオは、Vector Virtual CAN にブリッジを起こします。偽のドライバーの既定のスコープでは、2 つのブリッジは互いを見つけません。同じスコープでは、`virtual:2` のブリッジと `virtual:1` のブリッジが、起動のときにも動いている間にも互いを見つけ、先のブリッジが警告し、一覧が両方の virtual のチャネルを使用中と言い、先のブリッジが終わると `shared` が 0 に戻ります。印そのもの（Linux と Windows、1 つのプロセスの中）は `bridge_runtime.device_mark_*` が確かめます。

### 確認したこと（Windows 11、Vector Virtual CAN と VN5620）

Windows 11 の MSYS2 UCRT64 で、XL Driver Library を使ったビルドとチャネル列挙を確認しています。Vector Virtual CAN の Channel 1 と 2 は同じ仮想バスに接続されていて、別々の SIL Kit ネットワーク間での送受信、`--bitrate` による init 権限、送信レシートの扱いを確認しています。VN5620 の 2 つの物理チャネルをループバック接続した構成で、別々の SIL Kit ネットワーク間での送受信も確認しています。

VN5620 のループバックで、次の点も確認しています。

- J1939 の 8 バイトを超えるメッセージ（BAM と RTS/CTS、RTS/CTS は最大の 1785 バイトまで）が、2 つのブリッジと物理のバスを通って、中断や再送要求なしに全部受信されること。CTS の往復がブリッジと物理のバスを通る分、1 件にかかる時間は SIL Kit だけの場合より長くなります。
- 物理チャネルでも、送信レシートが init 権限の有無によらず返り、`own_tx_dropped` が送信した数と同じになること。CAN の帯域を超える速度で動かしたとき、止める時点で送信できなかったフレームが `ext_tx_unsent` に数えられること。
- ACK を返す相手がいないとき、`--tx-timeout-ms` で新しいフレームを捨て（`ext_tx_dropped`）、XL に渡したフレームは相手が現れた後に全部バスに出ること（`silkit_to_ext` と `own_tx_dropped` が一致）。
- 同じチャネルを別のアプリケーション（init 権限を持つ別のブリッジ）と使うと、そのアプリケーションが送信したフレームを、ブリッジは通常の受信として受け取り、SIL Kit へ転送します。後から `--bitrate` 付きで開いた側は、init 権限がないので起動に失敗します。
- バスの監視（「[実行中のバスの監視](#実行中のバスの監視)」）。ACK を返す相手がいないとき、ビットレートが違うとき、ケーブルを抜いたときに、エラーパッシブとエラーフレームの増加を 1 秒ごとに表示し、直ると「no new problems」を表示します。送信だけをするコントローラーは、受信に成功するまで受信エラーカウンターが下がらないので、直った後もエラーパッシブのままのことがあります。
- 実行中に USB を抜くと、`xlCanRequestChipState` が `XL_ERR_INVALID_ACCESS` で失敗し続け、ブリッジは終了コード 1 で終わります。ただし VN5620 は、挿し直しても USB のデバイスとして現れないことがあり、電源の入れ直しが必要でした。実行中には USB を抜かないでください。
- Vector Virtual CAN でも、init 権限のない物理チャネルでも、状態の問い合わせは受け付けられます（`warning:` は出ません）。
- 終端を外して CAN_H と CAN_L を短絡するとバスオフになり、XL ドライバは、直した後もバスオフのままであること。ブリッジがチャネルを有効にし直すと、直ったバスで転送が戻ること（init 権限ありの場合）。終端を外しただけでは、受信エラーでエラーパッシブになり、バスオフにはなりません。
- 受信キューのあふれ（受信側のプロセスを止めて起こした）で、受信のフレームは失われ、その後も送信が続くこと。このテストでは送信レシートは失われませんでしたが、あふれたときにレシートが失われないとまでは確認していません。
- コンソールのウィンドウを閉じたときの統計の出力と終了コード 0、Ctrl+C での停止
- `windows-mingw-static` のブリッジとランチャーが、`PATH` に MinGW がなくても動くこと（取り込む DLL は Windows、UCRT、`vxlapi64.dll` だけ）

### 未確認のこと

次の点は、Vector のドライバとハードウェアでまだ確認していません。

- 他の物理 CAN 構成や機器での送受信と送信レシート
- チャネルを有効にし直さなくても、有効にするだけでバスオフから戻るか。チャネルを無効にしたとき、受信キューに残ったイベント（遅れて届くレシート）が消えるか
- CAN FD モードで動いているチャネルを、クラシック CAN のポート（`XL_INTERFACE_VERSION`）で開いたときの挙動
- `tests/vector_xl/xl_bridge_it_launcher` の往復の E2E テストは、Windows では bridge を強制終了するため、統計と終了コードを検査しません。止める要求（POSIX は SIGTERM、Windows はプロセスグループへの Ctrl+Break）で止めたときの統計の 1 行と終了コード 0 は、`stop-request` のシナリオが確認します。Windows では、テストの実行ファイルとブリッジが共有するコンソールがないと Ctrl+Break を送信できないので、このシナリオはスキップされます（`device-lost` のシナリオは、ブリッジが自分で終わるので、どこでも検査します）。
- `ExternalPortAdapter` で XL のチャネルにプロトコルスタックを載せた動作（自動テストは `ExternalCanPort` のフェイクに対してだけです）
- Windows のデバイスの使用中の印が、セッションをまたいで効くか。管理者でない利用者がグローバルな名前（`Global\`）を作れるか、セッションの名前（`Local\`）に切り替わるか。自動の試験は、1 つのセッションの中の印を確かめます。
- `vehicle_sil_launch` の設定ファイルからブリッジを起こす形。自動の試験は、フェイクのドライバーで `--describe` を確かめます（`tests/vector_xl/xl_bridge_it_launcher` の `describe`）。設定ファイルからの実行は、Ethernet ブリッジでだけ確かめています（`ethernet_bridge.integration.pcap-launcher`）。
