# participant の基盤（`vehicle_sil::core`）

[English](../../internals/core.md)

SIL Kit の participant と lifecycle を持つ `ParticipantRuntime` について、公開 API が保証すること、所有と終了の順序、スレッドとキュー、実時間モードの時刻、SIL Kit の設定の既定を書きます。仮想時間の進み方と時間ゲートは [virtual-time.md](virtual-time.md) にあります。

| 節 | 書いてあること |
|---|---|
| [公開 API の保証](#公開-api-の保証) | `start()`、`stop()`、CommunicationReady のハンドラー、失敗したときの停止、`bind_host()` |
| [所有権と終了順序](#所有権と終了順序) | 何が何を所有するか、`Peer` が終了する順 |
| [スレッドとキュー](#スレッドとキュー) | どのスレッドが何を処理するか、キューの容量 |
| [時刻（実時間モード）](#時刻実時間モード) | 時刻同期を使わないときの時計 |
| [SIL Kit の設定の既定](#sil-kit-の設定の既定) | 既定の participant の設定、自分の設定に追加するキー、registry の設定 |

## 公開 API の保証

### コンストラクターとコントローラー

- `ParticipantRuntime` のコンストラクターはレジストリに接続します。失敗すると `SilKit::SilKitError` を投げます。
- バスのコントローラーは `start()` の前に作ります。`start()` が戻った時点で、接続済みの相手はそのコントローラーの購読を確認済みです。
- コントローラーを参照するもの（`SilKitCanPort`、`SilKitLinPort`）は、コントローラーを所有しません。participant より先に閉じるか破棄します。
- `LinMaster` と `LinSlave` はポートを所有しないので、ポートより先に破棄します。
- J1939 スタックは、プロセス全体の状態を持ちません。1 つのプロセスに、いくつでも `Network` と `Peer` を置けます。

### `start()` が戻るときの状態

`start(timeout)` は、lifecycle が Running になるまで待ちます。タイムアウトや失敗のときは `std::runtime_error` を投げます。

戻った直後の `state()` は、Running か、その後の状態です。短い実行は、見たときにもう終わっていることがあります。それも開始の成功です。Running より前の状態（ReadyToRun など）のまま戻ることはありません。

この保証のために、`start()` の待ちは次の 2 つの順を守ります。

1 つ目は、ReadyToRun を離れるまで待つことです。時刻同期のない participant では、SIL Kit は starting のハンドラーを ReadyToRun で呼び、戻ってから Running にします。`start()` は、ハンドラーが呼ばれたフラグだけでは戻らず、状態が ReadyToRun を離れるまで待ちます。

2 つ目は、フラグと状態を読む順です。待ちの中では、フラグ（starting のハンドラー、最初のステップ）を先に読み、状態をその後で読みます。この順を入れ替えると、ReadyToRun のまま戻る経路ができます。逆に、フラグを読んだ後に Running を通って止まった実行を失敗にしないよう、止まる途中か終わった状態を見たときはフラグを読み直します。`give_up` が「待つのをやめる」と答えたときも、同じ理由でフラグを読み直します。`give_up` がシミュレーションを見るのは待ちが状態を読んだ後なので、短い実行はその間に始まって終わっていることがあります。この participant が始まっていれば、その答えは採らず、`start()` は始まったものとして戻ります。

この 2 つの順は、`libs/core/src/participant_runtime_probe.hpp` のテスト用のフック（テストだけが使う。インストールしない）で順を決めて、`core.participant_start_*` が確認します。

### CommunicationReady のハンドラー

| 関数 | すること |
|---|---|
| `add_communication_ready_handler(handler)` | SIL Kit の CommunicationReady で呼ぶハンドラーを追加する |
| `set_communication_ready_handler_async(handler)` と `complete_communication_ready()` | ハンドラーが戻った後も、`complete_communication_ready()` を呼ぶまで、participant を最初のステップの前（CommunicationInitialized）にとどめる |

#### `add_communication_ready_handler()`

ハンドラーは、SIL Kit のスレッドの上で、最初のステップの前に呼ばれます。autonomous の participant でも同じスレッドです。

このハンドラーで、コントローラーを始めます。始めるのは、`SilKitCanPort::start()`、`SilKitEthPort::start()`、LIN の `init()`、ブリッジの `start_controller()` です。SIL Kit は、CAN の `SetBaudRate` と `Start`、LIN の `Init`、Ethernet の `Activate` を、ここで呼ぶよう勧めています。

- `start()` の前にだけ呼べます。
- 追加したハンドラーは、追加した順に、`set_communication_ready_handler()` や `set_communication_ready_handler_async()` のハンドラーより前に呼ばれます。互いに置き換えません。
- 追加したハンドラーの例外は、participant の失敗として報告します（`report_failure()`、「CommunicationReady handler failed: ...」）。残りのハンドラーは呼びます。
- ハンドラーが使うものは、`stop()` が戻るまで（または `ParticipantRuntime` を破棄するまで）生かしておきます。

`Peer` も、自分のコントローラー（CAN の `start()`、LIN の `init()`）をこの関数で追加します。そのため、`peer.runtime()` に `set_communication_ready_handler()` でハンドラーを設定しても、置き換わりません。`Peer` は、失敗したコントローラーのネットワークの名前を付けて `report_failure()` に渡します。

#### `set_communication_ready_handler_async()` と `complete_communication_ready()`

Coordinated では、全員が最初のステップの前で待つので、その間に publish/subscribe でやりとりできます。ハンドラーの中ではできません。完了は、ハンドラーが起こした別のスレッドから呼びます。

| 条件 | 起きること |
|---|---|
| ハンドラーの中から `complete_communication_ready()` を呼ぶ | 呼べない |
| ハンドラーが呼ばれる前に呼ぶ、または 2 回目に呼ぶ | 何もせずに偽を返す |
| `stop()` が戻った後に、最初のステップの前で待つ区間が始まる | ハンドラーを呼ばず、runtime が自分で完了させる |

完了するまで誰のステップも始まりません。そのため、待っている間にする処理が失敗したときも、必ず完了させます。`SimulationController::set_start_window()` がこの関数を使います（[control.md](control.md) の「SIL Kit の上のトランスポート（`vehicle_sil::control_silkit`）」）。

### ほかの participant の状態の通知

`set_peer_status_observer(observer)` は、ほかの participant（と自分）のライフサイクルの状態がわかるたびに、名前と状態を渡して observer を呼びます。

- 呼ぶのは、それを知ったスレッド（SIL Kit のスレッドか、状態を読み直すスレッド）で、1 つずつです。
- 同じ状態でもう一度呼ぶことがあります。
- observer は短くし、`peer_status()` のほかはこの runtime を呼ばないこと。
- `start()` の前に登録します。

`wait_for_peer()` は、相手の状態が条件を満たすまで待ちます。待っている間は、SIL Kit から状態を定期的に読み直します（[dependencies.md](../dependencies.md#sil-kit-addparticipantstatushandler-登録中のステータスの取りこぼし) の「SIL Kit: `AddParticipantStatusHandler` 登録中のステータスの取りこぼし」）。SIL Kit のハンドラーの中からは呼べません。

### `stop()` の保証

- `stop()` は何度呼んでもよく、例外を投げません。
- `stop()` が戻った後は、ステップのハンドラーも CommunicationReady のハンドラー（設定したもの、追加したもの、非同期のもの）も呼ばれません。
- 実行中のハンドラーは、終わるまで待ちます。
- ハンドラーが使うもの（ポート、ブリッジ、スタック）は、`stop()` の後で破棄できます。

CommunicationReady のハンドラーにこの保証が必要なのは、開始が Running の前に失敗したとき（タイムアウト、`give_up`）です。SIL Kit は Running の前の停止を受け付けないので、`stop()` の後も lifecycle は `ParticipantRuntime` を破棄するまで生きています。待っていた相手が遅れて来ると、CommunicationReady に進みます。

| CommunicationReady が来る時点 | ハンドラーの扱い |
|---|---|
| `stop()` が戻った後 | 呼ばずに先へ進む（コントローラーは始まらず、ステップのハンドラーも呼ばない）。`stop()` が預かった停止を、lifecycle が受け取れる所で渡し、そこで止まる: 仮想時間では最初のステップ、それ以外では Running になったとき |
| `stop()` が終了を待っている間 | ふつうに呼ぶ |

確認するテストは、`core.communication_ready_after_stop_*` と `core.communication_ready_handler_that_runs_is_waited_for_by_stop` です。

`stop()` は、SIL Kit のハンドラー（ステップ、CommunicationReady）の中から呼べません。自分を待って止まるためです。そこでは `request_stop()` を使います。`report_failure()` と `request_stop()` は、どのハンドラーの中からも呼べます。

### 失敗した participant の停止

participant が失敗すると（ステップのハンドラー、実時間のドライバー、`report_failure()`）、自分の `stop()` と `request_stop()` は、Error になるまで lifecycle に停止を渡しません。先にふつうの停止が渡ると、Coordinated ではシミュレーション全体が正常に止まり、controller から失敗が見えないためです。

失敗のフラグを立てることと、SIL Kit に Error を報告することは、1 つのロックの下で行います。そのため、失敗が見えた後の停止が報告を追い越すことはありません。SIL Kit が後回しにした処理を実行する順序には頼りません。

`stop()` の待ちは、Error と Shutdown を合わせて `shutdown_timeout` までです。過ぎれば、Error の前でも停止を渡します。

次の場合は、Error を経ないか、Stopping の後で Error になります。

- 失敗より前の `request_stop()`（失敗するステップのハンドラーが始まる前、`report_failure()` の前）は、ふつうの停止です。
- controller の中断や、ほかの participant の停止が先に来たときも同じです。

`report_failure()` を呼ぶ時点による違いは、次のとおりです。

| 呼ぶ時点 | 起きること |
|---|---|
| `start()` が lifecycle を始めるまで（`start()` の前） | 何もしない（SIL Kit が無視するため） |
| `start()` が lifecycle を始めている最中 | 覚えておき、`start()` が報告する |

最中の呼び出しがあるのは、autonomous の participant の CommunicationReady のハンドラーが、`start()` がそこから戻る前に呼ばれることがあるためです。この順は、テスト用のフックで決めて `core.participant_start_reports_a_failure_*` が確認します。

### `bind_host()`

`bind_host(runtime, host)`（`vehicle_sil/core/bind_host.hpp`）は、`ParticipantRuntime` のステップのハンドラーから、`vehicle_sil::step::Host` の `step(now)` を呼ぶようにします。最初のステップの前に、participant のステップ幅を `Host::set_step_size()` に渡します。

| 条件 | 結果 |
|---|---|
| 仮想時間でない runtime に使う | `std::invalid_argument` |
| ホストの時間の領域が `Virtual` でない | `std::logic_error` |
| ホストが例外を投げる | ステップのハンドラーの失敗として、participant を Error にする |

- `start()` の前に呼びます。
- ホストは runtime のステップより長く生かします（runtime を先に止めます）。
- `core` はこのために `vehicle_sil::step` をリンクしますが、バスの種類には依存しません（[can.md](can.md) の「仮想時間での駆動」）。

## 所有権と終了順序

| 持つ側 | 相手 | 関係 |
|---|---|---|
| `ParticipantRuntime` | `SilKit::IParticipant` | 所有する |
| `SilKit::IParticipant` | `ICanController`、`ILinController` | 所有する |
| `SilKitCanPort`、`SilKitLinPort` | コントローラー | 参照する（所有しない） |
| `LinMaster`、`LinSlave` | `LinPort` | 参照する（所有しない） |
| `Host` | ポートと `Network` | 参照する（所有しない） |

`Peer` は次の順で終了します（自分で組み立てるときも同じ順にします）。

1. ステップを止める（実時間では `RealTimeDriver`、続いて lifecycle。仮想時間では lifecycle）。
2. SIL Kit のポートを閉じる（ハンドラーを外し、コントローラーを止める）。
3. LIN のノード（それぞれのポートより先）、`Network`（コンポーネントが先）、ポート、ホストを破棄する。
4. participant を破棄する。

## スレッドとキュー

| スレッド | 所属 | 処理 |
|---|---|---|
| SIL Kit I/O | SIL Kit | `SilKitCanPort` の受信ハンドラーと、仮想時間のステップのハンドラーを呼ぶ |
| ドライバー | vehicle-sil（実時間のみ） | `RealTimeDriver` が、ホストのステップ（受信、プロトコル処理、アプリ、送信）を実行する |

`SilKitCanPort` の受信ハンドラーは、フレームに時刻を付けて受信キューに積みます。キューが満杯なら、フレームを捨てて `receive_overflows` に数え、`ReceiveOverflow` のイベントにします。

プロトコル処理（`Network`、コンポーネント、ハンドラー）はロックを持たず、ステップを実行するスレッドだけが操作します。仮想時間では SIL Kit のステップのハンドラー、実時間ではドライバーのスレッドです。ほかのスレッドからは、ホストの `post()` で処理を渡します。

キューの容量は、ホストの `post()` の待ち行列を除いて固定で、生成時に確保します。`post()` の待ち行列は上限がなく、呼ぶたびに確保します。`post()` は外部からの操作のための API で、ステップごとに使うものではありません。

| キュー | 既定の容量 | 備考 |
|---|---|---|
| `SilKitCanPort` の受信キュー | 1024 フレーム | `SilKitCanPortOptions::inbox_capacity` |
| `SilKitCanPort` のイベントのキュー | 1024 | `SilKitCanPortOptions::event_capacity` |
| `Network` の 1 ステップの受信数 | 1024 フレーム | `NetworkOptions::receive_capacity`。ほかの上限は [j1939.md](j1939.md) の「ステップ幅と資源の上限」 |
| サンプル Receiver の受信キュー | 256 | |

## 時刻（実時間モード）

次の場合は、SIL Kit の時刻同期を使いません。autonomous lifecycle で、すべて実時間で動きます。

- `ParticipantRuntimeOptions::virtual_time` が空（既定）の場合
- `PeerOptions::virtual_time` に `std::nullopt` を渡した場合（J1939 の最小ピアと Vector XL CAN Bridge）

`PeerOptions::virtual_time` の既定は、ステップ幅 1 ms の仮想時間です。

- J1939 のタイマー（Address Claim の後の待ちなど）は、ドライバーの時計（`RealTimeClock`）で進みます。
- 受信フレームの時刻は、ポートが時計で付けた受信時刻です。時刻同期をしない participant の SIL Kit のタイムスタンプは使いません（[can.md](can.md) の「実時間での駆動」）。

## SIL Kit の設定の既定

### 設定を渡さないとき

`ParticipantRuntimeOptions::participant_configuration` が `"{}"`（既定）か空なら、`ParticipantRuntime` は `default_participant_configuration(registry_uri)`（`participant_runtime.hpp`）で participant を作ります。SIL Kit の既定から変えるのは 3 つです。

| 条件 | 変えるキー |
|---|---|
| registry の URI のホストがループバックのアドレス | `Middleware.AcceptorUris` で、ループバックのアドレスだけで待ち受ける |
| Windows | `Middleware.EnableDomainSockets: false`（[dependencies.md](../dependencies.md#sil-kitwindows-スレッドを止められたプロセスとの-local-domain-ソケットを閉じると戻らないことがある) の「SIL Kit（Windows）: スレッドを止められたプロセスとの local-domain ソケットを閉じると戻らないことがある」） |
| いつも | `Experimental.TimeSynchronization.DynamicSimulationStep: false` |

`DynamicSimulationStep` のキーがない participant は、ほかの participant（ネットワークのシミュレーターなど）が `true` で参加すると、SIL Kit にステップを全員の最小の幅まで縮められます。`false` なら、自分の幅のままです。

### 待ち受けるアドレス

ループバックのアドレスかどうかは、`is_loopback_registry_uri()` が判定します。ループバックと見るのは、`127.0.0.0/8`（`getaddrinfo()` が読む数の書き方）、`::1`、IPv6 に写した `127.x.x.x`、`localhost` です。名前は解決しません。

ループバックのときに待ち受けるのは、TCP の `127.0.0.1` か `::1` のポート 0 です。Windows 以外では、ほかに、一時ディレクトリの新しい名前の local-domain ソケットでも待ち受けます。

`ParticipantRuntimeOptions::listen_on_network`（`std::optional<bool>`。`SimulationControllerOptions` にも同じもの）で、`AcceptorUris` を追加するかを変えられます。

| `listen_on_network` | `AcceptorUris` |
|---|---|
| `true` | 追加しない |
| `false` | URI だけで決める |
| 空 | 環境変数 `VEHICLE_SIL_LISTEN_ON_NETWORK`（`kListenOnNetworkVariable`）を読む |

| 環境変数の値 | `AcceptorUris` |
|---|---|
| `1` | 追加しない |
| `0`、空、変数なし | URI で決める |
| ほかの値 | `std::invalid_argument` |

`vehicle_sil_launch run` は controller に値を渡し、子にはいつも変数を渡します（[simulation-run.md](../simulation-run.md) の「待ち受けるアドレス」）。

### 自分の設定を渡したとき

自分の設定（YAML か JSON の文字列）を渡したときは、`merged_participant_configuration()` が、その設定に上の既定を重ねます。`ParticipantRuntime` のコンストラクターが呼びます。`Peer`、`SimulationController`、`Simulation` も、このコンストラクターを使います。

まず、設定を SIL Kit に読ませます（`ParticipantConfigurationFromString()`。schema の検査と `Includes` の解決）。エラーは、SIL Kit の `ConfigurationError` のままです。YAML として読めない文字列のときは、SIL Kit のメッセージの後ろに「(at line <行>, column <列> of the configuration text)」を追加します。SIL Kit のメッセージに場所がないことがあるためです。

設定の内容は、読ませた結果（`ParticipantConfigurationToJson()`。これも rapidyaml で読みます）で見ます。`Includes` のどのファイルに書いたキーも、同じに数えます。

ないキーは、次のとおり追加します。追加する先は、渡された文字列そのものです。

| キーの状態 | 追加するもの |
|---|---|
| `Middleware.AcceptorUris` がない | 上の「待ち受けるアドレス」のルールで追加する |
| Windows で、`Middleware.EnableDomainSockets` が `false` でない | `false` にする |
| `DynamicSimulationStep` がない | `false` を追加する |

Windows で `EnableDomainSockets` を `true` と書いても `false` にするのは、SIL Kit の JSON が既定の `true` を書かないためです。`true` と書いた設定を、書いていない設定と見分けられません。

追加するときは、SIL Kit 付属の rapidyaml で文字列を読み、キーを追加して YAML で書き戻します。

- ほかのキーは、書いたとおりに残ります。
- `Experimental:` のように下に何も書いていないキー（コメントだけの場合も）は、SIL Kit が「ない」と読むので、その下に追加します。
- SIL Kit の JSON を設定の代わりに使うことはしません。SIL Kit の JSON は、読んだものの一部を書かないためです（ログの sink の `Format` など）。
- 追加するものがない設定は、1 文字も変えません。

### 拒否する設定

渡した設定が次のどれかなら、`merged_participant_configuration()` は `std::invalid_argument` を投げます。メッセージには、どのキーをどうするかが入ります。

- `ParticipantName` が participant 名と違う。
- `Middleware.RegistryUri` が registry の URI と違う（URI が空なら比べない）。
- `DynamicSimulationStep` が `true`。

SIL Kit は設定の名前と URI を採用するので、そのまま進めると、別の名前で参加するか、別の registry に接続します。同じ値は受け付けます。

### `participant_configuration_from_file()`

`participant_configuration_from_file()` は、ファイルの文字列を返します。`Includes` があれば、ファイルのディレクトリを `Includes.SearchPathHints` に追加します。文字列の設定では、SIL Kit は作業ディレクトリと `SearchPathHints` からしか探さないためです。

中身は検査しません。次のファイルは、文字列をそのまま返します。エラーは、その文字列を使うときに SIL Kit が報告します。

- YAML として読めないファイル
- `Includes` の下が空のファイル
- `SearchPathHints` がリストでないファイル

### rapidyaml のないビルド

rapidyaml のないビルド（SIL Kit をサブモジュールからビルドしないとき）では、渡された設定をそのまま使います。

| すること | rapidyaml のないビルド |
|---|---|
| キーの追加 | しない（待ち受けるアドレスも、その設定のとおり） |
| 上の 3 つの検査（`ParticipantName`、`Middleware.RegistryUri`、`DynamicSimulationStep: true`） | しない |
| `merged_participant_configuration()` が SIL Kit に設定を読ませること | しない（SIL Kit が受け付けない設定は、participant を作るときに分かる） |
| `participant_configuration_from_file()` が `SearchPathHints` を追加すること | しない |
| 設定を渡さないとき（`{}`）の既定 | このビルドでも同じ |

そのビルドでは、Windows の `Middleware: {EnableDomainSockets: false}` と `"Experimental":{"TimeSynchronization":{"DynamicSimulationStep":false}}` を自分で書き、`ParticipantName` と `Middleware.RegistryUri` は書かないでください。

### ステップ幅の検査

仮想時間の `ParticipantRuntime` は、SIL Kit が渡したステップの幅が `VirtualTimeOptions::step` と違うと、ステップのハンドラーを呼ばずに、そのステップを失敗にします。participant は Error になります。`step_error()` は「SIL Kit gave this participant a step of ... ns at t=... ns, not its own ... ns: ...」で、設定で `false` にするキーをメッセージに含めます。ステップの格子、`TickScheduler`、時間ゲートが固定の幅を前提にしているためです。

既定の設定でも、自分の設定でも起きません（キーを追加するため）。起きるのは、rapidyaml のないビルドで、キーのない設定か `true` の設定を渡したときだけです。

幅を比べる所は `libs/core/src/participant_runtime_checks.hpp` の `detail::step_width_error()` で、テスト `core.a_step_that_silkit_cut_fails_with_the_key_that_keeps_the_step` が SIL Kit なしで確認します。

### registry の設定

vehicle-sil が立てる registry（`vehicle_sil_launch run`、`Simulation`）は、`default_registry_configuration()` で作ります。

- registry は、自分の URI のアドレスで待ち受けます（`AcceptorUris` を使いません）。
- どの OS でも `Middleware.EnableDomainSockets: false` です（TCP だけ）。

TCP だけにするのは、次の理由です。SIL Kit の registry は、TCP のポートで待ち受けられなくても local-domain ソケットだけで動き続け、同じ URI のほかの registry の local-domain のパスを奪います。TCP だけにすると、使用中のポートで `StartListening()` がすぐ失敗します。

### 名前が使用中のときの作り直し

`ParticipantRuntime` は participant を `create_participant()` で作ります。registry が名前を使用中として拒否する間（「A participant with the same name ... already exists」）、`ParticipantRuntimeOptions::name_in_use_wait` まで、50 ms おきに作り直します。

- `name_in_use_wait` の既定は 5000 ms です。`PeerOptions` にも同じものがあります。`Simulation` は `start_timeout` を超えない値にします。
- 作り直すのは、同じ名前で起動し直したプロセスでは、registry が前の接続の終わりにまだ気づいていないことがあるためです（遅い PC、ネットワーク越し）。
- 時間を過ぎたら、SIL Kit のメッセージに、待った時間と、ほかの participant が同じ名前を使っているかもしれないことを追加して `SilKit::ProtocolError` です。
- ほかの失敗は、すぐに投げます。
- SIL Kit の participant を自分で作るプログラム（テストの生の Agent など）も、この関数を使えます。

### 設定を作る所の統一

`libs/`、`tools/`、`bridges/`、`tests/` で文字列から SIL Kit の設定を作る所は、これらの関数を使います（テスト `core.silkit_configuration_defaults`）。

## 関連するドキュメント

- 全体像: [architecture.md](../architecture.md)
- どこに何があるか: [code-map.md](../code-map.md)
- ほかの内部の仕組みのドキュメント: architecture.md の「[内部の仕組み](../architecture.md#内部の仕組み)」
