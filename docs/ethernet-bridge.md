# イーサネットのブリッジ

SIL Kit の Ethernet ネットワークと、SIL Kit の外のイーサネットのインターフェースの間でフレームを運ぶ部品を使う人と、直す人向けです。前に読む文書はありません。直す人は、次に [code-map.md](code-map.md) の「Ethernet Bridge」を読みます。

ここに書いてあるのは、2 つのライブラリです。

| ターゲット | 中身 |
|---|---|
| `vehicle_sil::eth_link` | 外のインターフェースの口 `ExternalLink`、MAC アドレス、絞り込み `FrameFilter`、輪を防ぐ表 `MacSideTable`。SIL Kit を使わない |
| `vehicle_sil::ethernet_bridge_core` | SIL Kit の `IEthernetController` と `ExternalLink` の間でフレームを運ぶ `EthernetBridge`、統計と監視の行、引数の解釈（`parse_bridge_options()`）。プラットフォームに依存しない |

```mermaid
flowchart LR
    silkit["SIL Kit Ethernet<br/>IEthernetController"] <--> bridge["EthernetBridge<br/>FrameFilter, MacSideTable"]
    bridge <--> link["ExternalLink<br/>(eth_link)"]
    link <--> nic["外のインターフェース"]
```

## 外のインターフェースの口（`ExternalLink`）

`vehicle_sil::eth_link::ExternalLink`（`vehicle_sil/eth_link/external_link.hpp`）は、外のイーサネットのインターフェース 1 つです。フレームはバイト列で、宛先 MAC アドレスから payload の終わりまでです。FCS は含みません。

| 関数 | 契約 |
|---|---|
| `start(handler)` | インターフェースを開き、受信スレッドから `handler` に `LinkEvent` を渡し始める。開けなければ `LinkError` を投げる |
| `stop()` | 渡すのをやめる。戻る前に、前の知らせの後で OS が捨てたフレームの数を、呼んだスレッドから `Overrun` で知らせてよい。戻った時点で、ハンドラーは動いていない。始めていなくても呼べる |
| `transmit(frame)` | フレームを 1 つ送る。結果は `Ok`、`QueueFull`（送信のキューがいっぱいで送らなかった）、`Failed`（ほかの誤り。理由は `last_error()`）。フレームは呼び出しの間だけ読む |
| `describe()` | 起動のときに出す 1 行の部品。種類、名前、実か仮想か（例: `tap 'vsil0' (virtual)`） |

スレッド: ハンドラーは一度に 1 つのスレッドからだけ呼ばれます（受信スレッドと、`stop()` を呼んだスレッド）。`transmit()` はどのスレッドからでも、ハンドラーと並行にも呼べます。受信の時刻は渡しません。OS の時計はシミュレーションの時計と別のものだからです。

`LinkEvent` の種類です。

| 種類 | 意味 |
|---|---|
| `Frame` | 受けたフレーム（`frame`。ハンドラーの呼び出しの間だけ有効） |
| `Overrun` | OS が読み取りの前に捨てたフレームの数（`count`。前の知らせからの増分） |
| `LinkState` | リンクが上がった、下がった（`state`: `Up`、`Down`、`Unknown`） |
| `Fault` | リンクが動かなくなった（インターフェースがなくなった、読み取りが失敗し続ける）。理由は `detail`。この後フレームは届かず、`transmit()` は失敗する |
| `Warning` | 止めるほどではないが利用者に知らせること（`detail`） |

`LinkError` の種類（`kind()`）は `NoSuchInterface`、`PermissionDenied`、`LibraryMissing`、`NotEthernet`、`Other` です。`what()` は、何が起きたかと直し方を言います。

試験用の偽の口は `tests/eth_link/fake_link`（`FakeLink` と、偽の口どうしをつなぐ `FakeSegment`）です。受信スレッドを持ち、送信の詰まり、取りこぼし、`Fault`、リンクの上下、自分が送ったフレームが戻るインターフェースを模擬します。

## 転送（`EthernetBridge`）

`vehicle_sil::ethernet_bridge::EthernetBridge`（`vehicle_sil/ethernet_bridge/ethernet_bridge.hpp`）は、フレームを書き換えずに両方向に運びます。

- `start()` は、外の口を始め、SIL Kit のフレームのハンドラーを RX の向きだけで登録し、送信の確認のハンドラーを登録し、コントローラーを `Activate()` します。失敗したら、始めたものを戻してから例外を投げます。
- `stop()` は、SIL Kit のフレームのハンドラーを外し、外の口を止め、送信の確認のハンドラーを外し、コントローラーを `Deactivate()` します。何度呼んでもかまいません。
- 自分のキューは持ちません。SIL Kit から外へは SIL Kit の受信スレッドで `transmit()` し、外から SIL Kit へは外の口の受信スレッドで `SendFrame()` します。押し戻しはなく、失ったフレームは数えます。
- `Fault` が届いても、ブリッジは動き続けます。`fault()` が理由を返すので、止めるかどうかは呼ぶ側が決めます。
- 警告は `take_warnings()` で、古い順に受け取ります。外の口の `Warning` と、ブリッジ自身の警告（下の「輪の防ぎ方」）です。

1 つのフレームは、次の順に調べます。

| 向き | 順序 |
|---|---|
| SIL Kit から外へ | 長さ（`malformed`、`oversize`）→ 絞り込み（`filtered_to_ext`）→ MAC の規則（`loop_suspected`）→ `transmit()` |
| 外から SIL Kit へ | 長さ（`malformed`、`oversize`）→ MAC の規則（`own_tx_dropped`、`loop_suspected`）→ 絞り込み（`filtered_from_ext`）→ `SendFrame()` |

外から来たフレームは、絞り込みより先に MAC の規則を見ます。自分が送ったフレームの戻りは、絞り込みで落ちるものでも `own_tx_dropped` に数えるためです。

## 絞り込み（`FrameFilter`）

同じ規則を両方向にかけます（`vehicle_sil/eth_link/frame_filter.hpp`）。

- 長さ: イーサネットのヘッダー（14 バイト）より短いフレームは `malformed`、最大長（既定 1522 バイト、FCS なし）より長いフレームは `oversize` として捨てます。
- EtherType（`FrameFilterOptions::ether_types`）: 挙げた EtherType のフレームだけを通します。送り元 MAC の後の 2 バイトが VLAN タグの印（TPID。既定は `0x8100` と `0x88A8`）なら、その 4 バイトのタグの後ろを見ます。見るタグは 2 つまでです。タグの途中で終わるフレームは通しません。
- SIL Kit の側の MAC（`FrameFilterOptions::silkit_macs`）: SIL Kit から外へは、送り元がそのどれかのフレームだけを通します。外から SIL Kit へは、宛先がそのどれかか、グループのアドレス（ブロードキャスト、マルチキャスト）のフレームだけを通します。グループのアドレスは挙げられません。
- 両方を挙げたときは、両方を満たすフレームだけを通します。どちらも挙げなければ、長さのほかは絞りません。

> [!NOTE]
> VLAN の TPID の既定（`0x8100`、`0x88A8`）、最大長の既定 1522 バイト、EtherType の位置は、一般の資料の値で、規格では確かめていません。どれも `FrameFilterOptions`（引数では `--vlan-tpid`、`--max-frame-bytes`）で変えられます。試験と例が使う EtherType `0x88B5` は、ローカルの実験用として知られた値で、これも規格では確かめていません。プロトコルの EtherType はライブラリに入れていないので、利用者が挙げます。

SIL Kit は、60 バイトより短いフレームを受け手の側で 0 で埋めます（[dependencies.md](dependencies.md) の「実装が前提にしている上流の挙動」）。そのため SIL Kit から来るフレームは 60 バイト以上で、外から来た短いフレームは、埋められて SIL Kit の相手に届きます。

## 輪の防ぎ方（`MacSideTable`）

OS の設定に頼らず、フレームの送り元 MAC で防ぎます（`vehicle_sil/eth_link/mac_side_table.hpp`）。インターフェースによっては、自分が送ったフレームが自分の受信に戻りますが、どのインターフェースでも同じ規則で動きます。

- SIL Kit から来て、絞り込みを通ったフレームの送り元を「SIL Kit の側」として覚えます。
- 外から来たフレームの送り元が SIL Kit の側なら、自分が送ったフレームの戻りとして捨て、`own_tx_dropped` に数えます。
- 外から来て、絞り込みを通ったフレームの送り元を「外の側」として覚えます。絞り込みで落ちたフレームでは覚えません。
- SIL Kit から来たフレームの送り元が外の側なら、そのアドレスは両側にあります（同じネットワークの間に 2 つのブリッジがある、または同じ MAC アドレスが両側にある）。そのアドレスを持つフレームは、以後両方向で捨て、`loop_suspected` に数えます。警告は、アドレスごとに 1 回、最初の 8 つまでアドレスを挙げて出し、それより多いときは 1 回だけまとめて出します。
- グループの送り元アドレスは覚えません。覚えたアドレスは忘れません。

両方向で捨てるのは、2 つのブリッジを通る輪を、写しが届く順序によらず止めるためです。片方のブリッジだけがアドレスを両側に見ても、そのブリッジが外から SIL Kit へ戻すのをやめるので、輪は切れます。代わりに、同じ MAC アドレスを持つ機器が両側にあると、そのアドレスのフレームは届かなくなります。警告がアドレスを挙げるので、構成の誤りとして見つけられます。

表の大きさは既定 1024 アドレスで、確保はブリッジを作るときだけです。いっぱいになると、新しいアドレスは覚えずにフレームを通し、警告を 1 回出します。

`EthernetBridgeOptions::mac_guard` を `false`（引数では `--no-mac-guard`）にすると、この規則を使いません。輪は使う人が防ぎます。

警告の文は次のとおりです（試験が文言まで確かめています）。

```
MAC 02:00:00:00:00:99 is on both sides: frames from it came from the external link and now come from SIL Kit, so frames from it are dropped both ways (loop_suspected). Run one bridge between these networks, and give each device its own MAC address.
more MAC addresses are on both sides than the first 8 named: frames from them are dropped both ways too (loop_suspected).
the MAC table is full (1024 addresses): frames from addresses seen from now on are forwarded without the check against loops. Raise --mac-table-size if this many devices are expected.
```

## 統計

`format_statistics()` は、全部の数を 1 行にします。キーの名前と順序はこの文書の契約で、試験が読みます。

```
stats silkit_to_ext=0 ext_to_silkit=0 own_tx_dropped=0 loop_suspected=0 filtered_to_ext=0 filtered_from_ext=0 oversize=0 malformed=0 ext_overruns=0 ext_queue_full=0 ext_tx_failures=0 silkit_tx_failures=0
```

| キー | 数えるもの |
|---|---|
| `silkit_to_ext` | SIL Kit から外へ送ったフレーム |
| `ext_to_silkit` | 外から SIL Kit へ渡したフレーム（`SendFrame()` が戻ったもの） |
| `own_tx_dropped` | 外の口から戻ってきた、自分が送ったフレーム |
| `loop_suspected` | 送り元が両側にあるアドレスのフレーム |
| `filtered_to_ext`、`filtered_from_ext` | 絞り込みで通さなかったフレーム（向きごと） |
| `oversize` | 最大長より長いフレーム（両方向） |
| `malformed` | イーサネットのヘッダーより短いフレーム（両方向） |
| `ext_overruns` | OS が読み取りの前に捨てたフレーム |
| `ext_queue_full` | 外の口の送信のキューがいっぱいで失ったフレーム |
| `ext_tx_failures` | 外の口でのほかの送信の失敗 |
| `silkit_tx_failures` | `SendFrame()` が例外を投げたもの、または SIL Kit が送れなかったと確認したもの（`ControllerInactive`、`LinkDown`、`Dropped`、`InvalidFrameFormat`）。後者のフレームは、渡した時点で `ext_to_silkit` にも数えてある |

問題の数は `loop_suspected`、`oversize`、`malformed`、`ext_overruns`、`ext_queue_full`、`ext_tx_failures`、`silkit_tx_failures` です（`problems_grew()`）。転送した数、絞り込みの数、`own_tx_dropped` は問題に数えません。

## 監視の行

`format_health_change(before, now, interface, problems_before)` は、呼ぶ側が取った 2 つの見本（`EthernetBridge::health()`）の間の変化を 1 行にします。何もなければ行を作りません。

- 問題の数が増えたか、リンクの状態が変わったとき（ただし、状態不明から `link up` への変化は、健全なリンクの最初の知らせなので出しません）:
  ```
  interface enp3s0: link down; since the last report: ext_tx_failures +12
  ```
- 何も増えていないが、前の見本では増えていたとき（`problems_before`）:
  ```
  interface enp3s0: link up; no new problems since the last report
  ```

リンクの状態は `link up`、`link down`、`link state unknown` のどれかです。

## 引数（`parse_bridge_options()`）

`vehicle_sil/ethernet_bridge/bridge_options.hpp` の `parse_bridge_options()` が解釈する引数です。誤りは `BridgeUsageError` で、何が誤りかと直し方を言います。使い方の全文は `bridge_usage()` です。

| 引数 | 意味 |
|---|---|
| `--registry-uri <uri>` | SIL Kit の registry。必須 |
| `--network <name>` | SIL Kit の Ethernet ネットワーク。必須で、既定の名前はない（別のネットワークに誤ってつながないため） |
| `--participant <name>` | participant の名前（既定 `EthernetBridge`） |
| `--backend <tap\|packet\|npcap\|pcap>` | 外のインターフェースの開き方。必須。`tap` は PC の中の仮想のインターフェース、ほかは実のアダプターに届きうるものとして扱う |
| `--interface <name>` | つなぐインターフェース。必須で、「最初に見つかったもの」は選ばない |
| `--ethertype <hex>,...` | 通す EtherType（`0x` で始まる 16 進。繰り返すと足される） |
| `--src-mac <mac>,...` | SIL Kit の側の MAC アドレス（`:` か `-` で区切った 6 バイト。繰り返すと足される） |
| `--all-frames` | 絞らない。`--ethertype`、`--src-mac` とは一緒に使えない |
| `--vlan-tpid <hex>,...` | VLAN タグの印（既定 `0x8100,0x88a8`） |
| `--max-frame-bytes <n>` | 最大長、FCS なし、60〜65535（既定 1522）。SIL Kit が 60 バイトより短いフレームを渡さないので、60 が下限 |
| `--promiscuous` | 自分宛てでないフレームも受ける。`tap` では使えない |
| `--tap-create` | TAP デバイスをブリッジが動く間だけ作る。`tap` だけ |
| `--no-mac-guard` | MAC の規則を使わない |
| `--mac-table-size <n>` | MAC の表の大きさ、16〜65536（既定 1024） |
| `--list-interfaces` | インターフェースの一覧を求める（`--backend` のほかは要らない） |
| `--help` | 使い方 |

実のアダプターに届きうる `--backend`（`packet`、`npcap`、`pcap`）では、何を通すか（`--ethertype`、`--src-mac`、`--all-frames` のどれか）が必須です。ないと次の誤りになります。

```
--backend npcap sends to a real adapter: say what may pass with --ethertype, --src-mac or --all-frames.
```

## 試験

`ctest -R '^(eth_link|ethernet_bridge)\.'` で回ります。どれも実時間に頼らず、偽の口と偽のコントローラーで起きたことを待ちます。

- `eth_link.*`: MAC アドレス、絞り込み（EtherType、VLAN タグの先、MAC、最大長）、MAC の表（戻り、両側、上限、並行）、偽の口。`eth_link.boundary` は、`libs/eth_link` が SIL Kit のヘッダーを直接にも間接にも含まないことを確かめます。
- `ethernet_bridge.*`: 偽のコントローラーと偽の口での転送、絞り込み、自分のフレームの戻り、2 つのブリッジを通る輪が止まること、失ったフレーム、`Fault`、警告、始め方と止め方。統計と監視の行、引数の誤りの文言。`ethernet_bridge.silkit_*` は、プロセス内の registry（空いているポート）と 2 つの participant で、本物の SIL Kit の側を確かめます。
