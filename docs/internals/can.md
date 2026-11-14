# CAN の共通基盤

vehicle-sil 自体を直す人と、CAN の上に自分のプロトコルを作る人向けです。ポート、ホスト、ドライバー、仮想バス、SIL Kit のポート、バスのタイミング、簡易 CAN Network Simulator の契約を扱います。 先に [architecture.md](../architecture.md)（全体の形）と [code-map.md](../code-map.md)（どこに何があるか）を読みます。ほかの中の仕組みの文書は、architecture.md の「[中の仕組み](../architecture.md#中の仕組み)」にあります。

`vehicle_sil::can` と `vehicle_sil::can_silkit` は、J1939 スタック（`vehicle_sil::j1939`）の土台です。CAN の 11 ビットと 29 ビットのフレームを扱い、J1939 以外のプロトコルも載せられます。API はまだ変わることがあります。

- `vehicle_sil::can` は SIL Kit と Vector XL に依存しません。SIL Kit をリンクしない単体テストの実行ファイルをビルドすることで、毎回確かめます。
- ステップを回す仕組み（ホスト、ドライバー、時計、時間の領域）は、バスの種類によらない `vehicle_sil::step` にあります。`vehicle_sil::can` の `Host` はその上に作ってあり、`StepDriver`、`RealTimeDriver`、`RealTimeClock`、`TimeDomain` などは `vehicle_sil::step` のものの別名です（どちらの名前でも同じ型です）。
- 時刻はすべて `SimTime` です。プロトコル処理はスレッドを持たず、時計を読みません。仮想時間では SIL Kit または `StepDriver` の時刻、実時間では `RealTimeDriver` の時計（開始からの経過時間）です。

## 値の型

- `Frame` はクラシック CAN のデータフレームです（RTR、CAN FD、CAN XL は表しません）。`length` はバイト数（0〜8）で、DLC の符号ではありません。比較は ID、形式（11 ビット / 29 ビット）、`length` までのデータで行います。
- `TimedFrame` は時刻付きのフレームです。受信ではバックエンドが付けた時刻、送信では送るステップの時刻です。

## ポートの契約（`Port`）

- `release_before(now, sink)` は、時刻が `now` より前の受信フレームを、時刻順、同じ時刻なら到着順に渡します。同じ時刻のフレームを並べ替えません。最終的な順序はプロトコル処理が決めます。時刻がちょうど `now` のフレームは次の呼び出しで渡します。
- 自分が送ったフレームは受信しません（エコーと送信の確認はポートで捨てます）。
- `send()` は同期の結果を返します。`Accepted`（受け付けた）、`QueueFull`（送信キューが満杯。後のステップで出し直せる）、`Failed`（送れないフレーム、バックエンドの誤り）です。
- `send()` の後にフレームがどうなったか（送信された、バックエンドが捨てた）をバックエンドが知るときは、後から事象（`PortEvent`）で知らせます。`release_events_before(now, sink)` は、時刻が `now` より前の事象を、時刻順、同じ時刻なら到着順に渡します。既定の実装は事象を出しません。
- `release_before()`、`release_events_before()`、`send()` は、ドライバーのスレッドだけから呼びます。
- ポートは時刻の領域を持ちます。`Virtual`（仮想時間。仮想バス、時刻同期ありの SIL Kit）と `RealTime`（実時間ドライバーの時計。時刻同期なしの SIL Kit、Vector XL）です。
- `RealTime` のポートは、受けたフレームと事象に、ドライバーと共有する時計で到着時刻を付けます（下の「実時間での駆動」）。到着のたびに `set_wakeup()` で渡された関数を到着時刻付きで呼び、ドライバーを起こします。`Virtual` のポートは `set_wakeup()` を無視します。

## ポートの事象（`PortEvent`）

自分が送ったフレームについて、`send()` の同期の結果の後に分かったことです。

| 種類 | 意味 | 時刻 |
|---|---|---|
| `TransmitCompleted` | フレームが送信された | バスを模擬するバックエンドでは送信の終わり。それ以外ではバックエンドが送信を確認した時刻 |
| `TransmitFailed` | `send()` が受け付けたフレームを、バックエンドが送らなかった（送信キューの満杯、取り消し、バスから切り離されている） | バックエンドが知らせた時刻 |
| `ReceiveOverflow` | 受け箱が満杯で受信フレームを捨てた。`count` は捨てた数（0 は数が分からない分を含む） | 捨てたフレームのうち最も早い時刻 |
| `BusStateChanged` | コントローラーのエラー状態が `bus_state`（`ErrorActive`、`ErrorPassive`、`BusOff`）になった | 変わった時刻 |
| `ErrorFrame` | バスでエラーフレームを見た | 見た時刻 |

- 送信の事象は CAN 識別子（`id`）と、分かれば送ったときの `TimedFrame::time`（`send_time`）を持ちます。同じ識別子のフレームは `send_time` で見分けます。データは持ちません。
- `ReceiveOverflow` は、捨てたフレームごとには出しません。ポートが次に事象を渡すまでに捨てた数を、1 つの事象にまとめます。この事象は事象の受け箱の枠を使わないので、受け箱が満杯でも届き、送信の事象を押し出しません。
- バックエンドが知り得ない種類は出しません（報告なし）。

| バックエンド | 出す事象 |
|---|---|
| 仮想バス | タイミングありなら `TransmitCompleted`。`ReceiveOverflow`。故障注入と通信断の `TransmitFailed`。注入した任意の事象 |
| `SilKitCanPort` | SIL Kit の送信確認から `TransmitCompleted` と `TransmitFailed`。SIL Kit のエラー状態の通知から `BusStateChanged`（ネットワークシミュレーターが出す。SIL Kit の簡易シミュレーションは出さない）。`ReceiveOverflow` |
| `ExternalPortAdapter`（Vector XL） | `ErrorFrame`。外部ポートのあふれは数の分からない `ReceiveOverflow`。バス状態と送信の結果は出さない |

- `Host` は、ステップの段階 2 で、そのステップのフレームより先に事象を `Steppable::on_port_event()` に渡します。`on_port_event()` は記録だけにします。送信はしません（送信は `flush()` で行います）。
- J1939 スタックは、事象を `NetworkStatistics`（`frames_transmitted`、`transmit_failures`、`port_receive_overflows`、`port_overflow_events`、`bus_state_changes`、`error_frames`）に数え、最新のバス状態を `Network::bus_state()` に保つだけです。プロトコルの処理には使いません（バスオフでも動作を変えません）。
- 種類はこれから増えることがあります。`Steppable` は知らない種類を無視します。

## `Steppable` の契約

CAN の上にプロトコルスタックを作るときの拡張点です。J1939 の `Network` はこの契約で作っています。

- ホストは 1 ステップに、`begin(now)`、ポートの事象ごとの `on_port_event()`、フレームごとの `receive()`、`process()`、（アプリの処理の後に）`flush(port)` の順で呼びます。最初のステップの前に `start(step_size)` を 1 回呼びます。
- `now` は呼び出しごとに増えます。`Steppable` はスレッドを持たず、時計を読みません。時刻は `now` と、フレームと事象の時刻だけです。呼び出しは常に 1 つのスレッド（ドライバーのスレッド）からで、ロックは要りません。
- `receive()` と `on_port_event()` では記録だけにします。処理は `process()` で、送信は `flush()` でポートに出します。
- `receive()` に渡るフレームの時刻は `now` より前です。同じ時刻の、別の送信者のフレームの順序はバックエンドによって違うので、`Steppable` が決まった規則で並べます。
- `start()` は、そのプロトコルが動けないステップ幅なら例外を投げます（ホストが失敗状態になります）。実時間では、ドライバーの `max_idle` がステップ幅として渡ります。
- `next_deadline()` は、何も届かなくてもステップが要る最も早い時刻（タイマーの期限）を返します。実時間のドライバーはそれまで眠ります。タイマーのない仕事（`QueueFull` で残したフレーム、1 ステップの上限で繰り越したフレーム）は含めません。それらは実時間ではドライバーの `max_idle` の後のステップで出ます。ステップの後も過去の期限を返し続けるのは契約違反です（手動の時計では次のステップを 1 ns 後に 1 回回すだけですが、壁時計ではステップを回し続けます）。
- 例外を投げると、ホストは失敗状態になり、以後のステップはすべて失敗します。

## ホスト（`Host`）

- `vehicle_sil::step::Host` は、ステップの参加者（`step::Member`）を登録順に持ち、段階ごとに進めます。`Member` は 1 つのチャネルのプロトコル処理とそのポートの組で、`begin(now)`、`receive(now)`（ポートから `now` より前のものを受け取る）、`process()`、`flush()`、`start(step_size)`、`next_deadline()`、`time_domain()`、`set_wakeup()` を持ちます。バスの種類を知りません。
- `vehicle_sil::can::Host` は `step::Host` の派生で、`add(Steppable&, Port&)` で渡された組を中で `Member` にします（`receive(now)` は、ポートの事象を `on_port_event()` に、フレームを `receive()` に渡します）。組の入れ物はホストが持ちますが、`Steppable` とポートは持ちません。以下は CAN の組で見た契約です。
- 1 つの participant（またはテストのループ）の、プロトコル処理（`Steppable`）とポートの組を登録順に持ちます。1 ステップは、全部の組に対して段階ごとに進めます。(1) `begin(now)`、(2) ポートが渡す事象の `on_port_event()` とフレームの `receive()`、(3) `process()`、(4) アプリの処理を 1 回、(5) `flush(port)`。
- ステップの時刻は前のステップより大きくなければなりません。
- `step()` から例外が出ると（プロトコル処理、アプリ、ポートのどれからでも、使い方の誤りでも）、ホストは失敗状態になり、例外を呼び出し元に返します。以後の `step()` はすべて `std::logic_error` です。回復するには作り直します。
- 登録は最初のステップの前だけです。ホストは `Steppable` とポートを所有しません。起こし方が設定された後の登録では、ホストがポートの `set_wakeup()` を呼びます。それが例外を投げると、組は登録されずに例外が返ります（同じ組をもう一度登録できます）。
- ステップ幅: `set_step_size()` で受け取ったステップ幅を、最初のステップの `begin()` の前に、各 `Steppable` の `start()` に登録順で 1 回渡します（設定がなければ値なし）。`StepDriver::add()`、`bind_host()`、`RealTimeDriver` が設定します。`start()` が例外を投げると、ホストは失敗状態になります（そのプロトコルが動けないステップ幅の構成エラー）。
- `post(fn)`: ステップの外（別のスレッド、SIL Kit の他のハンドラ）からプロトコルスタックに処理を渡します。どのスレッドからでも呼べます。`fn` は次のステップの段階 4 の最初（アプリの処理の前）に、渡された順でドライバーのスレッドで実行します。ステップの中で渡した処理は次のステップです。`fn` の例外はアプリの例外と同じくホストを失敗させ、失敗したホストに渡した処理は実行しません。仮想時間でも使えますが、どのステップで実行されるかは `post()` を呼んだ時点で決まるので、決定的に扱いたいデータは受け箱（`StepInbox`）で受けてステップの中で処理します。
- 時間の領域: 1 つのホストのポートは同じ領域でなければなりません。ドライバーは自分の領域を要求します（`StepDriver` と `bind_host()` は `Virtual`、`RealTimeDriver` は `RealTime`）。混在は登録の時点で `std::logic_error` です（例: Vector XL のポートを SIL Kit の時刻同期で回すホストに入れる）。
- `next_deadline()` は全 `Steppable` の `next_deadline()` の最小です。

## 仮想時間での駆動

- `StepDriver`（`vehicle_sil::step`）は、SIL Kit を使わずに複数のホストを仮想時間で進めます。各ホストのステップ幅は別々でよく、ステップは（開始時刻、登録順）の順に実行します。SIL Kit の時刻同期が許す順序の 1 つです。
- テストのループでの終了の順序: そのホストを進める `StepDriver` を使うのをやめる → プロトコル処理とホストを破棄 → ポート（仮想バスが所有）を破棄。
- SIL Kit では `bind_host(runtime, host)` を使います。`ParticipantRuntime` のステップハンドラから `host.step(now)` を呼びます。仮想時間を使わない `ParticipantRuntime` には結び付けられません。本体は `vehicle_sil::core` の `vehicle_sil::bind_host()`（`step::Host` を受ける）で、`vehicle_sil::can::bind_host` は同じ関数です。

## 実時間での駆動（`RealTimeDriver`）

- `RealTimeDriver` と時計は `vehicle_sil::step` のもので、どの `step::Host` も回せます。時刻同期なしの SIL Kit や Vector XL のポートを持つホストを、壁時計で回します。participant ごとに 1 つ、スレッドを 1 本持ちます。プロトコルスタックにはロックがなく、触るのはこのスレッドだけです。アプリはホストの `post()` とアプリの処理で関わります。
- 時計（`RealTimeClock`）は差し替えられます。製品では `SteadyRealTimeClock`（生成時からの経過時間）、試験では `ManualRealTimeClock`（`advance_to()` で進めるとドライバーを起こす）を使います。ドライバーとホストのポートは同じ時計を使います。
- 登録（`Host::add()`、`set_application()`）は `start()` の前に済ませます。`start()` の直後に、ドライバーのスレッドで最初のステップが回ります。
- ステップを回すのは、開始の直後、ポートへの到着や `post()` のとき、ホストの `next_deadline()` の 1 ns 後（期限より後で発火するタイムアウトのため）、前のステップから `max_idle`（既定 10 ms）が過ぎたときです。
- ステップの時刻は時計の値ですが、前のステップより少なくとも 1 ns 後、起こした到着の時刻より少なくとも 1 ns 後にします。時計が進んでいなくても、届いたフレームは次のステップで必ず渡ります。
- 到着時刻（`RealTimeReceiver`）: ロックの中で、時計の値、前回渡した `now`、前の到着時刻 + 1 ns の最大を付けます。処理済みのステップより前の時刻にはならず、到着の順も保たれます。
- ステップが例外を投げると、スレッドは止まり、`failed()` と `error()` で理由が分かります（ホストも失敗状態）。
- `wait_idle(timeout)` は、スレッドがやることなく眠るまで待ちます。`ManualRealTimeClock` と組み合わせると、`advance_to()` の後に、その時刻までのステップが終わったことを確かめられます。
- 終了の順序: `RealTimeDriver` を止める（`stop()` か破棄。ホストとポートから起こしを外し、スレッドを join する）→ ポートを閉じる → プロトコルスタックとホストを破棄 → `ParticipantRuntime`（またはドライバー）を破棄。`stop()` をステップの中から呼んだ場合は、スレッドに終わるよう伝えるだけで、join は後の `stop()` で行います。
- 決定論はありません。壁時計の試験では、到着と順序だけを確かめます。

## 仮想バス（`VirtualCanBus`）

- `attach()` が、バス上の 1 つの CAN コントローラーにあたるポートを作ります。
- タイミングなし（既定）: 時刻 t で送ったフレームは、他の全ポートに時刻 t のまま届きます。調停と送信時間はありません。
- タイミングあり（`VirtualCanBusOptions::timing`）: フレームは下の「バスのタイミング」の規則でバスに出て、送信の終わりの時刻で他のポートに届きます。送ったポートには同じ時刻の `TransmitCompleted` が届きます。ポートごとの送信キュー（`transmit_queue_capacity`、既定 64）が満杯なら `send()` は `QueueFull` です。数えるのは、送って、バスを決めた時点でまだ始まっていないフレームです（`Host` は送る前に同じステップの受信を取り出すので、ステップの時刻の状態です）。バスは遅れて決めます。どれかのポートが `now` より前のフレームか事象を取り出すとき、`now` より前に始まる送信をすべて決めます。
- すでに決まった時刻より古いフレームは送れません（`std::logic_error`）。タイミングなしでは受信側がすでに受け取った時刻、タイミングありでは決めたバスの時刻です。ホストを時刻順に進めなかったことを示します。`StepDriver` は常に時刻順に進めます。
- 各ポートの受け箱は容量が固定です。入らないフレームは捨てて数えます（`overflows()`）。仮想バスではあふれも決定的ですが、フレームが失われるので、テストではあふれがないことを確かめます。
- 故障注入（試験用）: `Endpoint::set_fault_injector(f)` で、そのポートが送るフレームごとに `f` が `FaultAction` を返します。`Deliver`（通常）、`Drop`（`Accepted` を返して、バスの他のポートに届けない。送った側の Network の中のローカル折り返しは `Accepted` なので届く）、`Duplicate`（同じ時刻で 2 回届ける）、`Delay`（時刻 + 遅れで届ける。後から送ったフレームに追い越されるので、並べ替えもこれで作る。遅れは 0 より大きいこと）、`QueueFull`、`Failed`（その結果を返して届けない）です。不正なフレームは `f` を呼ぶ前に `Failed` です。`f` は送った順に 1 回ずつ呼ばれるので、決定的な関数なら結果も決定的です。タイミングありでは、`Drop` のフレームはバスを占有してからどこにも届かず（送ったポートには `TransmitCompleted` が届きます）、`Duplicate` は 2 回送信し、`Delay` は送った時刻 + 遅れからバスを争います。
- 受け箱のあふれは `ReceiveOverflow` でも知らせます。`Endpoint::inject_event(event)` は、任意の事象（バス状態、エラーフレームなど）をその端点に積みます。`FaultAction::transmit_failed()` は、`Accepted` を返してどこにも届けず、送った側に送信時刻の `TransmitFailed` を出します（非同期の送信失敗。タイミングありでもバスを使いません）。
- ECU 単位の通信断: `Endpoint::add_outage(from, until, direction)` は、その端点を [from, until) の間バスから切り離します。`Receive` はその間に届く時刻のフレームを受けず（`outage_dropped()`）、`Send` はその間の送信時刻のフレームをどこにも届けず、送った側に `TransmitFailed` を出します（`outage_lost()`。故障注入の関数は呼びません。タイミングありでもバスを使いません）。送信は送信時刻で判定するので、タイミングありでは、断の前に送ってバスを待っているフレームは断の間にも出ます。`Both` は両方です。フレームの時刻で決まるので、結果は決定的です。
- 1 つのスレッドから使います。

## 生フレームの観測（`TapPort`）

- `TapPort(inner, listener)` はポートを包み、すべてを `inner` に渡します。プロトコルスタックが受け取ったフレーム（`Received`、フレームの時刻）、送ったフレーム（`Sent`、送ったステップの時刻と `send()` の戻り値）、ポートの事象（`Event`、事象の時刻）を、通った順に `TapListener::on_tap()` へ知らせます。11 ビットと 29 ビットの両方をそのまま運びます。`Received` の順はポートの解放順（時刻順、同じ時刻なら到着順）で、SIL Kit のポートでは同じ時刻の別の送信者のフレームの順が実行ごとに変わりえます。実行の間で比べるときは（時刻、CAN ID）で並べ直すか、並べ替えた後の `Observer` の事象を使います。
- 読み取り専用です。注入はしません（試験の刺激は仮想バスの端点から送ります）。例外を投げないリスナーなら、リスナーの有無で、スタックが受け取るものと送った結果は変わりません。リスナーの例外は呼び出し元に伝わり（ホストは失敗します）、受信のフレームと事象はスタックに渡す前に知らせるので、そのフレームはスタックに渡りません。リスナーの中からその `TapPort` の `send()`、`release_before()`、`release_events_before()`、`set_listener()` を呼ぶと `std::logic_error` です。
- 時間の領域と wakeup は包んだポートのものです。包んだポートと同じく、ドライバーのスレッドから使います。リスナーは 1 つです。リスナーと `TapPort` は互いの登録を覚えており、どちらを先に破棄しても安全です。

## SIL Kit のポート（`SilKitCanPort`）

- 生成時に、CAN コントローラーに受信（RX 方向だけ）と送信確認のハンドラを付けます（ここから届くフレームは受け箱に入ります）。コントローラーを始めるのは `start()` で、participant の CommunicationReady のハンドラーから呼びます（`runtime.add_communication_ready_handler([&port] { port.start(); })`。`Peer` は自分のポートについて呼びます）。SIL Kit が `SetBaudRate()` と `Start()` を呼ぶよう勧めている時点です: SIL Kit のスレッドの上で、最初のステップより前で、coordinated の participant では全員のサービスが見えています。そのため、ネットワークシミュレーターには、起動の順によらずビットレートと開始が届きます。autonomous の participant には、この保証がありません（まだ見つけていないシミュレーターには届かず、送り直されません）。
- `start()` の前にステップが来ると（`release_before()` か `send()`）、`std::logic_error` です（「... before start(): the controller is not started ...」。何を呼ぶかを言います）。SIL Kit の簡易シミュレーションは、始めていないコントローラーの送信を、送信の確認なしに捨てるためです。`start()` は、`close()` の後と 2 回目には何もしません。
- `close()` はハンドラを外し、（始めていれば）コントローラーを止め、受け箱を空にします。`close()` から戻った後は、SIL Kit がポートを呼びません。
- 受信のコールバックは、SIL Kit のタイムスタンプ（送信側のステップ時刻）を付けて受け箱に積むだけです。クラシック CAN のデータフレーム以外は捨てて数えます。
- 「ステップ T の時点で、T より前のフレームは全部届いている」のは、送り手が自分のステップの中で送ったフレームだけです（SIL Kit の約束の範囲。vehicle-sil のポートはステップの中で送ります）。ほかの SIL Kit のプログラムがステップの外（別のスレッド、受信のハンドラー）から送ったフレームは、どのステップで渡るかが実行ごとに変わりえます。
- 時刻同期に参加していない送信者（Vector XL CAN Bridge など）のフレーム（仮想時間）: SIL Kit は、受け手のこの participant の今のステップの時刻を付けます。ただし、この participant の最初のステップの前に届いたフレームは `std::chrono::nanoseconds::min()` のままです。ポートはこの時刻を渡しません。時刻が負のフレームと事象には、届いた時点でのこの participant の最新のステップの時刻（最初のステップの前なら 0）を付け、次のステップで渡します。数は統計の `received_without_time` にあります。どのステップで届くかは届いた時点で決まるので、決定的ではありません。
- 送信は `SendFrame()` です。時刻は SIL Kit が送信側のステップ時刻を付けます。SIL Kit の簡易シミュレーションには同期の結果がないので、正しいフレームは `Accepted` です。
- `SendFrame()` の userContext には、送ったステップの時刻（`TimedFrame::time`）を目印付きで入れます（`encode_send_time()`。上位 8 ビットが目印 0xA5、下位 56 ビットが ns）。簡易 CAN Network Simulator はこれで送信時刻を知ります。SIL Kit は userContext を送信確認で送信側に返すだけで、他の受信者には渡しません。64 ビットのプラットフォームだけを対象にします。
- 送信確認は数えて、`PortEvent` として事象の受け箱に積みます（`Transmitted` は `TransmitCompleted`、`Canceled` と `TransmitQueueFull` は `TransmitFailed`）。SIL Kit のハンドラからプロトコル処理は呼びません（簡易シミュレーションでは送信確認が `SendFrame()` の中で返るので、呼ぶと再入になります）。時刻は、簡易シミュレーションでは送ったステップの時刻、ネットワークシミュレーターでは送信の終わりです。
- `SilKitCanPortOptions::bitrate` を設定すると、`start()` が `Start()` の前に `SetBaudRate()` で渡します。簡易シミュレーションは使いません。ネットワークシミュレーターは送信時間に使います。既定の 0 では `SetBaudRate()` を呼びません（SIL Kit の文書は、使う前に必ず設定するよう書いています。簡易シミュレーションと簡易 CAN Network Simulator では困りません。ほかのネットワークシミュレーターが、設定のないコントローラーをどう扱うかは確かめていません。そういうシミュレーターと使うときは、ビットレートを設定してください）。
- `SilKitCanPortOptions::transmit_queue_capacity`（既定 0 = 上限なし）を設定すると、送ったが送信確認の事象をまだ渡していないフレームがその数に達したとき、`send()` が `QueueFull` を返します。プロトコル処理はフレームを残して次のステップで出し直します。ネットワークシミュレーターは自分の送信キューを超えた要求を送らずに捨てる（`TransmitQueueFull` の送信確認）ので、シミュレーターを使うときはその大きさ以下にします。`event_capacity` より大きくはできません。送信確認の来ないフレーム（開始していないコントローラーでの送信など）は枠を返さないので、統計の `outstanding` で確かめます。SIL Kit の簡易シミュレーションでは送信確認が次のステップで渡るので、上限は 1 ステップに送れる数になります。
- SIL Kit のエラー状態の通知は `BusStateChanged` の事象にします（`NotAvailable` は出しません）。事象の受け箱に、送信確認をまだ受けていないフレームの数の空きを残せないときは、捨てて `event_overflows` に数えます（送信確認を失って `transmit_queue_capacity` の枠が減らないように）。受け箱のあふれは `ReceiveOverflow` です。
- 実時間（`SilKitCanPortOptions::real_time_clock` を設定）: 時刻同期なしの participant のポートになり、時刻の領域は `RealTime` です。SIL Kit のタイムスタンプ（同期なしの送信者では使えない）の代わりに、時計で到着時刻を付け、到着のたびにドライバーを起こします。userContext に送信時刻は入れません（ネットワークシミュレーターは仮想時間が前提のため）。`RealTimeDriver` で回します。
- 終了の順序: `ParticipantRuntime` を止める（実時間では `RealTimeDriver` を止める）→ ポートを `close()`（または破棄）→ プロトコル処理とホストを破棄 → `ParticipantRuntime` を破棄（コントローラーを所有しているため最後）。
- 同じシナリオを仮想バスと SIL Kit（仮想時間）で動かすと、受信と送信のトレースがバイト単位で一致します。ステップ幅の違う participant の組み合わせを含めて、テストで確かめています。前提として、同じ時刻の別の送信者のフレームについて、ポートが渡す順序は SIL Kit では決まりません（送信者ごとの順序だけが保たれます）。そのため、プロトコル処理がそれらを決まった規則で並べ直す必要があります。テストのプロトコルは（時刻、送信者）で並べ直しています。

## バスのタイミング（`CanBusModel`）

タイミングありの仮想バスと簡易 CAN Network Simulator は、同じバスモデルを使います。SIL Kit に依存しません。

- 送信時間: スタッフビットを含めないビット数（11 ビット ID は 47 + 8 × バイト数、29 ビット ID は 67 + 8 × バイト数。3 ビットのインターミッションを含む）÷ ビットレート。ns 単位に切り上げます。実際のフレームはスタッフビットのぶん（最大 2 割ほど）長くなります。
- 調停: バスが空いたときに、準備のできた送信キューの先頭どうしで、調停フィールドの小さいものが勝ちます。先に 11 ビットの基本 ID を比べ、同じなら 11 ビットのフレームが 29 ビットのフレームに勝ち（RTR が優性、SRR が劣性）、29 ビットどうしは拡張 ID を比べます。送信中のフレームには割り込みません。送信中に準備のできたフレームは、送信の終わりに争います。
- 同じ ID と形式のフレームどうし（実バスでは解決しない）: 短いデータ、データのバイトの順、送信者の順（仮想バスでは `attach()` の順）で決めます。
- 送信キューは送信者ごとの FIFO です。後ろのフレームは、調停で勝てる場合でも、前のフレームが出るまで待ちます。
- 模擬しないもの: スタッフビット、エラーフレーム、ACK エラーと自動再送、エラー状態、リモートフレーム、CAN FD、コントローラーの送信バッファの優先度。
- 決め方の 2 つの形（`VirtualCanBusTiming::simulator_step`）:
  - 0（厳密）: フレームは送った時刻からバスを争います。受信の時刻は送信時刻だけで決まり、受信側のステップ幅によりません。
  - Δ > 0: バスを Δ ごとに決めます。境界 T では、T より前に送られたフレームをキューに加え、[T, T + Δ) に始まる送信を決めます。時刻 t に送ったフレームは、t の後の最初の境界より前には始まりません。簡易 CAN Network Simulator（`SimulationStepSizeNanoseconds` = Δ）と同じ規則で、同じ送信には同じ受信の時刻を出します（テストで確かめています）。結果は Δ によって変わりますが、受信側のステップ幅にはよりません。
- タイミングの確認は、ノードを別々のポート（別々の CAN コントローラー）に置いて行います。同じ `Network` のノードどうしはローカル折り返しで届き、バスを通りません。

## 簡易 CAN Network Simulator（`vehicle_sil::can_netsim`、`vehicle_sil_can_bus_simulator`）

SIL Kit の簡易シミュレーション（trivial simulation）は、調停もバスの占有も再現しません。簡易 CAN Network Simulator は、SIL Kit の Network Simulator API（experimental）で、指定したネットワークの CAN を上のバスモデルで模擬する SIL Kit の participant です。CANoe のライセンスなしで、調停、送信時間、混雑を仮想時間で再現します。

- そのネットワークのコントローラーは、SIL Kit が自動で詳細シミュレーションに切り替えます。各 participant のコードは変わりません。
- 受信のフレームの時刻は送信の終わりです。送信側には、同じ時刻の TX のフレームと送信確認（`Transmitted`）を、送信側の userContext を付けて返します。コントローラーごとの送信キュー（既定 64）があふれた要求には `TransmitQueueFull` の送信確認を返し、送信しません。
- 時刻と決定論: 時刻同期に参加し、ステップ T で、T より前に送られた要求（vehicle-sil のポートが userContext に入れた送信時刻）を取り込み、[T, T + ステップ幅) に始まる送信を決めます。結果は送信時刻とステップ幅だけで決まり、他の participant のステップ幅やスレッドのタイミングによりません。目印のない要求（vehicle-sil 以外の participant）と、最後のステップより 1 秒以上先の時刻の要求は、最後のステップの時刻で扱い、数えます。この扱いは決定的ではありません。
- ビットレート: そのネットワークで最初に `SetBaudRate()` を呼んだコントローラーの値、なければ `--default-bitrate`（既定 500 kbit/s）です。後から違う値が来たら数えて警告し、使いません。vehicle-sil のポートは CommunicationReady で `SetBaudRate()` を呼ぶので、coordinated の participant（`vehicle_sil_launch run` の ECU、`Simulation`）の値は、起動の順によらず届きます。autonomous の participant と、コントローラーを作ってすぐ `SetBaudRate()` を呼ぶほかのプログラムの値は、シミュレーターを見つける前だと届きません（SIL Kit の挙動。docs/dependencies.md）。ポートにビットレートを設定しない構成と、そういう participant のために、ネットワークのビットレートは `--default-bitrate` でも指定してください。
- コントローラーのモードの変更（`Start()`、`Stop()` など）には、次のステップで状態の変化を返します。フレームは、モードによらず、送信者以外の接続中の全コントローラーに届けます（autonomous の participant の `Start()` は届かないことがあるので、モードで止めません）。CAN FD とリモートフレームの要求は捨てて数えます。
- 受け箱（ネットワークごとに 4096 要求）があふれたら、捨てて数え、警告します。あふれた実行は決定的ではありません。
- 設定にあっても、まだどのコントローラーも参加していないネットワークは何もしません（SIL Kit は最初のコントローラーが現れたときにフレームを配る手段を渡します）。Vector のものと共用する設定に、使わない CAN ネットワークがあってもかまいません。
- 使っているビットレートと、それが既定値かコントローラーの設定かを、ログに出します。
- 送信時刻の目印の上限は、最後のステップから 1 秒とステップ幅の大きいほうです。

設定ファイルとコマンドライン（Vector SIL Kit Network Simulator と共用）:

```yaml
SchemaVersion: 1
Description: 任意の説明
SimulationStepSizeNanoseconds: 1000000   # 省略時 1 ms
SimulatedNetworks:
  - Name: CAN1
    Type: CAN
  - Name: LIN1          # CAN 以外は模擬せず、警告する
    Type: LIN
```

- 形式は Vector SIL Kit Network Simulator の設定ファイル（`SchemaVersion: 1`、YAML か JSON）です。`Type: CAN` のネットワークだけを模擬します。ほかの種類のネットワーク、`Switches`、知らないキーは受け付けて警告します。必須の項目がない、`SchemaVersion` が 1 でない、種類が CAN、LIN、Ethernet、FlexRay のどれでもない、名前が重複している、といったファイルは使いません。
- コマンドラインは Vector のものと同じ形です: `vehicle_sil_can_bus_simulator [--name <participant>] [--connect-uri <uri>] [--log <level>] [--participant-configuration <file>] <configuration>`（短い形 `-n -u -l -c` も同じ。participant 名の既定は `NetworkSimulator`）。`-c` のファイルの中の相対パスは、実行時のカレントディレクトリから解決します。`-c` の構成では、`Experimental.TimeSynchronization.DynamicSimulationStep` を `false` にします（`true` か、キーなしでほかの participant が `true` だと、SIL Kit がステップを縮め、シミュレーターはそのステップで失敗します）。この実行ファイルだけの設定（`--default-bitrate`、`--transmit-queue`、`--start-timeout-ms`）はコマンドラインで渡し、設定ファイルには書きません。Vector のものが同じファイルを読めるようにするためです。
- 終了コード: 0 正常に終了、1 シミュレーション中の失敗、3 準備の失敗（設定ファイル、接続）、4 使い方の誤り。

Vector SIL Kit Network Simulator との差し替え:
- 実行ファイルを替えるだけで済むように作っています。どちらも同じ participant 名（既定 `NetworkSimulator`）でネットワーク名の模擬を引き受けるので、各ピアのコード、設定ファイル、システムコントローラーの必須 participant の一覧は変わらない想定です。Vector のものでの動作はまだ確かめていません。Vector のものは CANoe のライセンスが要ります。
- Vector のものは、スタッフビットを含む送信時間、相手のいない送信の ACK エラーとエラー状態、CAN FD、LIN、Ethernet、FlexRay を模擬します。簡易版はどれもしません。同じ設定でも時刻は一致しません。
- vehicle-sil のポートは、どちらのシミュレーターでも userContext に送信時刻を入れます。Vector のものにとっては意味のない値です。Vector のものでの決定論とタイミングは確かめていません。

既知の制限:
- 送信の開始は、送った時刻の後の最初のステップの境界より前になりません。その間に始まった優先度の低いフレームが先に出ることがあります（ずれは最大でステップ幅 + 1 フレーム）。
- 送信キューがあふれたときの扱いが、タイミングありの仮想バス（`send()` が同期で `QueueFull`）と簡易版（非同期の `TransmitQueueFull` の送信確認。フレームは送られない）で違います。あふれる場面では、両者のトレースは一致しません。J1939 などのフレームを失わないために、ポートの `transmit_queue_capacity` を `--transmit-queue` 以下にします（上の「SIL Kit のポート」）。
- 同じ時刻に別のコントローラーから同じ ID、データのフレームが出たときの順序は、各コントローラーが最初の要求を出した順（届いた順）で、決定的ではありません。受信側に届く内容は同じです。
- 送信を起点とするプロトコルのタイマー（J1939 の T3 など）は、ポートに渡したステップの時刻から数えます。バスの待ちのぶん、実際の送信より早く数え始めます（[j1939.md](j1939.md) の「トランスポートプロトコル」の制限）。
- Network Simulator API は SIL Kit の experimental な API です。使うのは `libs/can_netsim/src/can_bus_simulator.cpp` だけです。SIL Kit を更新したら、このシミュレーターのテストを回します。
