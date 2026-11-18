# SocketCAN Bridge

[English](../socketcan-bridge.md)

`vehicle_sil_socketcan_bridge` は、SIL Kit の CAN ネットワークと、動かしている Linux コンピューターの SocketCAN インターフェース（CAN アダプターの `can0`、または仮想の `vcan0`）の間で、クラシック CAN のフレームを双方向に転送します。特権は要らず、インターフェースには何も設定しません。このドキュメントには、このブリッジだけのこととして、接続の手順、引数、インターフェースの一覧、動作、確認済みの範囲と未確認の範囲が書いてあります。すべてのブリッジがランチャーと共有すること（ランチャーが渡す引数、`--describe`、終了コード、プログラム向けの行、デバイスの印）は、[bridge-contract.md](bridge-contract.md) にあります。Windows 用の Vector XL ブリッジ（[bridges.md](bridges.md)）と、同じ意味の引数は同じです。

ランチャーは `--connect` からこのブリッジを起動します。Linux では `vehicle_sil_launch run --connect CAN=can0 simulation.yaml` で、CAN のネットワーク `CAN` をインターフェース `can0` につなぎます（backend `socketcan` は、先が `virtual:<n>` でも `physical:<n>` でもないときの Linux の既定です。simulation-config.md の「[`--connect`](simulation-config.md#--connect)」）。手で起動するか、構成ファイルに `describe: true` の参加者として書きます（[bridge-contract.md](bridge-contract.md#自作のブリッジを起こすrole-bridge) の「自作のブリッジを起こす」）。

| 接続するもの | `--interface` | 必要なもの | 確認済み | 未確認 |
|---|---|---|---|---|
| 仮想 CAN | `vcan0` | `vcan` モジュールのある Linux（`sudo modprobe vcan`） | ブリッジ全体（試験のネットワーク名前空間で） | — |
| CAN アダプター（USB、PCIe、SPI） | `can0` | Linux、SocketCAN のドライバーがあるアダプター、立ち上げたインターフェース | エラーのフレーム、bus-off、送信のキュー（偽のソケットの層に対して） | 実機のアダプターと実際のバス |

## 最初の接続

```sh
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 up

sil-kit-registry --listen-uri silkit://127.0.0.1:8500 &
vehicle_sil_socketcan_bridge --registry-uri silkit://127.0.0.1:8500 --network J1939_CAN1 --interface vcan0
candump vcan0        # from can-utils: the frames of the simulation
cansend vcan0 123#1122   # a frame into the simulation
```

アダプターでは、先に速さを設定して立ち上げます（root が要ります。ブリッジはしません）。

```sh
sudo ip link set can0 type can bitrate 250000 restart-ms 100
sudo ip link set can0 up
vehicle_sil_socketcan_bridge --registry-uri silkit://127.0.0.1:8500 --network J1939_CAN1 --interface can0 --bitrate 250000
```

## 引数

| 引数 | 意味 |
|---|---|
| `--registry-uri <uri>` | SIL Kit のレジストリ（必須。`--describe` と `--list-channels` では不要） |
| `--interface <name>` | SocketCAN のインターフェース（必須） |
| `--network <name>` | SIL Kit の CAN ネットワーク（必須。既定はありません） |
| `--name <name>`、`--participant <name>` | SIL Kit の参加者の名前（`SocketCanBridge`） |
| `--bitrate <bit/s>` | インターフェースが動いているべき速さ（15000..1000000）。ブリッジは設定しません |
| `--tx-timeout-ms <ms>` | バスがこの時間、1 つも受け取らないあいだはフレームを捨てる（100..60000） |
| `--stats-every-ms <ms>` | 動いているあいだ統計の行を出す（100..3600000） |
| `--start-timeout-ms <ms>` | 参加者が動くのを待つ時間（15000） |
| `--describe` | ブリッジが何かを JSON で書いて終わる（インターフェースは開かない） |
| `--list-channels [--json]` | CAN のインターフェースを一覧して終わる |

終了コード: 0 は通常の停止、1 は動いている最中の失敗（SIL Kit が終わった、またはインターフェースがなくなった）、3 は準備の失敗、4 はコマンドラインの誤りです（bridge-contract.md の「[終了コード](bridge-contract.md#終了コード)」）。統計の行のキーは Vector XL ブリッジと同じです（「[統計の行](bridge-contract.md#統計の行)」。外側は `ext_*`、ブリッジ自身のフレームの受領は `own_tx_dropped`）。監視の行と停止は Vector XL ブリッジと同じです（bridges.md の「[監視の行](bridges.md#監視の行)」）。

### インターフェースの一覧

`--list-channels` は、リンクの種類が CAN のインターフェースを出します。`--json` は「[デバイスの一覧](bridge-contract.md#デバイスの一覧)」の形です（インターフェース 1 つが 1 つの target、`value` はインターフェース名）。

```
CAN interfaces (--interface):
  vcan0  virtual CAN, up
  can0  CAN, up, 250000 bit/s, error-active; in use by a vehicle-sil bridge (process 4321)
```

各 target に `can: {kind, up, bitrate, state, restart_ms}`（`kind` はアダプターなら `can`、それ以外はカーネルの種類の名前（`vcan`、`vxcan` …）で、`can` 以外はすべて仮想として扱います。`state` はコントローラーの状態で、仮想では `null`）と、「[別のブリッジが使っているデバイス](bridge-contract.md#別のブリッジが使っているデバイス)」と同じ `in_use`、`in_use_by`、`in_use_count` があります。down のインターフェースも一覧に出て `usable` です（`detail` に down と出ます）。一覧は特権なしでカーネルから読みます（netlink）。

## 起動

次のときブリッジは起動を断ります（終了コード 3、直し方つき）。

- そのインターフェースがない（今あるものを文に並べます）。
- インターフェースが down: `sudo ip link set can0 up`
- `--bitrate` がアダプターの速さと違う: `sudo ip link set can0 down && sudo ip link set can0 type can bitrate 250000 && sudo ip link set can0 up`。`--bitrate` がなければ、ブリッジはインターフェースをそのまま使います。速さの設定には `CAP_NET_ADMIN` が要りますが、ブリッジはそれを求めません。
- レジストリが答えない。

速さのない vcan では、`--bitrate` は `warning:` の行で知らせて無視します。`restart-ms` が 0 のアダプターでは警告します。bus-off の後、だれかが再起動するまでコントローラーはバスから外れたままになります。

## フレーム

- ブリッジはインターフェースに raw の CAN ソケットを 1 つ開き、自分が送ったフレームをカーネルから戻してもらいます（`CAN_RAW_RECV_OWN_MSGS`）。`MSG_CONFIRM` の印があるフレームはブリッジ自身のフレームの受領です。`own_tx_dropped` に数え、転送しません。
- ほかのフレームはすべて転送し、`ext_from_devices` に数えます。**この PC のほかのプログラム（`cansend`、`cangen`、別のブリッジのソケット）が送ったフレームも含みます。** それがブリッジの相手の機器であり、`can0` でも `vcan0` でも同じです。カーネルはそれらに `MSG_DONTROUTE` を付けますが、ブリッジはその印で除きません。
- クラシック CAN だけです。CAN FD のフレームは受けません（ブリッジは `CAN_RAW_FD_FRAMES` を有効にしないので、カーネルがソケットに渡さず、数えられません）。クラシック CAN でない SIL Kit のフレームは `silkit_rejected` に数えます。DLC 9..15 は 8 バイトで送ります。
- 忙しいバスでは、ソケットのカーネルの受信キューがあふれても、ブリッジには知らされません（`overruns` は 0 のままです）。
- インターフェースの送信キューが満杯（`ENOBUFS`）なら、フレームを捨てて `ext_queue_full` に数えます。`--tx-timeout-ms` では、受領のない最も古いフレームがその時間より古いあいだフレームを捨て（`ext_tx_dropped`）、同じ間隔で 1 つだけ探りとして送ります。受領が来ると捨てるのをやめます。見るのはその時間だけで、フレームの数は数えません（Vector XL ブリッジと同じ）。カーネルが返さなかった受領（bus-off でキューが空になった、受領があふれた）は、ブリッジが `--tx-timeout-ms` のあいだ何も送らなかったとき、とインターフェースが up に戻ったときに忘れます。
- `ext_tx_unsent` はいつも 0 です。カーネルがキューから失ったフレームを、ブリッジは知りません。受領（`MSG_CONFIRM`）がフレームのバスに出た後に来るのは、ドライバーが echo を返すときだけです。返さないドライバー（未確認）では、フレームを渡した時点で受領が来るので、`own_tx_dropped` はバスに出た数にならず、`--tx-timeout-ms` はバスの詰まりを見つけません。

## リンクの行

ブリッジは `bus` のほかに、プログラム向けに `link=<up|down>` を出し、その後にデバイスの印のキーを続けます。

```
[bridge] link bus=active link=up device=552dd508e2f03efe marked=1 shared=0
```

`bus` は `active`、`passive`、`bus-off`、`unknown` で、「[リンクの行](bridge-contract.md#リンクの行)」と同じです。vcan の値は `unknown` だけです。`link=down` はインターフェースが down（`IFF_UP` がない）ことで、down になったときと戻ったときに `warning:` の行も出します。ブリッジは 1 秒に 4 回インターフェースを見て、送信が `ENETDOWN` で断られたときはすぐ見ます。down のあいだも動き続け、up に戻れば送受信を再開します。そのあいだフレームは送れません（断られた数は `ext_tx_failures`）。down をどれだけ許すかはランチャーが決めます。

## バスの状態と `restart-ms`

ブリッジはエラーのフレームを受け（`CAN_RAW_ERR_FILTER`）、見るたびにコントローラーの状態をカーネルから読みます（netlink）。error passive、bus-off、再起動が `bus` を決めます。エラーのフレームは `error_frames` に数え、健康の行「CAN bus 'can0': ...」に出ます。bus-off から戻すのはブリッジではなくカーネルで、`restart-ms` が 0 でなければ、bus-off の `restart-ms` ミリ秒後にコントローラーを再起動します。vcan はエラーのフレームを出しません。

## インターフェースがなくなったとき

`ENODEV` と `ENXIO`（アダプターの抜去、`ip link del`）と、見たときにインターフェースがなくなっている（名前が変わった）ことで、ブリッジは終わります。`FAIL: the interface is gone ...`、終了コード 1。ランチャーが `--connect` から起動したブリッジなら、これは接続が切れたこと（`connect-lost`）として報告されます。

## 別のブリッジが使っているデバイス

ブリッジはインターフェースに「[別のブリッジが使っているデバイス](bridge-contract.md#別のブリッジが使っているデバイス)」の印を置きます（正体はインターフェース名）。`vcan0` に 2 つ目のブリッジがあると、両方が警告して `shared=1` を出し、一覧は使用中と言います。1 つのインターフェースの 2 つのブリッジは互いのフレームを受けてネットワークが混ざります。ブリッジは止まりません。

## ビルド

Linux だけです。CMake は他のものと一緒に `vehicle_sil_socketcan_bridge` をビルドします（追加のライブラリは要りません）。カーネルに SocketCAN（`can_raw`、仮想のインターフェースには `vcan`）が要ります。`scripts/package.py tools` は、この実行ファイルを Linux のアーカイブの `bin/` に入れます。

## 検証の範囲

| 試験 | 確かめること |
|---|---|
| `socketcan_bridge.*` | コマンドライン、netlink の応答とインターフェースの確認、一覧、エラーのフレーム、偽のソケットの層での `SocketCanPort`（受領、エラーのフレーム、bus-off、`ENOBUFS`、`ENETDOWN`、`ENODEV`、送信のタイムアウト） |
| `socketcan_bridge.netns.*` | ネットワーク名前空間の `vcan0` での実行ファイル（フレームの表の往復、受領、別のソケットのフレーム、down と up、削除、断る場合、一覧、2 つ目のブリッジ、`vehicle_sil_launch run --connect J1939_CAN=vcan0` が `examples/ecu_main` のエンジン ECU と一緒にブリッジを起こすこと） |

`netns` の試験は、ネットワーク名前空間や `vcan0` を作れない所（`vcan` モジュールがない: `sudo modprobe vcan`、またはユーザー名前空間が無効）では飛ばします（77）。`VEHICLE_SIL_REQUIRE_SOCKETCAN=1` なら失敗にします（CI はモジュールを読み込んでこれを付けます）。

**確認済み**: `vcan0` での双方向の転送、ブリッジ自身のフレームの受領、別のソケットのフレーム、down と up、削除、2 つ目のブリッジ、一覧。**未確認**: 物理の CAN アダプターと実際のバス（速さの確認、error passive、bus-off と `restart-ms` での復帰、アダプターの抜去）と、実際のカーネルのエラーのフレーム（試験は偽のソケットの層で再現しています）、カーネルの受信キューがあふれる速さ。
