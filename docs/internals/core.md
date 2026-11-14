# participant の土台（`vehicle_sil::core`）

vehicle-sil 自体を直す人と、`ParticipantRuntime` を直接使う人向けです。SIL Kit の participant と lifecycle を持つ `ParticipantRuntime` の契約、所有と終了の順序、スレッドとキュー、実時間モードの時刻を扱います。仮想時間の進み方と時間ゲートは [virtual-time.md](virtual-time.md) にあります。 先に [architecture.md](../architecture.md)（全体の形）と [code-map.md](../code-map.md)（どこに何があるか）を読みます。ほかの中の仕組みの文書は、architecture.md の「[中の仕組み](../architecture.md#中の仕組み)」にあります。

## 公開 API の契約

- `ParticipantRuntime` のコンストラクタはレジストリに接続します。失敗すると `SilKit::SilKitError` を投げます。
- バスのコントローラは `start()` の前に作ります。`start()` が戻った時点で、接続済みの相手はそのコントローラの購読を確認済みです。
- `start(timeout)` は lifecycle が Running になるまで待ちます。タイムアウトや失敗のときは `std::runtime_error` を投げます。戻った直後の `state()` は Running か、その後の状態です（短い実行は、見たときにもう終わっていることがあります。それも開始の成功です）。Running より前の状態（ReadyToRun など）のまま戻ることはありません。
  - 時刻同期のない participant では、SIL Kit は starting のハンドラーを ReadyToRun で呼び、戻ってから Running にします。`start()` は、ハンドラーが呼ばれた印だけでは戻らず、状態が ReadyToRun を離れるまで待ちます。
  - `start()` の待ちの中では、印（starting のハンドラー、最初のステップ）を先に読み、状態をその後で読みます。この順を入れ替えると、ReadyToRun のまま戻る道ができます。逆に、印を読んだ後に Running を通って止まった実行を失敗にしないよう、止まる途中か終わった状態を見たときは印を読み直します。
  - この 2 つの順は、`libs/core/src/participant_runtime_probe.hpp` の差し込み口（試験だけが使う。インストールしない）で順を決めて、`core.participant_start_*` が確かめます。
- `add_communication_ready_handler(handler)`: SIL Kit の CommunicationReady（SIL Kit のスレッドの上、最初のステップの前。autonomous の participant でも同じスレッドです）で呼ぶハンドラーを足します。コントローラーを始める所です（`SilKitCanPort::start()`、`SilKitEthPort::start()`、LIN の `init()`、ブリッジの `start_controller()`。SIL Kit は、CAN の `SetBaudRate` と `Start`、LIN の `Init`、Ethernet の `Activate` を、ここで呼ぶよう勧めています）。足した順に、`set_communication_ready_handler()` や `set_communication_ready_handler_async()` のハンドラーより前に呼ばれ、互いに置き換えません。足したハンドラーの例外は、participant の失敗として報告し（`report_failure()`、「CommunicationReady handler failed: ...」）、残りのハンドラーは呼びます。`Peer` も自分のコントローラー（CAN の `start()`、LIN の `init()`）をこれで足すので、`peer.runtime()` に `set_communication_ready_handler()` でハンドラーを設定しても、置き換わりません（`Peer` は、失敗したコントローラーのネットワークの名前を付けて `report_failure()` に渡します）。`start()` の前にだけ呼べます。ハンドラーが使うものは、`stop()` が戻るまで（または `ParticipantRuntime` を破棄するまで）生かしておきます。
- `set_communication_ready_handler_async(handler)` と `complete_communication_ready()`: ハンドラーが戻った後も、`complete_communication_ready()` を呼ぶまで、participant は最初のステップの前（CommunicationInitialized）にとどまります。Coordinated では全員がそこで待つので、その間に publish/subscribe でやりとりできます（ハンドラーの中ではできません）。完了は、ハンドラーが起こした別のスレッドから呼びます（ハンドラーの中からは呼べません。ハンドラーが呼ばれる前と、2 回目は何もせずに偽を返します）。完了するまで誰のステップも始まらないので、窓の用が失敗したときも必ず完了させます。`stop()` が戻った後に窓が開く場合は、ハンドラーを呼ばず、runtime が自分で完了させます。`SimulationController::set_start_window()` がこれを使います（[control.md](control.md) の「SIL Kit の上の運び手」）。
- `set_peer_status_observer(observer)`: ほかの participant（と自分）のライフサイクルの状態がわかるたびに、名前と状態を渡して呼びます。呼ぶのは、それを知ったスレッド（SIL Kit のスレッドか、状態を読み直すスレッド）で、1 つずつです。同じ状態でもう一度呼ぶことがあります。短く、`peer_status()` のほかはこの runtime を呼ばないこと。`start()` の前に登録します。
- `stop()` は何度呼んでもよく、例外を投げません。
- `stop()` が戻った後は、ステップのハンドラーも CommunicationReady のハンドラー（設定したもの、足したもの、非同期のもの）も呼ばれません。動いている途中のハンドラーは、終わるまで待ちます。ハンドラーが使うもの（ポート、ブリッジ、スタック）は、`stop()` の後で壊せます。
  - CommunicationReady のハンドラーにこの約束が要るのは、開始が Running の前に失敗したとき（時間切れ、`give_up`）です。SIL Kit は Running の前の停止を受け付けないので、`stop()` の後も lifecycle は `ParticipantRuntime` を破棄するまで生きていて、待っていた相手が遅れて来ると CommunicationReady に進みます。そのときは、ハンドラーを呼ばずに先へ進み（コントローラーは始まらず、ステップも動きません）、停止が届く所で止まります。
  - `stop()` が終了を待っている間に来た CommunicationReady では、ハンドラーをふつうに呼びます。
  - `stop()` は、SIL Kit のハンドラー（ステップ、CommunicationReady）の中から呼べません（自分を待って止まります）。そこでは `request_stop()` を使います。`report_failure()` と `request_stop()` は、どのハンドラーの中からも呼べます。
  - 確かめる試験は `core.communication_ready_after_stop_*` と `core.communication_ready_handler_that_runs_is_waited_for_by_stop` です。
- participant が失敗すると（ステップのハンドラ、実時間のドライバー、`report_failure()`）、自分の `stop()` と `request_stop()` は、Error になるまで lifecycle に停止を渡しません（先にふつうの停止が渡ると、Coordinated ではシミュレーション全体が正常に止まり、controller から失敗が見えないためです）。失敗の印を立てることと SIL Kit に Error を報告することは 1 つのロックの下で行うので、失敗が見えた後の停止が報告を追い越すことはありません。SIL Kit が後回しにした処理を実行する順序には頼りません。`stop()` の待ちは Error と Shutdown を合わせて `shutdown_timeout` までで、過ぎれば Error の前でも停止を渡します。次の場合は Error を経ないか、Stopping の後で Error になります。失敗より前に届いた `request_stop()`（失敗するステップのハンドラが始まる前、`report_failure()` の前）は、ふつうの停止です。controller の中断やほかの participant の停止が先に届いたときも同じです。`report_failure()` は、`start()` が lifecycle を始めるまでは何もしません。
  - `report_failure()` は、`start()` の前には何もしません（SIL Kit が無視するためです）。`start()` が lifecycle を始めている最中の呼び出し（autonomous の participant の CommunicationReady のハンドラーは、`start()` がそこから戻る前に呼ばれることがあります）は覚えておき、`start()` が報告します。この順は、差し込み口で決めて `core.participant_start_reports_a_failure_*` が確かめます。
- `bind_host(runtime, host)`（`vehicle_sil/core/bind_host.hpp`）: `ParticipantRuntime` のステップのハンドラから、`vehicle_sil::step::Host` の `step(now)` を呼ぶようにします。最初のステップの前に、participant のステップ幅を `Host::set_step_size()` に渡します。仮想時間の runtime だけに使えて（それ以外は `std::invalid_argument`）、ホストに `Virtual` の時間の領域を求めます（`std::logic_error`）。`start()` の前に呼びます。ホストの例外は、ステップのハンドラの失敗として participant を Error にします。ホストは runtime のステップより長く生かします（runtime を先に止めます）。`core` はこのために `vehicle_sil::step` をリンクしますが、バスの種類には依存しません（[can.md](can.md) の「仮想時間での駆動」）。
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

`ParticipantRuntimeOptions::virtual_time` が空（既定）の場合と、`PeerOptions::virtual_time` に `std::nullopt` を渡した場合（J1939 の最小ピアと Vector XL CAN Bridge）は、SIL Kit の時刻同期を使いません。`PeerOptions::virtual_time` の既定は、ステップ幅 1 ms の仮想時間です。autonomous lifecycle で、すべて実時間で動きます。

- J1939 のタイマー（Address Claim の後の待ちなど）は、ドライバーの時計（`RealTimeClock`）で進みます。
- 受信フレームの時刻は、ポートが時計で付けた到着時刻です。時刻同期をしない participant の SIL Kit のタイムスタンプは使いません（[can.md](can.md) の「実時間での駆動」）。

## SIL Kit の構成の既定

- `ParticipantRuntimeOptions::participant_configuration` が `"{}"`（既定）か空なら、`ParticipantRuntime` は `default_participant_configuration(registry_uri)`（`participant_runtime.hpp`）で participant を作ります。SIL Kit の既定から変えるのは 3 つです。
  - registry の URI のホストがループバックのアドレス（`is_loopback_registry_uri()`: `127.0.0.0/8` を `getaddrinfo()` が読む数の書き方で、`::1`、IPv6 に写した `127.x.x.x`、`localhost`。名前は引きません）なら、`Middleware.AcceptorUris` でループバックのアドレスだけで待ち受けます（TCP の `127.0.0.1` か `::1`、ポート 0。Windows 以外では、ほかに一時ディレクトリの新しい名前の local-domain ソケット）。
  - `ParticipantRuntimeOptions::listen_on_network`（`std::optional<bool>`。`SimulationControllerOptions` にも同じもの）が `true` なら足さず、`false` なら URI だけで決めます。空なら環境変数 `VEHICLE_SIL_LISTEN_ON_NETWORK`（`kListenOnNetworkVariable`）を読みます: `1` なら足さない、`0`・空・なしなら URI で決める、ほかの値は `std::invalid_argument`。`vehicle_sil_launch run` は controller に値を渡し、子にはいつも変数を渡します（[simulation-run.md](../simulation-run.md) の「待ち受けるアドレス」）。
  - Windows では `Middleware.EnableDomainSockets: false`（[dependencies.md](../dependencies.md) の SIL Kit（Windows）の節）。
  - いつも `Experimental.TimeSynchronization.DynamicSimulationStep: false`。このキーがない participant は、ほかの participant（ネットワークのシミュレーターなど）が `true` で参加すると、SIL Kit にステップを全員の最小の幅まで縮められます。`false` なら、自分の幅のままです。
- 自分の構成（YAML か JSON の文字列）を渡したときは、`merged_participant_configuration()` が、その構成に上の既定を重ねます。`ParticipantRuntime`（`Peer`、`SimulationController`、`Simulation` もこれを通ります）のコンストラクターが呼びます。
  - まず SIL Kit に読ませます（`ParticipantConfigurationFromString()`。schema の検査と `Includes` の解決。誤りは SIL Kit の `ConfigurationError` のまま。YAML として読めない文字列のときは、SIL Kit の文の後ろに「(at line <行>, column <列> of the configuration text)」を足します。SIL Kit の文に場所がないことがあるためです）。構成が何を言っているかは、その結果（`ParticipantConfigurationToJson()`。これも rapidyaml で読みます）で見ます。`Includes` のどのファイルに書いたキーも、同じに数えます。
  - `Middleware.AcceptorUris` がなければ、上の決まりで足します。Windows で `Middleware.EnableDomainSockets` が `false` でなければ `false` にします（SIL Kit の JSON は既定の `true` を書かないので、`true` と書いた構成を、書いていない構成と見分けられません）。`DynamicSimulationStep` がなければ `false` を足します。
  - 足す先は、渡された文字列そのものです（SIL Kit 付属の rapidyaml で読み、キーを足して YAML で書き戻す）。ほかのキーは、書いたとおりに残ります。`Experimental:` のように下に何も書いていないキー（コメントだけの場合も）は、SIL Kit が「ない」と読むので、その下に足します。SIL Kit の JSON を構成の代わりに使うことはしません。SIL Kit の JSON は、読んだものの一部を書かないためです（ログの sink の `Format` など）。足すものがない構成は、1 文字も変えません。
  - `ParticipantName` が participant 名と違う、`Middleware.RegistryUri` が registry の URI と違う（URI が空なら比べない）、`DynamicSimulationStep` が `true`、のどれかなら `std::invalid_argument` です（どのキーをどうするかを言います）。SIL Kit は構成の名前と URI を採るので、そのまま進めると、別の名前で参加するか、別の registry につながります。同じ値は通します。
  - `participant_configuration_from_file()` は、ファイルの文字列を返します。`Includes` があれば、ファイルのディレクトリを `Includes.SearchPathHints` に足します（文字列の構成では、SIL Kit は作業ディレクトリと `SearchPathHints` からしか探さないため）。中身は確かめません。YAML として読めないファイル、`Includes` の下が空のファイル、`SearchPathHints` がリストでないファイルは、文字列をそのまま返します（誤りは、その文字列を使うときに SIL Kit が言います）。
  - rapidyaml のないビルド（SIL Kit をサブモジュールからビルドしないとき）では、渡された構成をそのまま使います。キーは足しません（待ち受けるアドレスも、その構成のとおり）。上の 3 つの検査（`ParticipantName`、`Middleware.RegistryUri`、`DynamicSimulationStep: true`）もしません。`merged_participant_configuration()` は SIL Kit に読ませることもしないので、SIL Kit が受け付けない構成は、participant を作るときに分かります。`participant_configuration_from_file()` は `SearchPathHints` を足しません。そのビルドでは、Windows の `Middleware: {EnableDomainSockets: false}` と `"Experimental":{"TimeSynchronization":{"DynamicSimulationStep":false}}` を自分で書き、`ParticipantName` と `Middleware.RegistryUri` は書かないでください。構成を渡さないとき（`{}`）の既定は、このビルドでも同じです。
- 仮想時間の `ParticipantRuntime` は、SIL Kit が渡したステップの幅が `VirtualTimeOptions::step` と違うと、ステップのハンドラーを呼ばずにそのステップを失敗にします（participant は Error、`step_error()` は「SIL Kit gave this participant a step of ... ns at t=... ns, not its own ... ns: ...」で、構成で `false` にするキーを言います）。ステップの格子、`TickScheduler`、時間ゲートが固定の幅を前提にしているためです。既定の構成でも、自分の構成でも起きません（キーを足すため）。起きるのは、rapidyaml のないビルドで、キーのない構成か `true` の構成を渡したときだけです。幅を比べる所は `libs/core/src/participant_runtime_checks.hpp` の `detail::step_width_error()` で、試験 `core.a_step_that_silkit_cut_fails_with_the_key_that_keeps_the_step` が SIL Kit なしで確かめます。
- vehicle-sil が立てる registry（`vehicle_sil_launch run`、`Simulation`）は `default_registry_configuration()` で作ります。registry は自分の URI のアドレスで待ち受けます（`AcceptorUris` を使いません）。どの OS でも `Middleware.EnableDomainSockets: false` です（TCP だけ）。SIL Kit の registry は、TCP のポートで待ち受けられなくても local-domain ソケットだけで動き続け、同じ URI のほかの registry の local-domain のパスを奪います。TCP だけにすると、ふさがったポートで `StartListening()` がすぐ失敗します。
- `ParticipantRuntime` は participant を `create_participant()` で作ります。registry が名前を使用中として断る間（「A participant with the same name ... already exists」）、`ParticipantRuntimeOptions::name_in_use_wait`（既定 5000 ms。`PeerOptions` にも同じもの。`Simulation` は `start_timeout` を超えない値にします）まで、50 ms おきに作り直します。同じ名前で起動し直したプロセスでは、registry が前の接続の終わりにまだ気づいていないことがあるためです（遅い機械、ネットワーク越し）。過ぎたら、SIL Kit の文に、待った時間と、ほかの participant が同じ名前を使っているかもしれないことを足して `SilKit::ProtocolError` です。ほかの失敗はすぐに投げます。SIL Kit の participant を自分で作るプログラム（試験の生の Agent など）も、この関数を使えます。
- `libs/`、`tools/`、`bridges/`、`tests/` で文字列から SIL Kit の構成を作る所は、これらの関数を通ります（試験 `core.silkit_configuration_defaults`）。
