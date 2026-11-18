# イーサネットのブリッジ

[English](../ethernet-bridge.md)

`vehicle_sil_ethernet_bridge` は、SIL Kit の Ethernet ネットワークと SIL Kit の外部のインターフェースの間で、イーサネットのフレームを書き換えずに双方向に転送します。実時間で動き、時刻同期には参加しません。このドキュメントには、このブリッジだけのこととして、接続の手順、引数、バックエンドごとの動作、フィルターとループの防止、ライブラリの API、確認した範囲が書いてあります。すべてのブリッジがランチャーと共有すること（ランチャーが渡す引数、`--describe`、終了コード、ブリッジが出す行、デバイスの印）は、[bridge-contract.md](bridge-contract.md) にあります。

| 接続する相手 | `--backend` | OS | インストールするもの | 確認した範囲 |
|---|---|---|---|---|
| 同じ PC の OS のネットワークスタックと、TAP デバイスを開くソフト | `tap` | Linux | なし | ネットワーク名前空間の中で、本物のカーネルに対する自動テスト |
| ネットワークのインターフェース（NIC、veth）の先の機器 | `packet` | Linux | なし | veth の対での自動テスト（物理 NIC は未確認） |
| `packet` と同じ | `pcap` | Linux | libpcap | veth の対と本物の libpcap での自動テスト（物理 NIC は未確認） |
| ネットワークのアダプターの先の機器 | `npcap` | Windows | Npcap | 本物の Npcap では、仮想マシンの仮想 NIC と、LAN につないだ USB のアダプター 1 つでの手動の確認だけ |

> [!WARNING]
> Windows の `npcap` は、本物の Npcap に対しては、手動で確認しただけです。確認したのは、仮想マシンの仮想 NIC と、その仮想マシンに渡した物理の USB のアダプター 1 つ（LAN につなぎ、相手は PC のネットワークスタック）です。回線の先の実機や別の PC、物理 NIC での大量のトラフィックは確認していません。テストは、同じ関数を持つフェイクのライブラリに対するものです。確認した範囲は「[確認したことと、確認していないこと](#確認したことと確認していないこと)」にあります。

ビルド済みのツールのアーカイブ（[build.md](build.md) の「ビルド済みのツール」）には、`bin/` の `vehicle_sil_launch` の隣に `vehicle_sil_ethernet_bridge` が入っています。そのため、`PATH` を変えなくても `vehicle_sil_launch run --connect` がブリッジを見つけます。Npcap と libpcap はアーカイブに入っていません。リポジトリでビルドしたときのブリッジは `build/<preset>/bridges/ethernet/vehicle_sil_ethernet_bridge` です。

Vector XL の CAN のブリッジは [bridges.md](bridges.md) にあります。ビルド済みのツールのアーカイブには入っていないので、自分でビルドして、`vehicle_sil_launch` の隣か `PATH` の通ったディレクトリに置きます。

## 最初の接続

Linux で、PC の中の TAP デバイスに接続する手順です。先に作った自分が所有者の TAP デバイスなら、外部のライブラリも、実行時の権限も不要です。`vehicle_sil_ethernet_bridge` と `sil-kit-registry` には `PATH` が通っているものとします。

| 手順 | すること | 確認 |
|---|---|---|
| 1 | 自分のユーザーを所有者にして TAP デバイスを作り、up にします。 | `--list-interfaces` に `vsil0  Ethernet, up, disconnected` と表示されます（TAP デバイスは、プログラムが開くまで carrier がありません）。 |
| 2 | SIL Kit の registry を起動します。 | registry が `silkit://127.0.0.1:8500` で待ち受けます。 |
| 3 | 別の端末でブリッジを起動します。 | `bridging SIL Kit network ...` の行が表示されます。 |
| 4 | Ctrl+C で止めます。 | `stopping`、統計、`stopped` の順に表示され、終了コード 0 で終わります。 |

手順 1 のコマンドです。

```sh
sudo ip tuntap add dev vsil0 mode tap user $USER
sudo ip link set vsil0 up
vehicle_sil_ethernet_bridge --list-interfaces
```

手順 2 のコマンドです。

```sh
sil-kit-registry --listen-uri silkit://127.0.0.1:8500
```

手順 3 のコマンドと、接続できたときの表示です。

```sh
vehicle_sil_ethernet_bridge --registry-uri silkit://127.0.0.1:8500 --network ETH1 --backend tap --interface vsil0
```

```
[eth-bridge] bridging SIL Kit network 'ETH1' (participant 'EthernetBridge', silkit://127.0.0.1:8500) <-> tap 'vsil0' (TAP device in this computer): every frame, both ways; MAC guard on; Ctrl+C to stop
```

手順 4 の表示です。OS が TAP デバイスに送信したフレームがあると、`ext_to_silkit` は 0 になりません。TAP デバイスでは相手が OS なので、そのフレームは機器からのものに数えます（`ext_from_devices`。「[統計](#統計)」）。

```
[eth-bridge] stopping
[eth-bridge] stats silkit_to_ext=0 ext_to_silkit=6 ext_from_devices=6 own_tx_dropped=0 loop_suspected=0 mac_unguarded=0 filtered_to_ext=0 filtered_from_ext=0 oversize=0 malformed=0 ext_overruns=0 ext_queue_full=0 ext_tx_failures=0 silkit_tx_failures=0 ext_checksum_incomplete=0
[eth-bridge] stopped
```

うまくいかないときは、次の表で探します。

| 症状 | 原因 | 直し方 |
|---|---|---|
| `error: ...` と表示され、終了コード 4 で終わる | 引数のエラーです。 | 「[引数](#引数)」を見て直します。 |
| `FAIL setup: cannot connect to the SIL Kit registry ...` | registry に接続できません。 | メッセージが示すとおりに、registry を起動するかアドレスを直します。 |
| `FAIL setup: no TAP device 'vsil0' ...` など | TAP デバイスを開けません。 | 「[`tap`](#tap)」のメッセージの一覧にある直し方に従います。 |
| `interface 'vsil0' is down: ...` の警告 | TAP デバイスが down です。 | `sudo ip link set vsil0 up` を実行します。 |

### ネットワークのインターフェースに接続する（Linux）

ネットワークのインターフェースに接続するときは、`CAP_NET_RAW` と、何を通すかの指定が必要です。

```sh
sudo setcap cap_net_raw+ep ./vehicle_sil_ethernet_bridge
vehicle_sil_ethernet_bridge --registry-uri silkit://127.0.0.1:8500 --network ETH1 --backend packet --interface enp3s0 \
    --ethertype 0x88b5
```

### ネットワークのアダプターに接続する（Windows）

先に Npcap をインストールします（「[`npcap` と `pcap`](#npcap-と-pcap)」）。`--interface` の名前は、`--list-interfaces` が表示するとおりに書きます。

```bat
vehicle_sil_ethernet_bridge --list-interfaces
vehicle_sil_ethernet_bridge --registry-uri silkit://127.0.0.1:8500 --network ETH1 --backend npcap --interface "Ethernet 2" ^
    --ethertype 0x88b5
```

## 引数

```
vehicle_sil_ethernet_bridge --list-interfaces [--backend <backend>] [--json]
vehicle_sil_ethernet_bridge --registry-uri <silkit://host:port> --network <name> [--backend <tap|packet|npcap|pcap>] --interface <name> [options]
vehicle_sil_ethernet_bridge --describe --network <name> [--backend <backend>] --interface <name> [options]
```

| 引数 | 意味 |
|---|---|
| `--registry-uri <uri>` | SIL Kit の registry のアドレスです（必須）。 |
| `--network <name>` | SIL Kit の Ethernet ネットワークの名前です（必須。既定の名前はありません）。 |
| `--name <name>` | participant の名前です（既定 `EthernetBridge`）。 |
| `--participant <name>` | `--name` と同じです（どちらか 1 つを書きます）。 |
| `--start-timeout-ms <ms>` | SIL Kit で participant が Running になるのを待つ上限です（1〜10^12、既定 15000）。なれなければ終了コード 3 で終わります。 |
| `--stats-every-ms <ms>` | 動いている間にも統計の行を出します。最初の間隔で 1 回、その後は数が変わった間隔ごとに出します（100〜3600000）。指定しなければ、止まるときに 1 回だけです（「[統計](#統計)」）。 |
| `--describe` | ブリッジが何かを JSON で標準出力に書いて終わります（「[設定ファイルから起こす](#設定ファイルから起こす)」を参照）。 |
| `--backend <tap\|packet\|npcap\|pcap>` | 外部のインターフェースの開き方です（既定は Linux で `packet`、Windows で `npcap`。「[バックエンド](#バックエンド)」）。 |
| `--interface <name>` | 接続するインターフェースです（必須。「最初に見つかったもの」は選びません）。 |
| `--ethertype <hex>,...` | 通す EtherType です（`0x` で始まる 16 進。繰り返すと追加されます）。 |
| `--src-mac <mac>,...` | SIL Kit 側の MAC アドレスです（`:` か `-` で区切った 6 バイト。繰り返すと追加されます）。 |
| `--all-frames` | フィルターをかけません（`--ethertype`、`--src-mac` とは一緒に使えません）。 |
| `--vlan-tpid <hex>,...` | VLAN タグを示す値です（既定 `0x8100,0x88a8`）。 |
| `--max-frame-bytes <n>` | FCS を除いた最大長です（60〜65535、既定 1522）。 |
| `--promiscuous` | どのアドレス宛てのフレームも受信します。インターフェースがプロミスキャスを受け付けなければ、起動に失敗します（`tap` では使えません）。 |
| `--no-promiscuous` | インターフェース自身のアドレス宛てとグループのアドレス宛てのフレームだけを受信します（`tap` では使えません）。 |
| `--tap-create` | TAP デバイスを、ブリッジが動く間だけ作ります（`tap` だけ）。 |
| `--no-mac-guard` | MAC のルール（「[ループの防止](#ループの防止macsidetable)」）を使いません。 |
| `--mac-table-size <n>` | MAC テーブルの大きさです（16〜65536、既定 1024）。 |
| `--list-interfaces` | バックエンドのインターフェースの一覧を表示して終わります（ほかの引数は不要です。「[インターフェースの一覧](#インターフェースの一覧--list-interfaces)」）。 |
| `--json` | `--list-interfaces` と一緒のときだけ使えます。一覧を JSON で標準出力に書きます。プログラムが読むためのものです（「[一覧の JSON](#一覧の-json--json)」）。 |
| `--help` | 使い方を表示します。 |

引数ごとの補足です。

- `--network` に既定の名前がないのは、別のネットワークに誤って接続しないためです。
- `--promiscuous` も `--no-promiscuous` もなければ、インターフェースが受け付けるならプロミスキャスで、受け付けなければ警告を 1 つ出してプロミスキャスなしで動きます（「[プロミスキャス](#プロミスキャス)」）。
- `--backend` の `tap` は PC の中の仮想のインターフェース、ほかは物理のアダプターにフレームが出うるものとして扱います。`tap`、`packet`、`pcap` は Linux、`npcap` は Windows で使えます。
- `--max-frame-bytes` の下限が 60 なのは、SIL Kit が 60 バイトより短いフレームを渡さないためです。
- `--backend` がなければ、OS のバックエンドを使います。Linux では `packet`、Windows では `npcap` です。`--list-interfaces` の一覧とブリッジ本体は、同じバックエンドを使います。
- `--list-interfaces` に `--backend npcap` か `--backend pcap` を付けると、pcap のライブラリが返す一覧を表示します。
- Windows では、引数をワイド文字のコマンドラインから UTF-8 で読みます。そのため、`--name`、`--network`、`--interface` に ASCII 以外の文字を使えます。

物理のアダプターにフレームが出うるバックエンド（`packet`、`npcap`、`pcap`）では、`--backend` で指定しても既定でも、何を通すか（`--ethertype`、`--src-mac`、`--all-frames` のどれか）の指定が必須です。指定がないと、バックエンドの名前を入れた次のエラーになります。

```
--backend npcap sends to a real adapter: say what may pass with --ethertype, --src-mac or --all-frames.
```

引数は、`vehicle_sil/ethernet_bridge/bridge_options.hpp` の `parse_bridge_options()` が解釈します。エラーは `BridgeUsageError` で、何がエラーかと直し方を示します。使い方の全文は `bridge_usage()` が返します。

## 設定ファイルから起こす

`vehicle_sil_launch` は、シミュレーションのほかの participant といっしょにブリッジを起こせます。設定ファイルには ECU と同じように（`describe: true`、既定）書き、`role: bridge` と `required: false` を付けます。

```yaml
participants:
  - name: EthBridge
    role: bridge
    required: false
    command: [vehicle_sil_ethernet_bridge, --network, ETH_EXT, --backend, npcap,
              --interface, "Ethernet 2", --src-mac, "02:00:00:00:00:01"]
```

Linux では `--backend packet`（か `pcap`）とネットワークインターフェースの名前を書きます。ネットワーク（`ETH_EXT`）は、`networks` に `kind: ethernet` で書きます。

- ランチャーが足す引数と `--describe` が書くものは、bridge-contract.md の「[自作のブリッジを起こす](bridge-contract.md#自作のブリッジを起こすrole-bridge)」と「[`--describe`](bridge-contract.md#--describe)」にあります。このブリッジは、`--describe` のときインターフェースを開かず、pcap のライブラリを読み込まず、SIL Kit にもつながないので、Npcap や libpcap のない PC でも `check` が通ります。
- `required: false`: シミュレーションはブリッジを待たず、実行が終わるとランチャーがブリッジに止める要求を送ります（ブリッジが参加する前に送られたフレームについては、[bridges.md](bridges.md#起動の順序) の「起動の順序」）。

ファイルに書かずにランチャーにブリッジを起こさせるなら、`check` と `run` に `--connect <network>=<target>` を渡します（simulation-config.md の「[`--connect`](simulation-config.md#--connect)」）。アダプターやチャンネルは PC ごとに違うので、共有のファイルには書かず、起動するときに渡します。`run` は、ECU より先にブリッジを起こし、動き出すのを待つので、最初のフレームもバスに出ます。ブリッジが終わると実行を失敗にします（simulation-run.md の「[バスを外につなぐ](simulation-run.md#バスを外につなぐ--connect)」）。

### 1 台の PC で `run --connect` を試す（Linux）

veth のペアをケーブルの代わりにします。ブリッジが一方の端（`vsil0`）を開き、機器がいるはずのもう一方の端（`vsil1`）を `tcpdump` で見ます。この手順は、このリポジトリのビルド（`build/<preset>`）の例 `examples/ethernet` を使います。この例の 2 つの ECU はネットワーク `ETH` で通信し、MAC アドレスを報告するので、ファイルに `external` を書かなくても、ブリッジは ECU のフレームを通します。ビルドでは、ブリッジは `vehicle_sil_launch` の隣にないので、手順ではブリッジのディレクトリを `PATH` に足します。

| 手順 | すること | 見えるもの |
|---|---|---|
| 1 | veth のペアを作り、両端を up にします。 | `vehicle_sil_ethernet_bridge --list-interfaces` に、`vsil0` と `vsil1` が `Ethernet, up, connected` で出ます。 |
| 2 | ブリッジに `CAP_NET_RAW` を与えます（「[`packet`](#packet)」）。 | 何も出ません。 |
| 3 | 別の端末で、もう一方の端を見ます。 | `tcpdump` が待ちます。 |
| 4 | 例を速さ 1、`--connect ETH=vsil0` で実行します。 | `outside: ETH -> vsil0 (bridge.ETH)` の行、参加者の表に `stopped (SIGTERM), exit code 0` の `bridge.ETH`、`passed: 0 errors, 0 warnings, 0 notes` が出ます。 |
| 5 | `tcpdump` の出力を見ます。 | Sensor（`02:00:00:00:00:01`）と Display（`02:00:00:00:00:02`）の間のフレーム（EtherType `0x88b5`）が、例が動く 1 秒間に 20 ほど出ます。 |
| 6 | veth のペアと権限を消します。 | 何も出ません。 |

手順 1 と 2 のコマンド:

```sh
sudo ip link add vsil0 type veth peer name vsil1
sudo ip link set vsil0 up
sudo ip link set vsil1 up
build/<preset>/bridges/ethernet/vehicle_sil_ethernet_bridge --list-interfaces
sudo setcap cap_net_raw+ep build/<preset>/bridges/ethernet/vehicle_sil_ethernet_bridge
```

手順 3 のコマンド:

```sh
sudo tcpdump -e -n -i vsil1 ether proto 0x88b5
```

手順 4 のコマンド:

```sh
cd build/<preset>/examples/ethernet
PATH="$PWD/../../bridges/ethernet:$PATH" ../../tools/launch/vehicle_sil_launch run simulation.yaml --speed 1 --connect ETH=vsil0
```

手順 5 で `tcpdump` が出す最初のフレームは、次のようになります。

```
02:00:00:00:00:01 > 02:00:00:00:00:02, ethertype Unknown (0x88b5), length 60:
02:00:00:00:00:02 > 02:00:00:00:00:01, ethertype Unknown (0x88b5), length 60:
```

手順 6 のコマンド:

```sh
sudo ip link del vsil0
sudo setcap -r build/<preset>/bridges/ethernet/vehicle_sil_ethernet_bridge
```

- 手順 2 をしないと、実行は `connect-failed` で失敗し、ブリッジの行が、実行する `setcap` のコマンドを書きます。
- ここでランチャーがブリッジに渡す backend は、Linux の既定の `packet` です。自分で作った TAP デバイス（[最初の接続](#最初の接続)）をつなぐなら、先に backend を書きます（`--connect ETH=tap:vsil0`）。Windows の既定は `npcap` です（simulation-config.md の「[`--connect`](simulation-config.md#--connect)」）。
- NIC の先の本物の機器も同じようにつなぎます。`vsil0` の代わりに `--connect ETH=<NIC の名前>` を渡します。機器と一緒に自動で試験するなら、シナリオを `--speed 1 --strict` で実行します（simulation-run.md の「[バスを外につなぐ](simulation-run.md#バスを外につなぐ--connect)」）。
- テスト `ethernet_bridge.netns.launch` は、同じことをネットワークの名前空間の中でします。そこでは `sudo` も `setcap` も要りません（「[`ethernet_bridge.netns.*`](#ethernet_bridgenetns)」）。

## 終了コード

終了コード（0、1、3、4）と participant の状態の扱いは、すべてのブリッジで同じです（bridge-contract.md の「[終了コード](bridge-contract.md#終了コード)」）。このブリッジでは、終了コード 3 は、この OS で使えないバックエンド、インストールされていない pcap のライブラリ、開けないインターフェース、準備のときの SIL Kit の失敗です（`FAIL setup: <理由>`）。終了コード 1 には、外部のインターフェースが動かなくなったこと（なくなった、読み取りの失敗）も入ります（`FAIL: <理由>`）。

## 起動から停止までの動作

ブリッジは、次の順に動きます。

| 順 | すること | 失敗したとき |
|---|---|---|
| 1 | 引数を調べます。 | エラーの 1 行と、使い方の見方の 1 行だけを標準エラーに出力して、終了コード 4 で終わります。 |
| 2 | `--backend` がこの OS で使えるかを調べます。 | SIL Kit に接続する前に、理由と代わりのバックエンドを示して終了コード 3 で終わります。 |
| 3 | SIL Kit の registry に接続し、Ethernet のコントローラーを作ります。 | 接続しようとしたアドレスと次にすることを示して、終了コード 3 で終わります。 |
| 4 | 外部のインターフェースを開き、転送を始めます（`EthernetBridge::start()`）。 | 何が起きたかと直し方を示して、終了コード 3 で終わります。 |
| 5 | participant の lifecycle を始めます。 | — |
| 6 | 起動の 1 行を出力し、止める要求か失敗まで動きます。 | — |

- 順 2 で、`npcap` と `pcap` はライブラリを読み込みます。インストールされていなければ、インストールの方法を示して終了コード 3 で終わります。
- 順 4 のメッセージは、「[バックエンド](#バックエンド)」の各節にあります。
- 転送（順 4）を lifecycle（順 5）より先に始めるのは、ほかの participant が、ブリッジが Running になったのを見た時点で送信を始めるからです。
- 引数のエラー（`error: <理由>` と使い方の見方の 1 行。たとえば `error: --backend must be tap, packet, npcap or pcap, got: usb`）と、registry に接続できないときのメッセージは、すべてのブリッジで同じ形です（bridge-contract.md の「[ブリッジが出す行](bridge-contract.md#ブリッジが出す行)」）。

### 起動の 1 行

起動の 1 行は、何をどこに接続したか、何を通すか、プロミスキャスと MAC のルールの状態を示します。

```
[eth-bridge] bridging SIL Kit network 'ETH1' (participant 'EthernetBridge', silkit://127.0.0.1:8500) <-> packet 'enp3s0' (network interface): EtherType 0x88b5 only, both ways; promiscuous on; MAC guard on; Ctrl+C to stop
```

| 部分 | 表示 |
|---|---|
| 何を通すか | `every frame, both ways`、`EtherType 0x88b5,0x0800 only, both ways`、`SIL Kit MAC 02:00:00:00:00:01 only, both ways`、または後の 2 つを `and` で結んだもの |
| 効いているプロミスキャス | `promiscuous on`。`--no-promiscuous` では `promiscuous off (only frames to the interface's own address and to group addresses arrive)`、インターフェースが受け付けなかったときは `promiscuous off (the interface does not accept it: only frames to its own address and to group addresses arrive)`（`tap` では表示しません） |

### 実行中の出力

| 出力 | 形 | いつ |
|---|---|---|
| 警告 | `[eth-bridge] warning: <文>` | 1 秒に 1 回まで |
| 監視の行（「[監視の行](#監視の行)」） | `[eth-bridge] interface <名前>: ...` | 1 秒に 1 回まで |
| リンクの行（「[リンクの行](#リンクの行)」） | `[eth-bridge] link link=<値> device=<ハッシュ> marked=<0/1> shared=<数>` | SIL Kit のライフサイクルを始める前に 1 回、その後は値が変わったとき |
| 統計の行。`--stats-every-ms` のとき（「[統計](#統計)」） | `[eth-bridge] stats silkit_to_ext=<数> ...` | 最初の間隔で 1 回、その後は数が変わった間隔ごと |

### 停止

止める要求は、Ctrl+C、Ctrl+Break、SIGTERM、Windows でコンソールを閉じたときです。ブリッジは、`stopping`、統計の 1 行（「[統計](#統計)」）、最後のエラーがあれば `last error: <文>`、`stopped` の順に出力して終わります。止める直前に出た警告は、`stopping` の後、統計の前に出力します。

止める順は次のとおりです。

| 順 | すること |
|---|---|
| 1 | SIL Kit のハンドラーを解除します。 |
| 2 | 外部の受信を止めます（インターフェースを閉じます）。 |
| 3 | 統計を出力します。 |
| 4 | lifecycle を止めます。 |

### プロミスキャス

プロミスキャスなしのネットワークアダプターは、自分の MAC アドレス宛てとグループのアドレス宛てのフレームしか渡しません。SIL Kit 側の機器は自分のアドレスを持つので、それらへのユニキャストは、プロミスキャスのときだけ届きます。そのため、ブリッジは既定でプロミスキャスを使います（`packet`、`npcap`、`pcap`。TAP デバイスは、もともと全部のフレームを受け取ります）。

| 引数 | プロミスキャス |
|---|---|
| どちらもなし（既定） | インターフェースが受け付けるなら on。受け付けなければ、プロミスキャスなしで動き、警告を 1 つ出します（下）。 |
| `--promiscuous` | on。インターフェースが受け付けなければ、起動に失敗します（終了コード 3）。 |
| `--no-promiscuous` | off。 |

インターフェースがプロミスキャスを受け付けないときの警告です（括弧の中は OS かライブラリの文）。

```
interface 'Wi-Fi' does not accept promiscuous mode (result 2, not supported), so the bridge runs without it there: only frames to the interface's own address and to group addresses arrive, and frames to the devices of the SIL Kit side (they have addresses of their own) do not. If devices outside must reach them, use an adapter that accepts promiscuous mode (a wired one usually does).
```

どちらで動いているかは、起動の 1 行が示します（「[起動の 1 行](#起動の-1-行)」）。受け付けないことの現れ方は、バックエンドで違います。

| バックエンド | ブリッジが分かる拒否 | 分からない拒否 |
|---|---|---|
| `packet` | カーネルがソケットの設定をエラーで拒む。 | 設定を受け取って無視するドライバー（無線のドライバーの一部）。ブリッジはプロミスキャスとみなします。 |
| `pcap` | 対応していないという libpcap の警告（result 2）か、このユーザーには使えないというエラー（result -11）。 | `packet` と同じです。 |
| `npcap` | プロミスキャスで開くと失敗し、なしで開くと開ける。 | プロミスキャスで開けて、何も言わずに無視するアダプター（Npcap の開き方は警告を伝えません）。 |

分からないときの既定は、`--promiscuous` と同じに動きます。起動の 1 行は `promiscuous on` で、下のヒントは出ません。

### プロミスキャスなしのときのヒント

プロミスキャスなし（`packet`、`npcap`、`pcap` で、`--no-promiscuous` か、インターフェースが受け付けなかったとき）で、ブリッジが外部へフレームを送信してから 5 秒たっても、SIL Kit 側の MAC アドレス宛てのフレームを 1 つも受信しないとき、ブリッジはヒントを 1 回だけ警告します。SIL Kit 側の MAC アドレスとは、外部へ送信したフレームの送信元です。

```
no frame to a MAC address of the SIL Kit side has come from packet 'enp3s0' (network interface) in the 5 s since the bridge first sent a frame there, and promiscuous mode is off (--no-promiscuous): without promiscuous mode an interface passes on only frames to its own address and to group addresses, so frames to the SIL Kit devices (they have other addresses) do not arrive. If devices outside are meant to answer, leave out --no-promiscuous.
no frame to a MAC address of the SIL Kit side has come from packet 'enp3s0' (network interface) in the 5 s since the bridge first sent a frame there, and the interface runs without promiscuous mode (it does not accept it, see the warning at the start): without promiscuous mode an interface passes on only frames to its own address and to group addresses, so frames to the SIL Kit devices (they have other addresses) do not arrive. If devices outside are meant to answer, use an adapter that accepts promiscuous mode (a wired one usually does).
```

1 つ目は `--no-promiscuous` のとき、2 つ目はインターフェースがプロミスキャスを受け付けなかったときの文です。プロミスキャスのときは、ヒントを出しません。

警告するのは、プロミスキャスなしのインターフェースが、自分のアドレス宛てとグループのアドレス宛てのフレームしか渡さず、SIL Kit の機器のアドレスはインターフェースのアドレスと違うからです。レスポンスを待たない使い方（外部へ流すだけ）でも出るので、文は条件つきです。

ブリッジが受信したときには、まだ SIL Kit 側の MAC アドレスと分かっていなかったフレーム（ブリッジが最初に送信するより前に受信したものを含む）も数えます。ブリッジは、そうしたフレームの宛先（グループのアドレスを除く）を覚えておき、後でそのアドレスを送信元にして送信すれば、そのアドレス宛てのフレームを受信していたとみなします。覚える宛先は 32 個までで、それより多い分は覚えません（その分は、受信していたとはみなしません）。上限があるのは、この PC の OS が同じインターフェースから送信したフレームもブリッジが受信するので、宛先の数が多いことからは、インターフェースが宛先でフィルターをかけないとは言えないためです。

### 機器から何も来ないときのヒント

物理のアダプターにフレームを出せるバックエンド（`packet`、`npcap`、`pcap`）で、ブリッジが外部へ最初にフレームを送信してから 10 秒たっても、そこの機器からフレームが 1 つも来ないとき（`ext_from_devices` が 0 のまま。「[統計](#統計)」）、ブリッジはヒントを 1 回だけ警告します。ケーブルが挿さっていない、機器の電源が入っていない、アダプターを取り違えた、のどれもこう見え、それでも送信は成功します。たとえば Hyper-V の `vEthernet` のアダプターは、その物理 NIC にケーブルがなくても接続済みのままです。

```
nothing has come from a device on packet 'enp3s0' (network interface) in the 10 s since the bridge first sent a frame there (frames from the addresses of this computer's adapters do not count): if a device is meant to answer, check that it is on, its cable, and that this is the adapter it is on.
nothing has come from a device on packet 'enp3s0' (network interface) in the 10 s since the bridge first sent a frame there (the bridge does not know the addresses of this computer's adapters, so frames this computer sends there would count): if a device is meant to answer, check that it is on, its cable, and that this is the adapter it is on.
```

- この PC がそこへ送信するフレーム（ARP、IPv6）は、機器がいてもいなくても来るので、数えません。この PC のどれかのアダプターの MAC アドレスからのフレーム（ブリッジのアダプター、そのアダプターに結んだスイッチの `vEthernet` のアダプター、そのアダプターを足にする Linux のブリッジ、その上の macvlan）と、`packet` では、カーネルがこの PC の送信と印を付けたフレームを、アドレスによらず数えません。ブリッジはアドレスを、起動のときに 1 回だけバックエンドから知ります（後から増えた・変わったアダプターは入りません）。`packet` はカーネルから、`pcap` は libpcap のインターフェースの一覧から、`npcap` は Windows から（すべてのアダプターの物理アドレス）です。1 つ目は知っているとき、2 つ目は知らないときの文です。
- 同じ仮想スイッチのほかの仮想マシンのフレームは数えます。ブリッジは機器のフレームと見分けられず、ケーブルがなくても来ることがあります。
- 時間は、外部へ最初に送信したときから数えます。それより前に機器から来たフレームもヒントを止め、機器からフレームが 1 つ来たら、ヒントは出ません。
- レスポンスを待たない使い方（外部へ流すだけ）でも出るので、文は条件つきです。
- `tap` ではヒントを出しません。TAP デバイスはこの PC の中にあるからです。
- Vector XL の CAN のブリッジも、そのチャンネルについて同じヒントを出します（[bridges.md](bridges.md#統計)）。

## バックエンド

| `--backend` | OS | 相手 | 仕組み | 必要な権限 |
|---|---|---|---|---|
| `tap` | Linux | 同じ PC の OS のネットワークスタックと、その TAP デバイスを開くソフト（Wireshark など） | `/dev/net/tun` の TAP デバイス | 先に作った自分が所有者のデバイスなら不要。`--tap-create` は `CAP_NET_ADMIN` |
| `packet` | Linux | ネットワークのインターフェース（NIC、veth）の先の機器と、同じ PC で同じインターフェースを packet ソケットや pcap で開くソフト | インターフェースにバインドした packet ソケット（`AF_PACKET`） | `CAP_NET_RAW` |
| `pcap` | Linux | `packet` と同じ | この PC にインストールされている libpcap を実行時に読み込む | `CAP_NET_RAW` |
| `npcap` | Windows | ネットワークのアダプター（NIC、Microsoft KM-TEST Loopback Adapter）の先の機器と、同じ PC で同じアダプターを Npcap で開くソフト | この PC にインストールされている Npcap（`wpcap.dll`）を実行時に読み込む | ふつうは不要。Npcap を「管理者だけ」の設定でインストールした PC では管理者 |

- `packet` は、同じ PC の OS のネットワークスタックを相手にできません（「[同じ PC の OS のネットワークスタックと通信する](#同じ-pc-の-os-のネットワークスタックと通信する)」）。
- `tap` と `packet` は Linux のカーネルの API だけを使い、外部のライブラリは不要です。
- `pcap` と `npcap` は、PC にインストールされているライブラリを実行時に読み込みます。vehicle-sil はそのライブラリを同梱せず、ビルドにも使いません（[dependencies.md](dependencies.md)）。
- Linux では、ふつうは `packet` を使います。インストールするものがなく、チェックサムが未完成のフレームを見分けられるからです。`pcap` は、同じインターフェースを libpcap で開きたいときのものです。

ブリッジは、インターフェースに IP アドレスを設定することも、up にすることもしません。インターフェースが down だと、起動のときに 1 回警告し、up にする方法を示します。

```
interface 'vsil0' is down: no frame passes until it is up. Bring it up with 'sudo ip link set vsil0 up'.
```

### インターフェースの一覧（`--list-interfaces`）

`--list-interfaces` は、バックエンドのインターフェースを表示します。`packet`（Linux の既定）と `tap` では、この PC のインターフェースを、イーサネットかどうかと up かどうかを付けて表示します。

```
interfaces of this computer (--backend packet and --backend pcap take the Ethernet ones; --backend tap takes a TAP device):
  --interface lo      not Ethernet (link type 772), up
  --interface enp3s0  Ethernet, up, connected
  --interface vsil0   Ethernet, down
```

いま vehicle-sil のブリッジが使っているインターフェースは、行の終わりに `; in use by a vehicle-sil bridge (process 4321)` が付きます（bridge-contract.md の「[別のブリッジが使っているデバイス](bridge-contract.md#別のブリッジが使っているデバイス)」）。`npcap`（Windows の既定）と `pcap` では、pcap のライブラリが返すインターフェースを表示します。

```
interfaces Npcap offers (Npcap version 1.80, based on libpcap version 1.10.5); --backend npcap takes the Ethernet ones:
  --interface "Ethernet 2"          up, connected  (Microsoft KM-TEST Loopback Adapter; \Device\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318})
  --interface Ethernet              up, disconnected  (Intel(R) Ethernet; \Device\NPF_{0A1B2C3D-0000-1111-2222-333344445555})
  --interface \Device\NPF_Loopback  up, loopback  (Adapter for loopback traffic capture)
```

| 行の部分 | 内容 |
|---|---|
| 先頭 | `--interface` に渡す名前です（空白のある名前は `"` で囲んであります）。 |
| その後ろ | ライブラリが返す状態です（`up` / `down`、`connected` / `disconnected`、`wireless`、`loopback`）。 |
| 括弧の中 | 説明と pcap の名前です。 |

- 名前の幅は、コンソールの桁で数えてそろえます。全角の文字（日本語、中国語、韓国語、全角の英数字など）は 2 桁と数えます。コンソールのフォントがその文字を 2 桁の幅で描けば、次の列がそろいます。
- イーサネットでないもの（Linux の `any` など）も表示します。開くときに拒否します。
- Npcap の一覧にネットワークのアダプターが 1 つもない（`\Device\NPF_Loopback` だけ）ときは、最後に 1 行、Npcap の「管理者だけ」のインストールと無効のアダプターのことを追加します（`Npcap offers no network adapter here: ...`）。

#### 一覧の JSON（`--json`）

`--list-interfaces --json` は、同じ一覧を bridge-contract.md の「[デバイスの一覧](bridge-contract.md#デバイスの一覧)」の形で書きます。ここには違う所と例を書きます。

```json
{
  "vehicle_sil_targets": 1,
  "tool": "vehicle_sil_ethernet_bridge",
  "backend": "npcap",
  "source": "Npcap version 1.80, based on libpcap version 1.10.5",
  "targets": [
    {
      "value": "Ethernet 2",
      "label": "Ethernet 2",
      "detail": "up, connected; Intel(R) Ethernet; \\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}",
      "usable": true,
      "reason": null,
      "in_use": false,
      "in_use_by": [],
      "in_use_count": 0,
      "ethernet": {
        "pcap_name": "\\Device\\NPF_{0A1B2C3D-0000-1111-2222-333344445555}",
        "connection": "Ethernet 2",
        "description": "Intel(R) Ethernet",
        "up": true,
        "connected": true,
        "loopback": false,
        "wireless": false
      }
    },
    {
      "value": "\\Device\\NPF_Loopback",
      "label": "\\Device\\NPF_Loopback",
      "detail": "up, loopback; Adapter for loopback traffic capture",
      "usable": false,
      "reason": "loopback: not an Ethernet adapter",
      "in_use": false,
      "in_use_by": [],
      "in_use_count": 0,
      "ethernet": {
        "pcap_name": "\\Device\\NPF_Loopback",
        "connection": null,
        "description": "Adapter for loopback traffic capture",
        "up": true,
        "connected": null,
        "loopback": true,
        "wireless": false
      }
    }
  ],
  "notes": [],
  "error": null
}
```

| キー | このブリッジでの内容 |
|---|---|
| `tool` | `vehicle_sil_ethernet_bridge` です。 |
| `backend` | 一覧のバックエンドです。`--backend`、なければ既定です。 |
| `source` | `npcap`、`pcap` では、ライブラリが返す版です。`packet`、`tap` では `null` です。 |
| `targets[].detail` | 使用中のデバイスでは、`detail` の終わりに `in use by a vehicle-sil bridge (process 4321)` も付きます。 |
| `targets[].ethernet` | バックエンドが返す内容です。`npcap`、`pcap` では `pcap_name`、`connection` と `description`（ないときは `null`）、`up`、`connected`（ライブラリが言わないときは `null`）、`loopback`、`wireless` です。`packet`、`tap` では `name`、`ethernet`、`link_type`、`up` です。 |
| `notes` | 文の一覧の最後の行の説明です。1 文ずつ入ります（ネットワークのアダプターを返さない Npcap）。ほかのときは空です。 |

`value` は、装置に接続名があり、一覧のほかの装置に同じ接続名がなければ、その接続名です。同じかどうかは、`--interface` と同じく、ASCII の英字の大文字と小文字を区別せずに比べます（「[`--interface` に書ける名前](#--interface-に書ける名前)」）。そうでなければ pcap の名前です。`packet` と `tap` では、インターフェースの名前です。

| インターフェース | `usable` | `reason` |
|---|---|---|
| ループバック（ライブラリのフラグ） | `false` | `loopback: not an Ethernet adapter` |
| イーサネットでないもの（`packet`、`tap`） | `false` | `not Ethernet (link type <n>)` |
| libpcap の `any` | `false` | `not Ethernet` |
| down、disconnected | `true` | `null`。`detail` に出ます。ブリッジは起動のときに警告します |
| ほか | `true` | `null` |

pcap のライブラリは、開く前には、装置がイーサネットかどうかを言いません。そのため、ループバックと `any` のほかは、イーサネットでない装置も使えるものとして出ます。その装置は、ブリッジが起動のときに拒否します。`tap` の一覧は、この PC のインターフェースです。ブリッジが開けるのは、その中の TAP デバイスだけです。

一覧を作れないときの JSON は、`targets` が空で、エラーが入ります（終了コードと `FAIL setup:` の行の行き先は、約束のページにあります）。

```json
{
  "vehicle_sil_targets": 1,
  "tool": "vehicle_sil_ethernet_bridge",
  "backend": "npcap",
  "source": null,
  "targets": [],
  "notes": [],
  "error": {
    "code": "library-missing",
    "message": "Npcap is not installed: cannot load C:\\Windows\\System32\\Npcap\\wpcap.dll (Windows error 126). ..."
  }
}
```

| `error.code` | いつ |
|---|---|
| `library-missing` | pcap のライブラリが入っていない、またはブリッジに要る関数がない。 |
| `permission-denied` | OS がこの利用者に断った。 |
| `backend-unavailable` | そのバックエンドがこの OS で使えない（Linux で `--backend npcap` など）。 |
| `failed` | ほか。 |

### `tap`

TAP デバイスは PC の中の仮想のイーサネットのインターフェースで、ブリッジはその回線の向こう側になります。ブリッジが送信したフレームは、OS がそのインターフェースで受信したフレームになります。OS やほかのソフトがそのインターフェースに送信したフレームを、ブリッジが受信します。

- 既定では、すでにあるデバイスに接続します。自分を所有者にして作ってあれば（`ip tuntap add ... user $USER`）、権限は不要です。
- `--tap-create` を付けると、ブリッジがデバイスを作ります（`CAP_NET_ADMIN` が必要です）。作ったデバイスは down の状態で、ブリッジが止まるとなくなります。up にするのは利用者です（up になると、監視の行が `link up` と表示します）。
- フィルターの指定は不要です（既定で全部通します）。フレームは PC の中にとどまるからです。その TAP デバイスを OS のブリッジで NIC に接続した場合に外部へ出るフレームは、接続した人の責任です。
- 1 つの TAP デバイスに接続できるソフトは 1 つです。
- OS が TAP デバイスへ渡す前に捨てたフレームは、ブリッジからは見えません（`ext_overruns` は増えません）。OS 側のインターフェースの統計（`ip -s link show <名前>`）で確認します。

開けないときのメッセージです（終了コード 3）。

```
no TAP device 'vsil0'. Make it first: 'sudo ip tuntap add dev vsil0 mode tap user $USER' and 'sudo ip link set vsil0 up'; or let the bridge make it while it runs with --tap-create (needs CAP_NET_ADMIN). Interfaces here: lo, enp3s0.
cannot attach to TAP 'vsil0': Operation not permitted. It belongs to another user. Make it yours first: 'sudo ip tuntap del dev vsil0 mode tap', then 'sudo ip tuntap add dev vsil0 mode tap user $USER' and 'sudo ip link set vsil0 up'.
cannot attach to TAP 'vsil0': Device or resource busy. Another program has it open, and a TAP device serves one program: stop that program, or use another TAP device.
interface 'lo' is not a TAP device. For a network interface use --backend packet; a TAP device is made with 'sudo ip tuntap add dev <name> mode tap user $USER'.
cannot create TAP 'vsil0': an interface with that name exists. Leave out --tap-create to attach to it, or choose another name.
cannot create TAP 'vsil1': Operation not permitted. Creating a TAP device needs CAP_NET_ADMIN: make it first with 'sudo ip tuntap add dev vsil1 mode tap user $USER' and 'sudo ip link set vsil1 up' and leave out --tap-create, or run as root.
cannot open /dev/net/tun: Permission denied. This user may not use TAP devices here: ask the administrator for read and write access to /dev/net/tun, or run as root.
cannot open /dev/net/tun: No such file or directory. The kernel's TUN/TAP driver is not there: load it with 'sudo modprobe tun'.
```

### `packet`

インターフェースにバインドした packet ソケットで、フレームをそのまま送受信します。

- `CAP_NET_RAW` が必要です。実行ファイルに 1 回付与するか（`sudo setcap cap_net_raw+ep <実行ファイル>`）、root で動かします。エラーメッセージは、この実行ファイルのパスを入れたコマンドを示します。
- 物理のネットワークに出うるので、何を通すか（`--ethertype`、`--src-mac`、`--all-frames` のどれか）の指定が必須です。
- 受信するのは、インターフェースが受信したフレームと、同じ PC の OS やほかのソフトがそのインターフェースに送信したフレームです。ブリッジ自身が送信したフレームは受信しません。
- ブリッジが送信したフレームは、回線の先へ出るだけです。受信するのは、回線の先の機器と、同じインターフェースを packet ソケットや pcap で開いているソフトです。同じ PC の OS のネットワークスタック（普通のソケットを使うソフト）は受信しません。
- 受信したフレームの外側の VLAN タグは、カーネルがフレームのバイト列から外して、別の情報として渡します。ブリッジはそれを元の位置に戻すので、タグつきのフレームはタグを保ったまま両方向に通ります。
- プロミスキャス（「[プロミスキャス](#プロミスキャス)」）は、ブリッジが動いている間だけのソケットの設定で、止まると元に戻ります。
- イーサネットでないインターフェース（`lo` など）は開きません。
- OS がソケットへ渡す前に捨てたフレームは `ext_overruns` に数えます。
- 受信のオフロード（GRO、LRO）が有効だと、最大長より長いフレームが見えることがあります。そのフレームは `oversize` として捨てます。困るときは、インターフェースのオフロードを無効にします（`sudo ethtool -K <名前> gro off lro off`）。

開けないときのメッセージです（終了コード 3）。

```
no interface 'eth9' for --backend packet. Interfaces here: lo, enp3s0, vsil0 (run --list-interfaces).
interface 'lo' is not Ethernet (link type 772): --backend packet carries Ethernet frames only. Choose an Ethernet interface (run --list-interfaces).
cannot open a packet socket on 'enp3s0': Operation not permitted. A network interface needs CAP_NET_RAW: run 'sudo setcap cap_net_raw+ep /opt/vehicle-sil/bin/vehicle_sil_ethernet_bridge' once, or run as root.
interface 'wlp2s0' does not accept promiscuous mode (Operation not supported), and --promiscuous asks for it: leave out --promiscuous to let the bridge run without it (it then says so).
```

#### 同じ PC の OS のネットワークスタックと通信する

同じ PC のソフトが、OS の普通のソケット（UDP、TCP）で SIL Kit の機器と通信するなら、`tap` を使います。`packet` では、次の 3 つが起きます。

| 起きること | 統計 | 直し方 |
|---|---|---|
| SIL Kit からのレスポンスを、OS のスタックが受信しません。 | 両方向とも増えます。 | `tap` を使うか、veth の対を使います。 |
| チェックサムが未完成のフレームが SIL Kit へ入ります。 | `ext_checksum_incomplete` | フレームを送信する側のインターフェースで、`sudo ethtool -K <名前> tx off` を実行します。 |
| TCP の長いフレームを捨てます。 | `oversize` | 同じ `tx off` で、分割した後の長さになります。 |

**レスポンスを受信しない理由**: ソフトが送信したフレームはブリッジが受信して SIL Kit へ渡しますが、SIL Kit からのレスポンスはブリッジが回線の先へ送信するだけで、同じ PC の OS のスタックには渡りません。片方向だけ通ります。veth の対の片方をブリッジが開き、もう片方に IP アドレスを付けて使う形なら、両方向に通ります。

**チェックサムが未完成になる理由**: OS は、送信する UDP と TCP のチェックサムの計算を NIC に任せます（送信のチェックサムのオフロード）。packet ソケットは NIC が計算する前のフレームを見るので、チェックサムのフィールドは正しい値ではありません。veth の対の向こうの端から OS が送信したフレームも同じです（計算する NIC がありません）。ブリッジはこのフレームを捨てず、チェックサムを計算することもせず、そのまま渡して `ext_checksum_incomplete` に数え、最初の 1 回だけ警告します。チェックサムを検証する受信側（自分の TCP/IP スタックを持つ ECU）は、このフレームを黙って捨てます。オフロードを無効にするインターフェースは、フレームを送信する側です（veth なら向こうの端）。

**長いフレームが見える理由**: OS は、TCP のデータを分割するのも NIC に任せます（送信の分割のオフロード、TSO、GSO）。packet ソケットには、分割する前の、最大長より長いフレームが見えます。

チェックサムの警告の文です。1 つ目はブリッジのインターフェース自身から OS が送信したフレーム、2 つ目はインターフェースが受信したフレーム（veth の向こうの端から）のときです。

```
interface 'enp3s0': a frame this computer's OS sent on it has no TCP/UDP checksum yet: the OS leaves the checksum to the network adapter (checksum offload), and the bridge sees the frame before that. The bridge passes such frames on as they are, and a receiver in SIL Kit that checks checksums drops them. Turn the offload off with 'sudo ethtool -K enp3s0 tx off'; or, if the peer is this computer's OS, use --backend tap instead. This warning is given once.
interface 'va': a frame received on it has no TCP/UDP checksum: this computer's OS sent it through a virtual interface (the other end of a veth pair) and left the checksum to a network adapter that is not there (checksum offload). The bridge passes such frames on as they are, and a receiver in SIL Kit that checks checksums drops them. Turn the offload off on the interface the frames are sent from (for a veth pair the other end, not 'va'): 'sudo ethtool -K <that interface> tx off'; or, if the peer is this computer's OS, use --backend tap instead. This warning is given once.
```

`tap` には、チェックサムと長さの 2 つは起きません。TAP デバイスから読むフレームは、チェックサムが入っていて、分割した後の長さだからです。

### `npcap` と `pcap`

同じコードが、Windows では Npcap の `wpcap.dll` を、Linux では libpcap を、実行時に読み込んで使います。どちらも vehicle-sil には含まれていません。利用者がインストールします。

| OS | `--backend` | インストールするもの |
|---|---|---|
| Windows | `npcap` | [Npcap](https://npcap.com) |
| Linux | `pcap` | ディストリビューションの libpcap（Debian と Ubuntu は `libpcap0.8`、Fedora は `libpcap`） |

- Npcap の無料版には、使える範囲の制限があります。インストールする前に Npcap のライセンスを読んでください（vehicle-sil はその内容を解釈しません）。
- Linux の `pcap` に `CAP_NET_RAW` が必要なのは、`packet` と同じです。

#### ライブラリを読み込む場所

ライブラリを読み込む場所は決まっていて、変える引数、環境変数、ファイルはありません。

| OS | 読み込むもの |
|---|---|
| Windows | `<システムのディレクトリ>\Npcap\wpcap.dll`（ふつうは `C:\Windows\System32\Npcap\wpcap.dll`）を、絶対パスで読み込みます。 |
| Linux | `libpcap.so.1`、なければ `libpcap.so.0.8` を、システムのローダーが探すとおりに読み込みます（ほかの共有ライブラリと同じ扱いです）。 |

Windows での DLL の探し方は、次のとおりです。

- `wpcap.dll` が使う DLL（`Packet.dll`）は、`wpcap.dll` と同じディレクトリ、次にシステムのディレクトリからだけ探します。
- `wpcap.dll` もそれが使う DLL も、カレントディレクトリ、`PATH`、実行ファイルのあるディレクトリからは読み込みません。人から受け取ったフォルダに `wpcap.dll` や `Packet.dll` が入っていても、それを読み込まないためです（システムのディレクトリに書き込めるのは管理者だけです）。
- この 2 か所で見つからない DLL があると、Npcap がインストールされていないときと同じメッセージで拒否します。
- `System32` の直下の `wpcap.dll`（WinPcap と互換のインストール）は使いません。

テストはフェイクのライブラリを読み込みますが、そのための仕組みは、テスト用にビルドした別の実行ファイルにだけあります（`tests/ethernet_bridge/fake_pcap_library.cpp`）。インストールする実行ファイルは `bridges/ethernet/app/pcap_library.cpp` をリンクし、場所は空、つまり上の決まった場所です。ライブラリの場所（`eth_link::PcapLinkOptions::library`）は、呼び出すプログラムが決める引数です。

#### `--interface` に書ける名前

| 名前 | 例 | 使える OS |
|---|---|---|
| pcap の名前 | Linux はインターフェースの名前そのもの、Windows は `\Device\NPF_{...}` | Linux、Windows |
| ネットワーク接続の名前（`--list-interfaces` の行の先頭） | `Ethernet 2` | Windows |
| アダプターの識別子 | `{4D36E972-...}` | Windows |

- アダプターの識別子は、波括弧があってもなくても、大文字でも小文字でもかまいません。
- 接続の名前が 2 つのアダプターに一致するときはエラーで、pcap の名前を示します。
- 接続の名前は、UTF-8 の文字列として比較します（引数は UTF-8 で読みます）。Windows と同じく、ASCII の英字の大文字と小文字は区別しません（`ethernet 2` で `Ethernet 2` を選べます）。ほかの文字は同じでなければなりません。大文字と小文字だけが違う接続の名前が 2 つあるときは、どちらとも違う書き方ではエラーになって両方の名前を示し、どちらかをそのとおりに書けば、そのアダプターを選びます。
- pcap の名前は、大文字と小文字も含めてそのとおりに比較します。
- 一覧もエラーメッセージも、コンソールにはコードページによらず読める形で出力し、ファイルとパイプには UTF-8 で出力します（[bridge-contract.md](bridge-contract.md#出力の文字コード) の「出力の文字コード」）。`chcp` は不要です。
- Npcap が返す文（デバイスの説明、エラーの文）は、Windows の ANSI コードページの文として読んで UTF-8 に変換します。
- 日本語の接続の名前は、一覧に出て、一致します（英語の Windows で確認）。ファイルとパイプには正しい UTF-8 で、コンソールには正しい文字で届きます。英語の Windows のコンソールのフォントでは、四角で表示されます。日本語の Windows のコンソールでの見え方は、未確認です。

#### 動作（`npcap` と `pcap` に共通）

- 開くときに、受信したフレームをすぐ渡す設定（immediate mode）と、ブロックしない読み取りを必ず使います。ライブラリがこの設定の関数を持たないほど古ければ、開かずに拒否します。
- 受信は、ライブラリの「フレームがある」という通知（Windows はイベント、Linux は記述子）を待って読みます。通知がなくても、100 ms ごとに 1 回は読みます。通知を出さないライブラリでも、フレームは遅れて渡り、渡らないままにはなりません。
- 通知では 1 つも読めず、100 ms ごとの読み取りでだけフレームが見つかることが 3 回あると、1 回警告します（下の警告の文）。
- 物理のネットワークに出うるので、何を通すか（`--ethertype`、`--src-mac`、`--all-frames` のどれか）の指定が必須です。
- イーサネットでないインターフェースは開きません。
- ライブラリが捨てたフレーム（読むのが間に合わなかった分）は `ext_overruns` に数えます。
- 無線のアダプター（ライブラリが `wireless` と返すもの）では、起動のときに 1 回警告します。

ブリッジは、「[プロミスキャス](#プロミスキャス)」のとおりにライブラリにプロミスキャスを要求します。プロミスキャスを受け付けないアダプターを、本物の Npcap が開けないエラーにするのか、プロミスキャスなしで何も言わずに開くのかは、**未確認**です。

**チェックサムが未完成のフレームは見分けられません。** pcap のライブラリは、そのフラグを渡さないからです。同じ PC の OS が送信したフレーム（「[同じ PC の OS のネットワークスタックと通信する](#同じ-pc-の-os-のネットワークスタックと通信する)」）は、チェックサムのフィールドが正しくないまま SIL Kit へ渡り、`ext_checksum_incomplete` は 0 のままで、警告も出ません。Linux でこれを知りたいときは `packet` を使います。

#### Linux の `pcap` だけの動作

- 受信したフレームの VLAN タグは、libpcap が元の位置に戻して渡します。
- ブリッジが送信したフレームはブリッジに戻りません。
- リンクの up と down は監視の行に表示され、インターフェースがなくなると `FAIL` で終わります（`packet` と同じ）。
- インターフェースが down だと、開けません（`packet` は警告だけで開きます）。

#### Windows の `npcap` だけの動作

| 項目 | 動作 |
|---|---|
| リンクの状態 | リンクの up と down は見ません（監視の行にリンクの状態は表示しません）。 |
| アダプターがなくなったとき | ライブラリの読み取りが失敗したときにだけ分かります。 |
| 自分が送信したフレーム | ブリッジに戻りません（`ExternalLink::echoes_own_transmissions()` は false）。 |
| 無効にしたアダプター | Npcap の一覧に出ないので、`--interface` に書くと「ない」のエラー（`no interface ...`）になります。 |
| 「管理者だけ」の Npcap を、管理者でない利用者が使うとき | Npcap が昇格を求める画面を表示し、その画面を閉じるまでブリッジは止まったままです（`--list-interfaces` も同じ）。 |
| VLAN タグつきのフレーム | 通るかは、アダプターのドライバーとその設定で決まります（ブリッジも Npcap も、タグを変えません）。 |

自分が送信したフレーム: Npcap は、アダプターを開いたソフトに、そのソフト自身が送信したフレームも渡します。ブリッジは、それを渡さない指定（`pcap_open()` の `PCAP_OPENFLAG_NOCAPTURE_LOCAL`）でアダプターを開きます。同じアダプターを開いているほかのソフトが送信したフレームは受信します。`pcap_open` のない `wpcap.dll` は、読み込むときに拒否します（下のメッセージ）。

無効にしたアダプター: エラーは「down」ではなく「ない」になります。このエラーメッセージは、無効のアダプターが一覧に出ないことを示します。

「管理者だけ」の Npcap: 昇格を求める画面を閉じると、一覧には `\Device\NPF_Loopback` しか出ず、アダプターは「ない」のエラーになります。Npcap は権限がないとは返さないので、ブリッジは、一覧にネットワークのアダプターが 1 つもないときに、`--list-interfaces` の一覧の最後の行と「ない」のエラーメッセージでこの場合を挙げます（下のメッセージ）。管理者として起動したコンソールから動かすか、Npcap をその制限なしでインストールし直します。

VLAN タグ: ドライバーが、既定の設定では、VLAN タグつきのフレームを渡さないことがあります。ドライバーの優先度と VLAN の処理を無効にすると受信できますが、今度は、ブリッジが送信したフレームから外側のタグが 1 つ外れて出ることがあります。そのときも送信は成功を返し、統計にも警告にも出ません。VLAN タグを使う前に、そのアダプターで両方向を確認してください。

#### 警告とエラーのメッセージ

警告の文です。

```
interface 'Wi-Fi' is a wireless adapter: wireless adapters and access points often pass only frames from and to the adapter's own MAC address, and the devices of the SIL Kit side have other addresses. If frames do not pass, use a wired adapter.
interface 'va' does not accept promiscuous mode (result 2, not supported), so the bridge runs without it there: only frames to the interface's own address and to group addresses arrive, and frames to the devices of the SIL Kit side (they have addresses of their own) do not. If devices outside must reach them, use an adapter that accepts promiscuous mode (a wired one usually does).
interface 'va' was opened with a warning (result 1).
Npcap does not tell when frames arrive on interface 'Ethernet 2': the bridge finds them by looking every 100 ms, so frames from this interface reach SIL Kit up to 100 ms late. None is lost by this. If the delay matters, install the current Npcap, or try another adapter.
libpcap does not tell when frames arrive on interface 'va': the bridge finds them by looking every 100 ms, so frames from this interface reach SIL Kit up to 100 ms late. None is lost by this. If the delay matters, update libpcap, or use --backend packet.
```

ライブラリが使えないときのメッセージです（終了コード 3。SIL Kit に接続する前に出力します）。

```
Npcap is not installed: cannot load C:\Windows\System32\Npcap\wpcap.dll (Windows error 126). Npcap is not part of vehicle-sil: install it yourself from https://npcap.com (see its licence for the limits of the free edition), then run --list-interfaces.
libpcap is not installed: cannot load libpcap.so.1 or libpcap.so.0.8 (libpcap.so.0.8: cannot open shared object file: No such file or directory). libpcap is not part of vehicle-sil: install your distribution's package (Debian and Ubuntu: 'sudo apt install libpcap0.8'; Fedora: 'sudo dnf install libpcap'), or use --backend packet, which needs no library.
C:\Windows\System32\Npcap\wpcap.dll (WinPcap version 4.1.3) lacks the function pcap_set_immediate_mode, which the bridge needs: it is too old, or it is not Npcap. Install a current one. Npcap is not part of vehicle-sil: install it yourself from https://npcap.com (see its licence for the limits of the free edition), then run --list-interfaces.
```

3 つ目は、足りない関数が 1 つのときの形です。`pcap_open` が足りないときも、関数の名前が変わるだけで同じ文です。

開けないときのメッセージです（終了コード 3）。

```
no interface 'Ethernet 9' for --backend npcap. Interfaces Npcap offers here: "Ethernet 2", Ethernet (run --list-interfaces --backend npcap). Npcap does not offer an adapter that is disabled: enable it in Windows. It does not offer one it is not bound to either, such as the physical adapter of a Hyper-V external switch: give the switch's adapter, "vEthernet (<switch>)", as --interface instead.
no interface 'Ethernet 2' for --backend npcap. Interfaces Npcap offers here: \Device\NPF_Loopback (run --list-interfaces --backend npcap). Npcap does not offer an adapter that is disabled: enable it in Windows. It does not offer one it is not bound to either, such as the physical adapter of a Hyper-V external switch: give the switch's adapter, "vEthernet (<switch>)", as --interface instead. It offers no network adapter here: if Npcap was installed for administrators only, run the bridge from a console started as administrator, or install Npcap again without that limit.
'Ethernet' is the name of 2 interfaces for --backend npcap: \Device\NPF_{4D36E972-E325-11CE-BFC1-08002BE10318}, \Device\NPF_{0A1B2C3D-0000-1111-2222-333344445555}. Give one of these names as --interface instead.
interface 'any' is not Ethernet (pcap link type 113): --backend pcap carries Ethernet frames only. Choose an Ethernet interface (run --list-interfaces --backend pcap).
cannot open 'enp3s0' with libpcap (pcap_activate: result -8, socket: Operation not permitted). A network interface needs CAP_NET_RAW: run 'sudo setcap cap_net_raw+ep /opt/vehicle-sil/bin/vehicle_sil_ethernet_bridge' once, or run as root.
cannot open 'va' with libpcap (pcap_activate: result -9, va: That device is not up). The interface is down: bring it up with 'sudo ip link set va up'.
cannot open 'Ethernet 2' with Npcap (pcap_open: the reason the library gives). Run --list-interfaces --backend npcap to see whether Npcap still offers the adapter.
cannot open 'Ethernet 2' with Npcap (pcap_open: the reason the library gives). Run --list-interfaces --backend npcap to see whether Npcap still offers the adapter. The adapter may not accept promiscuous mode: try without --promiscuous.
interface 'va' does not accept promiscuous mode (result 2, not supported), and --promiscuous asks for it: leave out --promiscuous to let the bridge run without it (it then says so), or use an adapter that accepts promiscuous mode (a wired one usually does).
this user may not open 'va' in promiscuous mode with libpcap (result -11, the reason the library gives), and --promiscuous asks for it: run 'sudo setcap cap_net_raw+ep /opt/vehicle-sil/bin/vehicle_sil_ethernet_bridge' once or run as root, or leave out --promiscuous to let the bridge run without it (it then says so).
```

- 括弧の中の `result` はライブラリの関数の結果で、その後ろはライブラリの文です。
- Npcap の開き方（`pcap_open()`）は結果の番号を返さないので、`npcap` のメッセージにあるのはライブラリの文だけです。上の例の `pcap_open:` の後ろは、ライブラリの文が入る場所を示す仮の文です。
- `npcap` の「ない」のエラーには、無効のアダプターが一覧に出ないことを必ず追加します。
- 一覧にネットワークのアダプターが 1 つもない（`\Device\NPF_Loopback` だけ、または空）ときは、「管理者だけ」のインストールのことも追加します（2 つ目の例）。
- `npcap` の「開けない」のエラーには、`--promiscuous` を付けて開いたときだけ、付けずに試すことを追加します。最後の 2 つは、インターフェースがプロミスキャスを受け付けないときの、`--promiscuous` を付けた `pcap` のエラーです。

#### Windows で PC の中だけで接続する

Windows には TAP デバイスに当たるバックエンドがありません。PC の中のソフトどうしを接続するには、Windows に付属する仮想のアダプター「Microsoft KM-TEST Loopback Adapter」をインストールし、ブリッジと相手のソフトの両方が、そのアダプターを Npcap で開きます。同じアダプターを Wireshark で開けば、SIL Kit から出たフレームを見られます。同じアダプターを Npcap で開いたソフトどうしでは、フレームは両方向に通り、VLAN タグも保たれます。

同じ PC で OS の普通のソケット（UDP、TCP）を使うソフトとは: OS がそのアダプターに送信したフレームはブリッジに届きますが、**ブリッジが送信したフレームは OS に届きません**。ブリッジは、自分の送信を受信しない指定（`PCAP_OPENFLAG_NOCAPTURE_LOCAL`）でアダプターを開きます。このアダプターでは、その指定で開いたハンドルから送信したフレームは OS に届かず、指定なしで開いたハンドルから送信した同じフレームは届きました。

### 実行中にインターフェースの状態が変わったとき

| 起きたこと | バックエンド | ブリッジの動作 |
|---|---|---|
| リンクが down になった、up になった | `tap`、`packet`、`pcap` | 監視の行に `link down`、`link up` を表示し、動き続けます。 |
| インターフェースがなくなった（削除した、名前を変えた）、読み取りが失敗した | 全部 | `FAIL:` の行と統計を出力して、終了コード 1 で終わります。 |

- リンクが down の間に送信して OS が拒否したフレームは、`ext_tx_failures` に数えます。
- `FAIL:` の行は、`FAIL: interface 'vsil0' is gone (removed or renamed while the bridge ran)`、`FAIL: reading from 'Ethernet 2' failed: <ライブラリの文>` などです。

### この OS で使えないバックエンド

```
--backend npcap is for Windows. On Linux use --backend packet or --backend pcap (a network interface), or --backend tap (a TAP device).
--backend tap is for Linux. On Windows use --backend npcap (a network adapter through Npcap).
--backend tap cannot be used here: vehicle_sil_ethernet_bridge has no backend for this operating system (tap, packet and pcap are for Linux; npcap is for Windows). Run the bridge on a Linux or Windows computer that reaches the same SIL Kit registry.
```

3 つ目は Linux でも Windows でもない OS のメッセージで、そこでは `--list-interfaces` も同じ理由（`there is no interface to list here: ...`）で、終了コード 3 で終わります。

## 統計

行の形、キー、キーの順は、bridge-contract.md の「[統計の行](bridge-contract.md#統計の行)」にあります。このブリッジで、キーが数えるもののうち、そこの表より細かい点は次のとおりです。

- 監視の行が出るキー（そこの表の「監視の行」が「出す」）が増えると、監視の行が出ます（`problems_grew()`）。転送した数、フィルターの数、`own_tx_dropped` は問題に数えません。
- `ext_to_silkit` に数えるのは、`SendFrame()` が戻った時点のものです。`silkit_tx_failures` のうち、SIL Kit が送信できなかったと確認したフレーム（`ControllerInactive`、`LinkDown`、`Dropped`、`InvalidFrameFormat`）は、渡した時点で `ext_to_silkit` にも数えてあります。
- `ext_checksum_incomplete` に数えるのは、`packet` で、同じ PC の OS が送信したフレームです（「[同じ PC の OS のネットワークスタックと通信する](#同じ-pc-の-os-のネットワークスタックと通信する)」）。`ext_to_silkit` にも数えてあります。
- `ext_from_devices` で、外の何かが答えているかが分かります。数えるのは、外から来たフレームのうち、この PC が送信したものでなく、送信元が SIL Kit 側のアドレスでないもので、フィルターの前に数えます。数えないもの: この PC が送信したフレーム（この PC のアダプターの MAC アドレスからのものと、`packet` ではカーネルがこの PC の送信と印を付けたもの。「[機器から何も来ないときのヒント](#機器から何も来ないときのヒント)」のとおりで、ブリッジがアドレスを知らなければ数えます）、SIL Kit 側のアドレスからのフレーム（`--src-mac` のアドレスと、MAC のルールがあれば、ブリッジが外部へ送信したフレームの送信元。`own_tx_dropped` と `loop_suspected` が数えるフレーム）、ヘッダーより短いフレームと最大長より長いフレーム、SIL Kit のコントローラーを有効にする前のフレーム。機器からのフレームは、フィルターが SIL Kit へ通さないものでも数えます（`filtered_from_ext` にも数えます）。
- 同じアダプターを使うこの PC のプログラムの答えは数えません。`packet` では送信元のアドレスによらず、`npcap` と `pcap` では、この PC のアダプターのアドレスから送ったとき（そうしたプログラムはふつうそうします）です。
- `tap` では、相手がこの PC のネットワークスタックなので、来たものをこの PC のものとはみなしません。`ext_from_devices` は、SIL Kit 側からでない来たフレームをすべて数え、`ext_to_silkit` と `filtered_from_ext` を合わせたものと同じです。
- `own_tx_dropped` に数えるのは、自分の送信を戻す外部リンクのときだけで、vehicle-sil の外部リンクは、どれも戻しません。`--no-mac-guard` では、SIL Kit 側を見分けるのは `--src-mac` のアドレスだけなので、ブリッジが外部へ送信して戻ってきたフレームは `ext_from_devices` に数えます。

## 監視の行

`format_health_change(before, now, interface, problems_before)` は、呼び出し側が取得した 2 つのスナップショット（`EthernetBridge::health()`）の間の変化を 1 行にします。何もなければ行を作りません。

| 場合 | 行 |
|---|---|
| 問題の数が増えたか、リンクの状態が変わったとき | `interface enp3s0: link down; since the last report: ext_tx_failures +12` |
| 何も増えていないが、前のスナップショットでは増えていたとき（`problems_before`） | `interface enp3s0: link up; no new problems since the last report` |

- 状態不明から `link up` への変化は、健全なリンクの最初の通知なので、行にしません。
- リンクの状態は `link up`、`link down`、`link state unknown` のどれかです。

## リンクの行

行の形は、bridge-contract.md の「[リンクの行](bridge-contract.md#リンクの行)」にあります。このブリッジは `link link=<値>` を出します。

```
[eth-bridge] link link=up device=af3d43e7e4b8883a marked=1 shared=0
```

| 値 | 意味 |
|---|---|
| `up` | リンクが上がっています（`tap`、`packet`、`pcap`: インターフェースが up でキャリアがある） |
| `down` | リンクが下がっています |
| `unknown` | リンクがまだ報告していない、または分かりません。`npcap` はリンクを監視しないので、いつも `unknown` です |

- ブリッジは、インターフェースを開いてから印を置きます（接続名から pcap の名前が分かるのは、Npcap のリンクが開いてからだからです）。同じインターフェースの `packet` と `pcap` は同じデバイスです。
- `link_line_value()` が値を作ります。

## フィルター（`FrameFilter`）

同じルールを両方向に適用します（`vehicle_sil/eth_link/frame_filter.hpp`）。

| 条件 | 設定 | 通すフレーム |
|---|---|---|
| 長さ | 最大長（既定 1522 バイト、FCS なし） | イーサネットのヘッダー（14 バイト）以上で、最大長以下のフレーム |
| EtherType | `FrameFilterOptions::ether_types` | 挙げた EtherType のフレームだけ |
| SIL Kit 側の MAC | `FrameFilterOptions::silkit_macs` | SIL Kit から外部へは送信元が、外部から SIL Kit へは宛先が、そのどれかのフレームだけ |

- 長さ: ヘッダーより短いフレームは `malformed`、最大長より長いフレームは `oversize` として捨てます。
- EtherType: 送信元 MAC の後の 2 バイトが VLAN タグを示す値（TPID。既定は `0x8100` と `0x88A8`）なら、その 4 バイトのタグの後ろを見ます。見るタグは 2 つまでです。タグの途中で終わるフレームは通しません。
- SIL Kit 側の MAC: 外部から SIL Kit へは、宛先がグループのアドレス（ブロードキャスト、マルチキャスト）のフレームも通します。グループのアドレスは `silkit_macs` に挙げられません。
- EtherType と MAC の両方を挙げたときは、両方を満たすフレームだけを通します。どちらも挙げなければ、長さのほかはフィルターをかけません。

> [!NOTE]
> VLAN の TPID の既定（`0x8100`、`0x88A8`）、最大長の既定 1522 バイト、EtherType の位置は、一般の資料の値で、規格では確認していません。どれも `FrameFilterOptions`（引数では `--vlan-tpid`、`--max-frame-bytes`）で変えられます。テストと例が使う EtherType `0x88B5` は、ローカルの実験用として知られた値で、これも規格では確認していません。プロトコルの EtherType はライブラリに入れていないので、利用者が挙げます。

SIL Kit は、60 バイトより短いフレームを受信側で 0 で埋めます（[dependencies.md](dependencies.md) の「実装が前提にしている上流の挙動」）。そのため SIL Kit から来るフレームは 60 バイト以上で、外部から来た短いフレームは、埋められて SIL Kit 側の相手に渡ります。

## ループの防止（`MacSideTable`）

同じ 2 つのネットワーク（SIL Kit の Ethernet ネットワークと外部の回線）の間には、ブリッジを 1 つだけ置きます。2 つ置くのは誤った構成で、フレームが 2 つのブリッジを回り続けることがあります。ブリッジは、フレームの送信元 MAC から、見つけられる場合にこの構成を見つけ、そのアドレスのフレームを止めて警告します。どの場合でも止めるとは保証しません（「[ループが止まらない場合](#ループが止まらない場合)」）。OS の設定には頼りません（`vehicle_sil/eth_link/mac_side_table.hpp`）。

### MAC のルール

| 受信したフレーム | ブリッジの動作 |
|---|---|
| SIL Kit から来て、フィルターを通ったフレーム | 送信元を「SIL Kit 側」として覚えます。 |
| 外部から来て、フィルターを通ったフレーム | 送信元を「外部側」として覚えます。 |
| SIL Kit から来た、送信元が外部側のフレーム | そのアドレスを「両側にある」とします。 |
| 外部から来た、送信元が SIL Kit 側のフレーム（外部リンクが自分の送信を戻さない） | そのアドレスを「両側にある」とします。 |
| 外部から来た、送信元が SIL Kit 側のフレーム（外部リンクが自分の送信を戻す） | 自分が送信したフレームの戻りとして捨て、`own_tx_dropped` に数えます。 |
| 送信元が両側にあるアドレスのフレーム | 以後両方向で捨て、`loop_suspected` に数えます。 |
| 送信元がグループのアドレス（ブロードキャスト、マルチキャスト）のフレーム | 覚えず、調べずに通します。 |

- `--src-mac`（`FrameFilterOptions::silkit_macs`）で挙げたアドレスは、ブリッジを作るときに SIL Kit 側として登録しておきます。
- フィルターで通さなかったフレームでは、送信元を覚えません。
- 「両側にある」とは、同じネットワークの間に 2 つのブリッジがあるか、同じ MAC アドレスが両側にあるということです。
- 外部リンクが自分の送信を戻すかどうかは、`echoes_own_transmissions()` で分かります。戻さない（`false`）なら、外部から来たフレームは自分の送信したものではありえません。戻す（`true`）ときに捨てるフレームでは、警告は出しません。
- 両側にあるアドレスの警告は、アドレスごとに 1 回、最初の 8 つまでアドレスを挙げて出力し、それより多いときは 1 回だけまとめて出力します。
- 両側にあるという記録は、ブリッジを起動し直すまで消えません。構成を直したら（ブリッジを 1 つにしたら）、残ったブリッジも起動し直します。

### ループが止まる場合

次の 3 つは、テストが確認しています。テストは、フェイクの外部リンクとフェイクのコントローラーの 2 つのブリッジで行います。`npcap` の開き方は、フェイクの pcap のライブラリの同じデバイスに 2 つのブリッジを接続して確認しています。

- 自分の送信を戻さない外部リンクでは、SIL Kit の機器が始めたループも、外部の機器が始めたループも、コピーを受信する順序によらず止まります。ループが続くのは、遅れた側のブリッジが、もとのフレームを自分の側で受信するまでの間です。受信した時点で、そのアドレスが両側にあると分かります。
- 自分の送信を戻さない外部リンクでは、片方のブリッジだけが `--no-mac-guard` でも、ルールを使う側のブリッジが見つけて止め、警告します。ルールを使わない側も、自分が送信したばかりのフレームが外部から来たことを 1 回警告することがあります（「[`--no-mac-guard` を指定したとき](#--no-mac-guard-を指定したとき)」）。
- 自分の送信を戻す外部リンクでも、SIL Kit の機器が始めたループは止まります。アドレスを外部で先に見たブリッジが、SIL Kit から来たフレームで両側にあると分かるからです。

### ループが止まらない場合

「自分の送信を戻す外部リンク」は、`ExternalLink` を自分で実装し、戻すと申告した場合です。vehicle-sil の外部リンクは、どれも戻しません。

- 自分の送信を戻す外部リンクで、外部の機器が始めたループ。
- 自分の送信を戻す外部リンクで、片方が `--no-mac-guard` の場合。ルールを使う側は、相手のコピーを自分の戻りとして捨てます。
- 両方のブリッジが `--no-mac-guard` の場合。
- MAC テーブルがいっぱいで覚えられなかったアドレス（`mac_unguarded` に数える）と、グループの送信元アドレスのフレーム。

1 つ目の場合に起きることは、次のとおりです。ブリッジ 1 が外部のフレームを SIL Kit に入れ、ブリッジ 2 がそれを自分の外部側のコピーより先に SIL Kit から受信すると、ブリッジ 2 はそのアドレスを SIL Kit 側と覚え、後から来る外部側のコピーを自分の戻りとして捨てます。どちらのブリッジもアドレスを両側に見ず、ループは続き、警告も出ません（テストはこの事実も確認しています）。この外部リンクでは、自分の戻りと、別のブリッジや同じアドレスの別の機器から来たフレームを、送信元だけからは見分けられません。

### 外部リンクが自分の送信を戻すかどうか

外部リンクが自分の送信を戻すかどうかは、外部リンクの実装が申告します。vehicle-sil の外部リンクは、どれも「戻さない」と申告します。`tap`、`packet`、`pcap` は、Linux のカーネルと libpcap で確認した挙動です（テスト `ethernet_bridge.netns.*`）。

| バックエンド | 挙動 |
|---|---|
| `tap` | ブリッジが TAP デバイスに書き込んだフレームは OS へ行き、ブリッジの読み取りには戻りません。 |
| `packet` | ブリッジのソケットが送信したフレームは、そのソケットには戻りません。 |
| `pcap` | libpcap のソケットも `packet` と同じです。 |
| `npcap` | Npcap は自分の送信を戻しますが、ブリッジは戻さない指定でアダプターを開きます（「[`npcap` と `pcap`](#npcap-と-pcap)」）。 |

- `packet` では、同じインターフェースのほかのソケットが送信したフレームは受信します。
- `npcap` では、同じアダプターを開いたほかのソフトが送信したフレームは受信するので、同じアダプターに接続した 2 つ目のブリッジも見つけられます。

そのため、これらの外部リンクでは「ループが止まる場合」の全部が当てはまり、SIL Kit 側のアドレスを送信元に持つフレームが外部から来たら、そのアドレスは両側にあるものとして止めて警告します。同じ PC で、SIL Kit の機器と同じ MAC アドレスを送信元にして同じインターフェースに送信するソフトがあると、この警告になります。

**外部リンクが「戻さない」と申告しているのに、自分の送信を戻す場合**（たとえば、戻さない指定が効かない Npcap やアダプター。あるかどうかは未確認です）も、ブリッジからは、上と同じ「SIL Kit 側のアドレスを送信元に持つフレームが外部から来た」ように見えます。

- MAC のルールを使っていると、SIL Kit の機器が最初のフレームを送信したところで、その戻りを外部から受信し、その機器のアドレスが両側にあるものとして止まります（止まる向きです）。そのため、SIL Kit 側のアドレスを外部で見たときの警告は、最後に、2 つ目のブリッジも同じアドレスの機器もないなら外部リンクが自分の送信を戻していることと、次にすることを示します（「[警告のメッセージ](#警告のメッセージ)」の 2 つ目）。次にすることは、`npcap` なら Npcap を新しくする、別のアダプターを試す、直らなければ vehicle-sil に報告する、です。
- `--no-mac-guard` のときは、「[`--no-mac-guard` を指定したとき](#--no-mac-guard-を指定したとき)」のとおり、1 回警告します。

### 同じ MAC アドレスの機器が両側にあるとき

両側にあるアドレスのフレームを両方向で捨てるので、同じ MAC アドレスを持つ機器が両側に本当にあると、そのアドレスのフレームは通らなくなります。警告がアドレスを挙げるので、誤った構成として見つけられます。ただし、自分の送信を戻す外部リンクでは、SIL Kit 側で先に覚えたアドレスを外部で見ても、戻りと区別できないので、黙って捨てます。

### MAC テーブルの大きさ

MAC テーブルの大きさは既定 1024 アドレス（`--mac-table-size`、16〜65536）で、メモリを確保するのはブリッジを作るときだけです。いっぱいになると、SIL Kit 側のアドレス（と両側にあるという記録）は、外部側の最も古いアドレスを追い出して入ります。外部側の新しいアドレスは覚えません。覚えられなかったアドレスのフレームは、調べずに通して `mac_unguarded` に数え、最初の 1 回だけ警告します。

### `--no-mac-guard` を指定したとき

`EthernetBridgeOptions::mac_guard` を `false`（引数では `--no-mac-guard`）にすると、MAC のルールを使いません。ループは利用者が防ぎます。

**自分の送信を戻す外部リンクで `--no-mac-guard` にすると**、SIL Kit から出たフレームが SIL Kit に戻ります。自分が送信したフレームの戻りを捨てるのは、MAC のルールだけだからです。

- ブリッジが外部へ送信したフレームは、外部リンクから戻り、外部から来たフレームとしてフィルターを通って SIL Kit へ入ります（`ext_to_silkit` に数えます）。送信した機器は自分のフレームを受信し、ほかの機器は同じフレームを 2 回受信します。ブリッジは SIL Kit から受信したフレームだけを外部へ送信するので、回り続けはしません。
- 自分のアドレスがほかで使われていないかを調べるネットワークスタックは、自分のフレームを見て「使われている」と判断することがあります。
- ブリッジは、外部リンクが「戻す」と申告していてルールを使わないとき、起動のたびに 1 回警告します（「[警告のメッセージ](#警告のメッセージ)」の最後から 2 つ目）。
- ルールを使っていても、同じことが起きるフレームがあります。MAC テーブルがいっぱいで覚えられなかった SIL Kit 側のアドレスのフレーム（`mac_unguarded` に数え、テーブルがあふれた警告が出ます）と、送信元がグループのアドレスのフレームです。どちらも、戻ったときに調べずに通します。
- vehicle-sil の外部リンクに、「戻す」と申告するものはありません。`ExternalLink` を自分で実装した外部リンクが申告した場合の動作です。

**外部リンクが「戻さない」と申告しているときに `--no-mac-guard` にすると**、ブリッジは、外部から来て SIL Kit へ渡すフレームを、自分が最後に外部へ送信したフレームと比較します（長さと、先頭の 64 バイトまで）。同じなら、1 回だけ警告します（「[警告のメッセージ](#警告のメッセージ)」の最後）。フレームは捨てずに SIL Kit へ渡します。ルールを使わないときは、どこから来たかでは何も捨てないからです。統計は変わりません（`ext_to_silkit` に数えるだけです）。

- この警告になるのは、外部リンクが申告に反して自分の送信を戻しているとき、同じネットワークの間に 2 つ目のブリッジがあるとき、外部の機器が受信したフレームをそのまま送り返しているときです。ブリッジからは見分けられないので、文は全部を挙げます。
- 比較するのは最後に送信した 1 つだけです。戻りを受信する前に次のフレームを送信していると、見つけません。外部リンクが戻すときに長さを変える（短いフレームを最小の長さまで埋める）場合も、見つけません。警告が出ないことは、戻っていないことの確認にはなりません。
- 警告は、起動のたびに 1 回までです。

### 警告のメッセージ

警告の文は次のとおりです（テストが文言まで確認しています）。アドレスを先に見た側と、いま見た側（`SIL Kit`、`the external link`）が入ります。SIL Kit 側のアドレスを外部で見たとき（2 つ目）だけ、外部リンクが自分の送信を戻している場合の文が後ろに付きます。

```
MAC 02:00:00:00:00:99 is on both sides: frames from it came from the external link and now come from SIL Kit, so frames from it are dropped both ways (loop_suspected). Run one bridge between these networks and give each device its own MAC address, then restart the bridge: it keeps dropping them until it restarts.
MAC 02:00:00:00:00:01 is on both sides: frames from it came from SIL Kit and now come from the external link, so frames from it are dropped both ways (loop_suspected). Run one bridge between these networks and give each device its own MAC address, then restart the bridge: it keeps dropping them until it restarts. If there is no second bridge and no other device with this address, the external link gives the bridge's own transmissions back although it says it does not: with --backend npcap, install the current Npcap or try another adapter, and report it to vehicle-sil if it stays.
more MAC addresses are on both sides than the first 8 named: frames from them are dropped both ways too (loop_suspected).
the MAC table is full (1024 addresses): frames from addresses that do not fit are forwarded without the check against loops (mac_unguarded); addresses of the SIL Kit side take the places of the oldest addresses outside. Raise --mac-table-size if this many devices are expected.
the MAC guard is off (--no-mac-guard) and this external link gives the bridge's own transmissions back to it: every frame the bridge sends out comes back and is passed into SIL Kit again (counted in ext_to_silkit), so its sender receives its own frame and every other participant receives it twice. A network stack that looks for its own address in use by others then finds it. Run without --no-mac-guard unless this is what you want.
a frame like the one the bridge had just sent out (the same length and first bytes) came from the external link, and with the MAC guard off (--no-mac-guard) it was passed into SIL Kit again (counted in ext_to_silkit). A second bridge or a device outside sends the frames back, or the external link gives the bridge's own transmissions back although it says it does not: with --backend npcap, install the current Npcap or try another adapter, and report it to vehicle-sil if it stays. Run without --no-mac-guard to have such frames dropped and their addresses named. This is said once.
```

## ライブラリの構成

フレームは、SIL Kit の `IEthernetController`、`EthernetBridge`（`FrameFilter` と `MacSideTable` を使います）、`ExternalLink`、外部のインターフェースの順に、双方向に通ります。

| ターゲット | 内容 | 依存 |
|---|---|---|
| `vehicle_sil_ethernet_bridge`（実行ファイル） | ブリッジの CLI です。 | `ethernet_bridge_core`、`bridge_runtime`、`core` です。どのプラットフォームでも、その OS の外部リンクを入れてビルドします（pcap のライブラリは実行時に読み込みます）。 |
| `vehicle_sil::eth_link` | 外部のインターフェースを表す `ExternalLink` と、その実装、フィルター、MAC テーブルです。 | SIL Kit を使いません。 |
| `vehicle_sil::ethernet_bridge_core` | フレームを転送する `EthernetBridge` と、実行ファイルの出力と引数の解釈です。 | SIL Kit と `eth_link` です。プラットフォームに依存しません。 |

`vehicle_sil::eth_link` に入っているものです。

- 外部のインターフェースを表す `ExternalLink`
- MAC アドレス
- フィルター `FrameFilter`
- ループを防ぐテーブル `MacSideTable`
- OS がライブラリなしで提供する API を使う外部リンク（`os_links.hpp`: Linux の TAP デバイスと packet ソケット）
- OS にインストールされている pcap のライブラリを実行時に読み込む外部リンク（`pcap_link.hpp`: Windows の Npcap、Linux の libpcap）

`vehicle_sil::ethernet_bridge_core` に入っているものです。

- SIL Kit の `IEthernetController` と `ExternalLink` の間でフレームを転送する `EthernetBridge`
- 統計と監視の行
- 引数の解釈（`parse_bridge_options()`）
- 実行ファイルが出力するもの（`bridge_app.hpp`: 起動の 1 行、OS ごとに使えるバックエンド、プロミスキャスのヒント）

ソースの場所は、[code-map-bridges.md](code-map-bridges.md#ethernet-bridge) の「Ethernet Bridge」にあります。

## 外部のインターフェース（`ExternalLink`）

`vehicle_sil::eth_link::ExternalLink`（`vehicle_sil/eth_link/external_link.hpp`）は、外部のイーサネットのインターフェース 1 つを表します。このドキュメントでは、その実装を「外部リンク」と書きます。フレームはバイト列で、宛先 MAC アドレスから payload の終わりまでです。FCS は含みません。

| 関数 | 仕様 |
|---|---|
| `start(handler)` | インターフェースを開き、受信スレッドから `handler` に `LinkEvent` を渡し始める。開けなければ `LinkError` を投げる |
| `stop()` | 渡すのをやめる。戻る前に、前の通知の後で OS が捨てたフレームの数を、呼び出したスレッドから `Overrun` で通知してよい。戻った時点で、ハンドラーは動いていない。開始していなくても呼べる |
| `transmit(frame)` | フレームを 1 つ送信する。結果は `Ok`、`QueueFull`（送信のキューがいっぱいで送信しなかった）、`Failed`（ほかのエラー。理由は `last_error()`）。フレームは呼び出しの間だけ読む。ブロックしない |
| `echoes_own_transmissions()` | `transmit()` で送信したフレームが、受信したフレームとしてハンドラーに戻るか。外部リンクの寿命の間は変わらない。ブリッジは `start()` のときに読み、ループの防ぎ方を変える（「[ループの防止](#ループの防止macsidetable)」） |
| `describe()` | 起動の 1 行に入る文字列。種類、名前、物理か仮想か（例: `tap 'vsil0' (TAP device in this computer)`、`packet 'enp3s0' (network interface)`） |
| `this_computer_addresses()` | 外部リンクの側でこの PC が送信に使う MAC アドレス。この PC のすべてのイーサネットのアダプターのもので、`start()` が 1 回だけ調べる。空: 来るものにこの PC のものはない（`tap`）。`nullopt`: 外部リンクが分からない（既定）。ブリッジは `start()` のときに読み、これらのアドレスからのフレームを `ext_from_devices` に数えない。ほかの外部リンクを包む外部リンクは、包んだものの答えをそのまま返す |

### スレッドとロック

- ハンドラーは、一度に 1 つのスレッドからだけ呼ばれます（受信スレッドと、`stop()` を呼び出したスレッド）。
- `transmit()` は、どのスレッドからでも、ハンドラーと並行にも呼べます。
- `transmit()` は、ブロックせず、例外を投げず、受信スレッドを待ちません。
- 外部リンクは、ハンドラーを呼んでいる間、`transmit()` が取るロックを持ってはいけません。
- 受信の時刻は渡しません。OS の時計はシミュレーションの時計と別のものだからです。

ロックの制約があるのは、次の理由です。ブリッジは、SIL Kit がフレームのハンドラーのロックを持ったまま呼ぶ受信スレッドで `transmit()` を呼びます。一方、外部リンクのハンドラーの中で呼ぶ `SendFrame()` は、同じロックを取ります（[dependencies.md](dependencies.md) の「実装が前提にしている上流の挙動」）。外部リンクのロックをハンドラーの間も持つと、2 つのスレッドが互いを待って止まります。

ブリッジは、仕様に反して `transmit()` が投げた例外を捕まえて `ext_tx_failures` に数え、続けます。

### `LinkEvent` と `LinkError`

| `LinkEvent` の種類 | 意味 |
|---|---|
| `Frame` | 受信したフレーム（`frame`。ハンドラーの呼び出しの間だけ有効） |
| `Overrun` | OS が読み取りの前に捨てたフレームの数（`count`。前の通知からの増分） |
| `LinkState` | リンクが up になった、down になった（`state`: `Up`、`Down`、`Unknown`） |
| `Fault` | 外部リンクが動かなくなった（インターフェースがなくなった、読み取りが失敗し続ける）。理由は `detail` |
| `Warning` | 止めるほどではないが利用者に通知すること（`detail`） |

- `Frame` の `sent_here` が真なら、この PC がそのフレームをインターフェースへ送信したものです（`packet` だけが伝えます）。ブリッジは `ext_from_devices` に数えません。
- `Frame` の `checksum_incomplete` が真なら、UDP や TCP のチェックサムが未完成のフレーム（同じ PC の OS が送信し、計算を NIC に任せたもの）です。外部リンクはそのまま渡し、最初の 1 つの前に `Warning` を 1 回出します。ブリッジは渡して `ext_checksum_incomplete` に数えます。
- `Fault` の後は、フレームは渡されず、`transmit()` は失敗します。

`LinkError` の種類（`kind()`）は `NoSuchInterface`、`PermissionDenied`、`LibraryMissing`、`NotEthernet`、`Other` です。`what()` は、何が起きたかと直し方を示します。

### 外部リンクを作る関数

| 外部リンク | ヘッダー | 作る関数 | 使える OS |
|---|---|---|---|
| TAP デバイス | `vehicle_sil/eth_link/os_links.hpp` | `make_tap_link()` | Linux |
| packet ソケット | `vehicle_sil/eth_link/os_links.hpp` | `make_packet_link()` | Linux |
| pcap のライブラリ | `vehicle_sil/eth_link/pcap_link.hpp` | `make_pcap_link()` | Windows、Linux |

packet ソケットと pcap のライブラリは、オプションに `promiscuous`（`eth_link::Promiscuous`: `Off`、`On`、`Auto`）を取ります。ライブラリの既定は `Off` で、何も指定しないプログラムの動きは変わりません。実行ファイルは、`--promiscuous` も `--no-promiscuous` もなければ `Auto` を渡します。`ExternalLink::promiscuous()` は、リンクが始まった後に効いているモードを返し（TAP デバイスのようにモードのないリンクは `nullopt`）、`Auto` は拒否を `Warning` のイベント 1 つで伝えます（`promiscuous_refused_warning()`）。

`os_links.hpp` の外部リンク:

- Linux 以外の OS では `has_os_links()` が `false` で、作る関数は `LinkError` を投げます。
- `list_interfaces()` は、この PC のインターフェースを返します。
- どちらの外部リンクも、受信スレッドが 250 ms ごとにインターフェースを見て、リンクの up と down、なくなったこと、OS が捨てた数を通知します。
- `LinkError` の文は、直し方が引数のときは `vehicle_sil_ethernet_bridge` の引数の名前で示します。

`pcap_link.hpp` の外部リンク:

- 作るときにライブラリを読み込み、なければ `LinkError`（`LibraryMissing`）を投げます。インターフェースを開くのは `start()` です。
- `pcap_interface_list()` は、`--list-interfaces` の文を返します。
- `PcapLinkOptions::library` が空なら PC にインストールされているライブラリ、空でなければそのファイルを読み込みます。テストがフェイクのライブラリを渡すための引数で、実行ファイルは空を渡します。

テスト用のフェイクの外部リンクは `tests/eth_link/fake_link`（`FakeLink` と、フェイクの外部リンクどうしを接続する `FakeSegment`）です。受信スレッドを持ち、送信の詰まり、取りこぼし、`Fault`、リンクの up と down、自分が送信したフレームが戻るインターフェースを模擬します。

## 転送（`EthernetBridge`）

`vehicle_sil::ethernet_bridge::EthernetBridge`（`vehicle_sil/ethernet_bridge/ethernet_bridge.hpp`）は、フレームを書き換えずに両方向に転送します。

| 関数 | 動作 |
|---|---|
| `start()` | 外部リンクを開始し、SIL Kit のフレームのハンドラーを RX の向きだけで登録し、送信の確認のハンドラーを登録します。 |
| `start_controller()` | コントローラーを `Activate()` します。 |
| `stop()` | SIL Kit のフレームのハンドラーを解除し、外部リンクを止め、送信の確認のハンドラーを解除し、コントローラーを `Deactivate()` します。 |
| `fault()` | `Fault` の理由を返します。 |
| `take_warnings()` | 警告を、古い順に返します。 |

- `start()` は、participant の lifecycle を始める前に呼びます。失敗したら、開始したものを戻してから例外を投げます。
- `stop()` は、何度呼んでもかまいません。
- 自分のキューは持ちません。SIL Kit から外部へは SIL Kit の受信スレッドで `transmit()` し、外部から SIL Kit へは外部リンクの受信スレッドで `SendFrame()` します。バックプレッシャーはなく、失ったフレームは数えます。
- `Fault` を受け取っても、ブリッジは動き続けます。止めるかどうかは呼び出し側が決めます。
- 警告は、外部リンクの `Warning` と、ブリッジ自身の警告（「[ループの防止](#ループの防止macsidetable)」）です。

### コントローラーを開始する時点

`start_controller()` は、participant の CommunicationReady のハンドラーから呼びます（`runtime.add_communication_ready_handler([&bridge] { bridge.start_controller(); })`）。SIL Kit が勧めている時点です（SIL Kit のスレッドの上、Running の前）。

- それまでに外部リンクから受信したフレームは、SIL Kit に渡しません（数えません）。SIL Kit から外部への向きは、`start()` から転送します。
- `Activate()` が失敗したら、例外ではなく `fault()` で通知します。
- `start_controller()` の後の `start()`（止めて開始し直す）は、`Activate()` もします。
- ブリッジは autonomous の participant なので、Ethernet を模擬するネットワークシミュレーターをその時点で見つけている保証はありません。

### フレームを調べる順序

| 向き | 順序 |
|---|---|
| SIL Kit から外部へ | 長さ（`malformed`、`oversize`）→ フィルター（`filtered_to_ext`）→ MAC のルール（`loop_suspected`）→ `transmit()` |
| 外部から SIL Kit へ | 長さ（`malformed`、`oversize`）→ MAC のルール（`own_tx_dropped`、`loop_suspected`）→ 機器からか（`ext_from_devices`）→ フィルター（`filtered_from_ext`）→ `SendFrame()` |

外部から来たフレームは、フィルターより先に MAC のルールを適用します。自分が送信したフレームの戻りと、両側にあるアドレスのフレームは、フィルターで通さないものでも、それとして数えるためです。

## 確認したことと、確認していないこと

### 確認したこと（Linux、自動テスト）

テスト `ethernet_bridge.netns.*` が、ユーザー名前空間の中のネットワーク名前空間で、本物の実行ファイルとカーネルに対して確認します。

`tap`:

- TAP デバイスの側の packet ソケットから送信したフレームを、SIL Kit 側の相手がそのまま受信し、レスポンスが戻る。来たフレームは、すべて `ext_from_devices` に数える。
- ブリッジが書き込んだフレームがブリッジに戻らない。
- 2 つ目のブリッジは拒否される。
- `--tap-create` で作ったデバイスが、down の状態で作られ、ブリッジが止まるとなくなる。

`packet`（veth の対）:

- タグなし、VLAN タグ 1 つ、タグ 2 つのフレームが、タグを保ったまま両方向に通る。
- 同じインターフェースの別のソケットが送信したフレームを、SIL Kit 側の相手が受信する。`ext_from_devices` には数えない（カーネルがここで送信したと印を付ける）。インターフェースのアドレスと、名前空間のほかのインターフェースのアドレスからのフレームも数えない。
- ブリッジが送信したフレームがブリッジに戻らない。
- `--promiscuous` のプロミスキャスが、インターフェースの promiscuity を、動いている間だけ上げる。既定と `--no-promiscuous` は、ここでは本物のカーネルで確かめていない（モードの選び方は単体試験が確かめる）。
- リンクの up と down の監視の行とリンクの行（`link link=up`、`down`、また `up` の順に、各 1 回）。
- OS のスタックが送信した UDP（veth の向こうの端からと、ブリッジのインターフェース自身から）を、SIL Kit 側の相手がそのまま受信し、`ext_checksum_incomplete` に数えられ、警告が 1 回だけ出る。
- 警告のとおりに `ethtool -K <名前> tx off` をすると、数も警告も出ない。

`pcap`（同じ veth の対、この PC にインストールされている本物の libpcap）:

- ライブラリの読み込みと、ブリッジが使う関数と構造体の形。
- タグなし、VLAN タグ 1 つ、タグ 2 つのフレームが、タグを保ったまま両方向に通る。
- 同じインターフェースの別のソケットが送信したフレームを、SIL Kit 側の相手が受信する。`ext_from_devices` に数える（libpcap はここで送信したことを伝えない）。インターフェースのアドレスと、名前空間のほかのインターフェースのアドレスからのフレームは数えない。
- ブリッジが送信したフレームがブリッジに戻らない。
- `--promiscuous`、`--list-interfaces`、リンクの up と down の監視の行。
- 同じインターフェースの `packet` のブリッジ。2 つが使用中の印で互いを見つけ（`shared`、`peers`、警告）、`--list-interfaces` がインターフェースを 2 つのブリッジが使っていると言い、2 つ目が止まると 1 つ目の `shared` が 0 に戻ります。
- 開けないときのメッセージ（ない、イーサネットでない、down、権限を落としたとき）。
- CI は Ubuntu のパッケージ `libpcap0.8` の libpcap で同じテストを実行し、スキップしたら失敗にします（`VEHICLE_SIL_REQUIRE_ETHERNET_NETNS=1`）。

`vehicle_sil_launch run --connect`（同じ veth のペア、`ethernet_bridge.netns.launch`）:

- ランチャーがブリッジを `PATH` で見つけ、既定のバックエンド（`packet`）で一方の端に、ECU の MAC アドレスを `--src-mac` にして起こし、終わりに終了コード 0 で止めます。
- SIL Kit の ECU のフレームがもう一方の端に届き、そこにいる機器の答えが ECU に戻ります（速さ 1、`--strict` のシナリオがそれを待ち、実行には指摘がありません）。

どのバックエンドにも共通:

- 開けないときのメッセージと終了コード 3（ない、TAP でない、イーサネットでない、すでにある、使用中、権限を落としたとき）。
- インターフェースを削除したときの `FAIL` と終了コード 1。
- registry がないときのメッセージと終了コード 3。

### 確認したこと（全プラットフォーム、フェイクの pcap のライブラリ）

フェイクの pcap のライブラリ `tests/eth_link/fake_pcap` に対して確認しています。フェイクは、本物と同じ名前の関数を持つ共有ライブラリで、ブリッジは本物と同じ経路で読み込みます。

- ライブラリをパスで読み込み、関数を名前で取得する。ライブラリがないとき、関数が足りない古いバージョンのときのメッセージと終了コード 3。
- 開くときに要求すること（immediate mode、ブロックしない読み取り、プロミスキャス。Npcap の開き方では、自分の送信を受信しない指定も）。
- フレームの送受信、長すぎるフレーム、止め方、開けないデバイス、送信の失敗と詰まり、読み取りの失敗（`FAIL` と終了コード 1）、捨てた数、警告。
- 「フレームがある」という通知を 1 回も出さないデバイスで、フレームを全部、順序どおりに受信し、警告が 1 回出る。フレームがない間は、待ち 1 回につき 1 回しか読まない。
- Npcap の開き方（`pcap_open()`）と libpcap の開き方（`pcap_create()`）の両方を、どの OS でも。
- Npcap の開き方では、送信したフレームを戻すデバイスでも自分の送信が戻らず、同じデバイスのほかのハンドルが送信したフレームは受信する。
- 同じデバイスに接続した 2 つのブリッジのループが、コピーを受信する順序によらず止まる。`--no-mac-guard` でも自分のフレームが SIL Kit に戻らない。
- `pcap_open` のないライブラリは、Npcap としては拒否する。
- 機器からの答えは `ext_from_devices` に数え、ライブラリの一覧にあるアダプターのアドレスからの答えは転送するが数えない（Linux だけ。Windows ではアドレスを OS から得るので、OS が知らないフェイクのアダプターでは確かめられない）。`--stats-every-ms` で、動いている間に統計の行が出て、その後は変わったときだけ出る。
- Windows では、フェイクのライブラリが使う DLL が、フェイクのライブラリと同じディレクトリかシステムのディレクトリにあるものだけであること（テスト `eth_link.fake_pcap_imports.*`）。ブリッジが DLL を探させるのはこの 2 か所だけなので、フェイクのライブラリにはコンパイラーのランタイムを静的にリンクしています。

### 確認したこと（Windows、本物の Npcap、手動）

本物の Npcap に対して、仮想マシンの仮想 NIC、Microsoft KM-TEST Loopback Adapter、物理の USB のアダプター 1 つ（最後の項目）で、手動で確認しました。テストにはなっていません。

- `wpcap.dll` を `System32\Npcap` から読み込める（`Packet.dll` も見つかる）。ブリッジが使う関数があり、構造体の形が合う（一覧、フレームの長さと中身、捨てた数）。
- 受信を待つイベントで、空回りせず、取りこぼしなく受信する。
- 接続の名前が一覧に出て、`--interface` と一致する（ASCII と日本語の名前、英語の Windows、コンソールでもファイルでも）。アダプターの識別子（小文字、波かっこなし）と pcap の名前でも開ける。大文字と小文字だけが違う名前は一致しない。
- SIL Kit 側の相手と、アダプターの先の機器との往復。プロミスキャスなし（いまの `--no-promiscuous`。当時は既定が off）では、SIL Kit の機器宛てのユニキャストを受信せず、ヒントの警告が出る。
- 同じアダプターを Npcap で開いたほかのソフトと、両方向に通る（仮想 NIC でも、KM-TEST Loopback Adapter でも）。
- Npcap は、ハンドルが送信したフレームをそのハンドルに戻す。`PCAP_OPENFLAG_NOCAPTURE_LOCAL` を付けて開いたハンドルは、自分の送信を受信せず、ほかのハンドルの送信は受信する。
- ブリッジが今の開き方（`pcap_open()` と自分の送信を受信しない指定）で開いたアダプターで、ブリッジの送信がブリッジに戻らない（`--no-mac-guard` でも）。同じアダプターの 2 つのブリッジは、どちらを先に開始しても、警告を出して転送を止める。受信は遅れず、何もないときに CPU を使わない。
- 同じアダプターで OS が送信したフレーム（ARP）を、ブリッジが受信する。
- VLAN タグつきのフレームが通るかは、アダプターのドライバーと設定しだい（「[`npcap` と `pcap`](#npcap-と-pcap)」）。全部を保つ NIC と、どちらの設定でも片方向が欠ける NIC があった。
- ブリッジが読むのが間に合わずに Npcap が捨てたフレームは、`ext_overruns` に数えられる（アダプターに届いて SIL Kit に届かなかった数と一致する）。監視の行にも出る。
- SIL Kit からまとめて送ったフレームは、全部が仮想 NIC から出た。送信は詰まらず、`ext_queue_full` は 0 のまま。仮想 NIC のリンクが切れている間は、送信が全部失敗して `ext_tx_failures` に数えられ、ライブラリの文が `last error:` に出て、ブリッジは動き続けた。
- KM-TEST Loopback Adapter で、OS が送信したフレームはブリッジに届き、ブリッジが送信したフレームは OS の普通のソケットを使うソフトに届かない（「[Windows で PC の中だけで接続する](#windows-で-pc-の中だけで接続する)」）。
- 止める要求（Ctrl+C、Ctrl+Break）で、統計を出力して終了コード 0。動いている間にアダプターを無効にすると、`FAIL` と終了コード 1。無効のアダプターは一覧に出ない。
- 制限なしでインストールした Npcap は、管理者でない利用者でも使える。「管理者だけ」でインストールした Npcap を管理者でない利用者が使うと、昇格を求める画面で止まる。
- 物理 NIC: USB のイーサネットのアダプター 1 つ（ASIX AX88772、100 Mbit/s、Windows に付属のドライバー）を仮想マシンに渡し、LAN につないで、ホストの PC のネットワークスタック（別の NIC）を相手に確認した（ARP）。一覧と `--interface` でアダプターが見つかる。プロミスキャスなし（いまの `--no-promiscuous`。当時は既定が off）では、要求は出て、ユニキャストの応答は受信せず、ヒントの警告が出る。`--promiscuous` ありでは、アダプターがプロミスキャスを受け付け、応答が SIL Kit 側に届く。ブリッジの送信はブリッジに戻らず、ループの警告も出ない。OS がそのアダプターに送信したフレームがブリッジに届く。動いている間にアダプターを無効にすると、`FAIL` と終了コード 1。Ctrl+C で、統計を出力して終了コード 0。

### 確認していないこと

Windows:

- 上の USB のアダプター以外の物理 NIC。回線の先の実機や別の PC。動いている間にケーブルを抜くこと。無線のアダプター。
- 物理 NIC での大量のトラフィックと、VLAN タグ。
- 日本語の Windows（そのコンソールでの日本語の接続の名前の見え方）。
- Npcap を WinPcap 互換ありでインストールした PC。
- プロミスキャスを受け付けないアダプター。プロミスキャスで開けないエラーになる（ブリッジはなしで開き直して警告する）のか、なしで何も言わずに開く（ブリッジは分からない）のか。開き直しは、フェイクのライブラリでだけ確かめています。
- 戻さない指定を受け付けて、それでも自分の送信を戻す Npcap やアダプターがあるか。
- 「管理者だけ」の Npcap で、デスクトップのある Windows が表示する確認の画面。
- 本物の Npcap で、送信の詰まりが `ext_queue_full` に数えられることがあるか（確認では詰まりが起きなかった）。
- 本物の Npcap での、Windows から得るアダプターのアドレス。物理 NIC にケーブルがないとき、`vEthernet` のアダプター、またはそのスイッチを結んだ物理 NIC で `ext_from_devices` が 0 のままか（そこでのホストの OS のフレームの送信元が、ホストのアダプターのアドレスであること）と、自分の送信を戻さない Npcap の指定との関係。機器から何も来ないときのヒントは、テストが時刻を与える単体試験でだけ確かめている。

Linux:

- libpcap で、送信のキューが詰まったことを見分けられるか（`ext_queue_full`）。ライブラリは失敗を 1 種類でしか返さないので、OS のエラーの番号が残っていれば見分けます。フェイクのライブラリでだけ確認しています。
- 物理 NIC と、その先の実機や別の PC との往復。
- プロミスキャスのありなしでの、フレームの受信のしかたと、プロミスキャスを受け付けないインターフェース。veth は、プロミスキャスでなくても全部のフレームをソケットに渡すので、名前空間では違いが出ず、プロミスキャスも受け付けます。プロミスキャスのヒントの警告は、時計を差し替えたテストでだけ確認しています。
- `setcap cap_net_raw+ep` した実行ファイルが、共有ライブラリの SIL Kit を見つけられるか。権限を付与した実行ファイルでは、ライブラリを探す環境変数が効かないことがあります。
- ほかの人が所有する TAP デバイスに接続しようとしたときのメッセージ。文言はテストで確認していますが、カーネルがそのエラーを返す場面は作れていません。
- 受信のオフロードで長いフレームが見える場面、無線のアダプター。
- 物理 NIC での、チェックサムが未完成のフレームの警告と、`ethtool -K <名前> tx off` の効果（テストは veth の対だけです）。
- 送信の分割のオフロードで TCP の長いフレームが見えることと、それが `tx off` でなくなることは、veth の対で確認しましたが、テストにはありません。

そのほか:

- Windows の本物の Npcap、Linux の物理の NIC で、設定ファイルから起こす形。自動の試験 `ethernet_bridge.integration.pcap-launcher` は、フェイクの pcap のライブラリで、設定ファイルから起こします。
- EtherType の位置、VLAN の TPID、最大長は、「[フィルター](#フィルターframefilter)」の注のとおり、規格では確認していません。

## テスト

`ctest -R '^(eth_link|ethernet_bridge)\.'` で実行します。名前空間のテストのほかは実時間に頼らず、フェイクの外部リンクとフェイクのコントローラーで、起きたことを待ちます。

| テスト | プラットフォーム | 確認するもの |
|---|---|---|
| `eth_link.*` | 全部 | MAC アドレス、フィルター、MAC テーブル、フェイクの外部リンク |
| `ethernet_bridge.*` | 全部 | フェイクのコントローラーとフェイクの外部リンクでの転送、統計・監視・リンクの行、引数のエラーの文言 |
| `eth_link.os_link_*` | 全部 | OS の外部リンクが開けないときのメッセージと種類、`--list-interfaces` の表示 |
| `ethernet_bridge.app_*` | 全部 | OS ごとに使えるバックエンドと使えないときのメッセージ、起動の 1 行、プロミスキャスのヒント |
| `ethernet_bridge.integration.*` | 全部 | フェイクの外部リンクをリンクした実行ファイルを、プロセスとして動かしたときの動作 |
| `eth_link.pcap_*` | 全部 | pcap の外部リンクのメッセージ、開き方、名前の一致、一覧、フェイクのライブラリでの送受信と失敗 |
| `ethernet_bridge.integration.pcap-*` | Linux、Windows | ライブラリを読み込む場所だけをフェイクに変えた実行ファイルでの動作 |
| `ethernet_bridge.netns.*` | Linux | 「確認したこと（Linux、自動テスト）」の内容 |

### `eth_link.*`

- フィルターは、EtherType、VLAN タグの後ろ、MAC、最大長を確認します。
- MAC テーブルは、戻り、両側、上限、並行を確認します。
- `eth_link.boundary` は、`libs/eth_link` が SIL Kit のヘッダーを直接にも間接にも含まないことを確認します。
- `eth_link.os_link_*` は文だけを確認するので、全プラットフォームで実行します。

### `ethernet_bridge.*`

- 転送、フィルター、自分のフレームの戻りを確認します。
- 2 つのブリッジを通るループは、止まる順序と、自分の送信を戻す外部リンクで止まらない順序、片方が `--no-mac-guard` の場合を確認します。
- `ethernet_bridge.*_npcap_adapter_*` は、Npcap の開き方の pcap の外部リンクを、フェイクの pcap のライブラリの同じデバイスに 2 つ開いて、ループが止まることと、`--no-mac-guard` でも自分のフレームが戻らないことを確認します。
- MAC テーブルがあふれたとき、転送の最中の `stop()`、失ったフレーム、`Fault`、警告、開始と停止を確認します。
- `ethernet_bridge.silkit_*` は、プロセス内の registry（空いているポート）と 2 つの participant で、本物の SIL Kit 側を確認します。
- `ethernet_bridge.app_*` のプロミスキャスのヒントは、時計を差し替えて、5 秒を待たずに確認します。
- `ext_from_devices` に数えるものを確認します。機器からのフレームは、フィルターが通さないものも数え、この PC のどれかのアドレスから来たもの、ここで送信したと印の付いたもの、SIL Kit 側、戻す外部リンクから戻ったもの、短すぎるものは数えません。`--no-mac-guard` では `--src-mac` のアドレスで見分けます。
- 機器から何も来ないときのヒントの決まり（`bridge_runtime.nothing_came_hint_*`）と、動いている間の統計の行（`bridge_runtime.run_bridge_*`）は、テストが時刻を与えて確認します。

### `ethernet_bridge.integration.*`

実行ファイルにフェイクの外部リンクをリンクしたもの（`vehicle_sil_ethernet_bridge_on_fake`。`tests/ethernet_bridge/fake_link_factory.cpp`）を、registry と SIL Kit 側の相手と一緒に、プロセスとして動かします。相手は `eth_bridge_it_peer` で、SIL Kit の `IEthernetController` を直接使います。

- 確認するのは、往復と統計、起動の 1 行、引数のエラー（4）、開けないインターフェース（3）、`Fault`（1）、止める要求（統計、`stopped`、0）です。
- `device-shared` は、`fake0` に 2 つのブリッジを起こします。偽のデバイスの既定のスコープ（プロセスごと。同時に走る試験が互いを見つけないため）では、互いを見つけません。同じスコープ（`VEHICLE_SIL_FAKE_DEVICE_SCOPE`）では、2 つ目が起動のときに 1 つ目を見つけ、1 つ目は動いている間に 2 つ目を見つけて警告し、両方が動き続け、`--list-interfaces --json` は `fake0` を 2 つのブリッジが使っていると言い、1 つ目が終わると 2 つ目の `shared` が 0 に戻ります。
- `backend-not-here` は本物の実行ファイルで、その OS で使えないバックエンドのメッセージと、pcap の一覧を確認します。一覧は、その PC にライブラリがあれば一覧、なければインストールの方法のメッセージです。

### `eth_link.pcap_*` と `ethernet_bridge.integration.pcap-*`

- `eth_link.pcap_*` は、Npcap と libpcap の両方のメッセージを、どの OS でも照合します。
- 開き方は、Npcap の `pcap_open()` と libpcap の `pcap_create()` の両方を、どの OS でも確認します。
- フェイクのライブラリ（`tests/eth_link/fake_pcap`）は、本物と同じ名前の関数を持つ共有ライブラリで、実行時に読み込みます。
- フェイクのライブラリの `fake0` は、レスポンスを返す機器です。ほかに、開けない、送信できない、読めない、捨てる、無線、送信したものを戻す、のデバイスと、答えがアダプター自身のアドレスから来る `fake-self` があります。`fake0`、`fake1`、`fake-self` は、Linux の libpcap のように、アドレスを 1 つずつ一覧に載せます。
- 関数が 1 つ足りないバージョンが 2 つあります（古いバージョンと、`pcap_open` のないバージョン）。
- `ethernet_bridge.integration.pcap-*` は、インストールする実行ファイルと同じ中身で、ライブラリを読み込む場所だけをフェイクのライブラリに変えたもの（`vehicle_sil_ethernet_bridge_on_fake_pcap`）を使います。
- `ethernet_bridge.integration.pcap-*` が確認するのは、往復と統計、一覧、ライブラリがない・古いときのメッセージ（3。SIL Kit に接続する前）、ないデバイス（3）、読み取りの失敗（1）です。
- `pcap-describe` は、ライブラリがなくても `--describe` が答えることを確かめます。
- `pcap-roundtrip` と `echo-roundtrip` は、機器の答えがすべて `ext_from_devices` に数えられることを確かめます。`pcap-own-address` は、アダプター自身のアドレスからの答えを転送し、数えないことを確かめます（Linux だけ）。`pcap-nothing` は、何も答えないデバイスへ `--stats-every-ms 100` で送り、動いている間に統計の行が、変わったときだけ、すべてのキーを順に並べて出て、`ext_from_devices=0` であることを確かめます。
- `pcap-promiscuous` は、プロミスキャスを受け付けるデバイスと受け付けないデバイスでの既定（警告と起動の 1 行）、`--no-promiscuous`、受け付けないデバイスでの `--promiscuous`（終了コード 3）、両方を付けたとき（終了コード 4）を確かめます。
- `pcap-launcher` は、ブリッジ（`role: bridge`、`required: false`）と、そのネットワークの ECU（`eth_bridge_it_ecu.cpp`）を書いた設定ファイルを、`registry_uri` のポート 0 で作ります。`check` が所見なしで通り、ブリッジをそのネットワークに挙げること、ECU のフレームがブリッジを通って外に出て、デバイスの応答が ECU に戻ること（シナリオで確かめる）、実行が終わるとブリッジが終了コード 0 で終わることを確かめます。

### `ethernet_bridge.netns.*`

`ethernet_bridge.netns.tap`、`ethernet_bridge.netns.packet`、`ethernet_bridge.netns.pcap`、`ethernet_bridge.netns.launch`（`vehicle_sil_launch run --connect`。このスクリプトが veth のペアのもう一方の端で機器の代わりをする）は、`tests/ethernet_bridge/bridge_netns.py` が実行します。スキップと省略の条件は次のとおりです。

| 条件 | 動作 | 失敗にする環境変数 |
|---|---|---|
| ユーザー名前空間を作れない | スキップします（終了コード 77）。 | `VEHICLE_SIL_REQUIRE_MULTI_HOST=1` |
| 名前空間は作れるが、その中で TAP デバイス、veth、packet ソケットを作れない | スキップします。 | `VEHICLE_SIL_REQUIRE_ETHERNET_NETNS=1` |
| libpcap がインストールされていない（`pcap` だけ） | スキップします。 | `VEHICLE_SIL_REQUIRE_ETHERNET_NETNS=1` |
| `setpriv` や `ethtool` がない | それが必要な部分だけを省いて、`NOT CHECKED` と表示します。 | `VEHICLE_SIL_REQUIRE_ETHERNET_NETNS=1` |

どの条件でスキップしたかは、出力の `skipped:` の行に表示されます。CI は 2 つの変数を両方設定するので、スキップも省略も失敗になります。
