# ランチャー・プロセス・設定・実行

[English](../code-map-launcher.md)

この分野を変更するときの地図です。全体の入口は [code-map.md](code-map.md)、テストの共通ルールは [code-map-testing.md](code-map-testing.md) にあります。

関連する仕様と変更範囲: [simulation-config.md](simulation-config.md) / [simulation-check.md](simulation-check.md) / [simulation-run.md](simulation-run.md) / [simulation-events.md](simulation-events.md) / [シナリオ・時間の軸・一括実行](code-map-scenario.md) / [Control API・gRPC・ランチャーの Control Server](code-map-control.md) / [基盤・ステップ・信号・SIL Kit の既定](code-map-core.md) / [ECU・Peer・ローカル実行・Simulation](code-map-execution.md) / [Vector XL とイーサネットのブリッジ](code-map-bridges.md)

**このページ:** [テストのラベル](#テストのラベル) / [ディレクトリごとのコードとテスト](#ディレクトリごとのコードとテスト) / [変更するときに一緒に直すもの](#変更するときに一緒に直すもの)

**変更の種類:** [`check` と `run` の出力](#check-と-run-の出力) / [ガイドにある出力とコードのコピー](#ガイドにある出力とコードのコピー) / [照合のルールと `code`](#照合のルールと-code) / [要求の欄と要求ファイル](#要求の欄と要求ファイル) / [設定ファイルかシナリオのキー](#設定ファイルかシナリオのキー) / [設定ファイルの `network_names`](#設定ファイルの-network_names) / [設定ファイルの `j1939_nodes`](#設定ファイルの-j1939_nodes) / [イベント、標準入力のコマンド、`version --json`](#イベント標準入力のコマンドversion---json) / [`run` の時間](#run-の時間) / [自分で起動する participant](#自分で起動する-participant) / [実行の送受信（`traffic`）](#実行の送受信traffic) / [ランチャーの上限](#ランチャーの上限)

## テストのラベル

### `launch` / `process`

シナリオ、時間の軸、一括実行の変更時は [シナリオのページ](code-map-scenario.md#変更するときに一緒に直すもの) も参照。

| テスト | 内容 |
|---|---|
| `launch.*` | 設定ファイルの読み込みと照合のルール、人向けと JSON の出力（プロセスを起動しない）。この名前を 4 つの試験の実行ファイルが持つ: `tests/config`、`tests/scenario`、`tests/runner`（それぞれ自分のライブラリだけをリンクし、ディレクトリの名前を 2 つめのラベルに持つ: `-L config`、`-L scenario`、`-L runner`）と `tests/launch`（残り） |
| `launch_cli.*` | `vehicle_sil_launch check` を実行ファイルとして動かす |
| `launch_run.*` | `vehicle_sil_launch run` を実行ファイルとして動かす |
| `config.*` | `libs/config` の境界（ヘッダーとソースが何を include してよいか） |
| `runner.*` | `libs/runner` の境界（公開ヘッダーが SIL Kit に届かないこと。`libs/launch` を include しないこと） |
| `scenario.*` | `libs/scenario` の境界（ヘッダーとソースが何を include してよいか） |
| `process.*` | 子プロセスのテスト |

`launch.*` は、各ルールの通る例と失敗する例を、文言と行番号まで照合します。

`launch_cli.*` が確認すること:

- [docs/simulation-config.md](simulation-config.md) の例とバイト単位で照合
- `--describe` に失敗する実行ファイル、`--strict`、読めないファイル、引数のエラー
- `launch_cli.connect`: 2 つのブリッジ（`main()` を偽の pcap ライブラリと偽の XL ドライバーで作ったもの）を、本当の名前でビルドディレクトリの `connect_tools/` に写して `check --connect`。`PATH` と、写した `vehicle_sil_launch` の隣で見つかり、作業ディレクトリでは見つからないこと、`networks[].outside`、引数を受けないブリッジ、`run --disable bridge.<network>` と `test --connect` を断ること
- `launch_cli.request`: 要求ファイルを付けた `check --request` と `run --request`。引数と同じ結果になる、引数が勝つ、ファイルのシナリオは設定ファイルの隣から読む、指定していないファイルは読まない、ファイルの間違いは使い方の誤りになる。
- `launch_cli.connections`: 同じブリッジで `connections`。`--connect` の行と、それを `check` が受けること、`--json`（ブリッジの一覧をそのまま渡す。LIN は一覧なし）、`--network`、一覧を作れないブリッジ（`library-missing`、`driver-missing`）とブリッジがないとき（`tool-missing`。終了コード 1）、コマンドライン（終了コード 4）と読めないファイル（終了コード 3）。ほかは `launch.connections_*`（`test_connections.cpp`）がプロセスなしで確かめます: 引用、一覧の読み、ランチャーが作る一覧、各ブリッジに 1 回だけ聞くこと、文と JSON の全体
- `launch_cli.descriptions`: `check --write-descriptions` と `check --descriptions`。保存した説明があれば何も起動しないこと（起動のたびに `VEHICLE_SIL_HELPER_MARK` に印を残すヘルパーの写しを、participant、`role: bridge`、`--connect` と要求ファイルの `connect` のブリッジに置く）、保存した照合と同じ報告、コマンドや実行ファイルが変わったとき、ない・壊れた・別の版のファイル、書けないファイル。`check --text-from-stdin`（同じ文のファイルと同じ、CRLF、大きすぎる、NUL）。`run` と `test` が 3 つを断ること
- `launch_cli.compare`: `check --compare-commands`。JSON の `commands`（`same`、`changed`、読めないファイルの `not_compared`、選択肢がないときの `null`）と注記、`--text-from-stdin` と `--descriptions` と一緒（何も起動しない）、人向けの出力。`run` と `test` は受けない。比べるものと比べないものは、プロセスなしで `launch.commands_*`（`tests/config/test_compare_commands.cpp`）が見る

`launch_run.*` が確認すること:

- docs の例との照合
- 失敗する ECU、そろわない participant、終わらない participant、残されたプロセス、中断、ランチャーが異常終了したとき
- 速度と一時停止とステップ実行、実時間への遅れ
- `launch_run.ethernet`: `examples/ethernet` の 2 つの ECU の照合とシナリオ
- `launch_run.test_command`、`launch_run.test_interrupt`: `vehicle_sil_launch test`
- `launch_run.live`: 止めるまで動く例の設定ファイル `simulation.live.yaml` の照合と、1000 ms を超えて動かしてから止める実行
- `launch_run.connect_*`（`launch_run_connect.cpp`）: `connect_tools/` のブリッジでの `run --connect`（偽の pcap ライブラリの装置 `fake0` が答え、偽の XL ドライバーが `virtual:1` で返す）。ブリッジがどの ECU の起動より前に参加すること、両向きのフレーム、起動できないブリッジ（`connect-failed`。時間の上限でなく、プロセスで気づく）、手で起動する participant を待つ間に終わるブリッジ、実行中に強制終了または終了コード 0 で終わるブリッジ（`connect-lost`）、`--strict` と `--start-paused` での速さの警告、実時間を保つ実行の時間のコマンド（`connect_time_locked`: 構成を読む前に書いたものも含めて、届いた順に `outside-real-time` で断り、state も同じことを言う）、`--time-policy free` での `outside-time-changed`（`connect_time_changed`: 実行の最初の一時停止についての注記 1 つ。`--strict` でも通る）、数（`connect_roundtrip`: 結果、`outside` イベント、ログの中のブリッジの最後の統計の行が一致し、ブリッジの統計の行は最後の 1 行だけが 1 回 `log` イベントになる。`connect_nothing_received`: `fake1` と返す相手のないチャンネルで、両方のネットワークに `connect-nothing-received`。`--strict` でも通る注意。`--json` でも）。どれもイベントか行を待ち（眠らない）、起きてはいけないことは終わった後の出力の全体で確かめる

`process.*` が確認すること:

- 標準出力と標準エラーの取り込み、行ごとの受け渡し、時間切れ
- 空の標準入力、作業ディレクトリ、受け継がないハンドル
- 停止の要求、起動したプロセスごとの終了
- 空白と ASCII 以外の文字を含むパスと引数

## ディレクトリごとのコードとテスト

### `libs/process`

| 名前 | 場所 |
|---|---|
| `ChildProcess`、`run_captured()`、`current_executable()`（このプロセスの実行ファイルのパス） | `process/child_process.hpp` |
| `Process` | `process/process.hpp` |

- テスト: `tests/process`
- `ctest -R`: `^process\.`

### `libs/config`

構成ファイルとその照合（純粋: プロセスも SIL Kit も使いません。`launch.*` が試験し、`config.*` が境界を守ります）。`config/detail/` のヘッダーは、ランチャーの部品と試験が読むためのものです。

公開ヘッダー:

| 名前 | 場所 |
|---|---|
| `read_config()` | `config/config.hpp` |
| 構成の照合（`CheckReport`、ルール） | `config/check.hpp` |
| `--connect` の `parse_connect()` と先の文法（`read_connect_target()`、`backend_problem()`、`target_problem()`）、backend ごとの `bridge_tool()`、`bridge_arguments()`、backend を除いたデバイスの `connect_device_key()`、`no_bridge_here()`（LIN、Windows 以外の Vector XL のブリッジ）、照合と `connections` と `run` がブリッジを起動するディレクトリ（ブリッジ自身のもの）の `bridge_working_directory()` | `config/connect.hpp` |
| 時間の固定と、run が始める速さ（`TimeLock`、`time_lock()`、`planned_speed()`、`time_lock_findings()`） | `config/time_plan.hpp` |

実装:

| 内容 | 場所 |
|---|---|
| `CheckReport` の数、`sort_findings()`、`connect_for_config()` | `src/check_report.cpp` |
| `--describe` の JSON を読む | `src/describe_read.cpp` |
| シミュレーションから見た ECU（`SeenEcu`）: 実行ファイルの自己申告に、participant の項目を当てたもの。照合、`run`、シナリオは、自己申告（名前、ステップ、ネットワーク、変数、制御の口、features）をこれからだけ読む | `config/detail/seen_ecu.hpp`、`src/seen_ecu.cpp`（`seen_variables()` も） |
| 自己申告のネットワークを、設定ファイルの `network_names` を当てた名前で、J1939 のノードを `j1939_nodes` を当てた値で渡す（`seen_networks()`） | `src/check_networks.cpp` |
| ファイルと説明を照らすルール（ネットワークは種類ごとの関数）。ファイルだけで分かる誤り（`duplicate-name`、キー、値の形）は、読むときに `src/config.cpp` で見つける | `src/check_rules.cpp` |
| 2 つの構成が同じコマンドを実行するか（`compare_commands()`、`check --compare-commands`）。機種を共通の部分に重ねるのは `config.cpp` の `detail::apply_variant()` | `src/compare_commands.cpp` |
| `--connect` の照合（`check_connections()`: 要求、見つけたブリッジとその `--describe`）。シミュレーション全体が要るルール（ブリッジをネットワークの participant に数える、`connect-loop`）は `check_rules.cpp` | `src/check_connect.cpp` |
| `--connect` の形とブリッジの引数。`find_tool()`（ランチャーの隣、次に `PATH`）と、それが `find_executable()` と共有する `PATH` の探索（`detail::find_on_path()`）は `libs/launch/src/probe/probe.cpp` | `src/connect.cpp` |
| 設定ファイルとシナリオの読み取りの共通の部分 | `config/detail/yaml_reader.hpp` |
| 時間の固定と計画した速さ、`detail::plan_time_step()`（コントローラーの既定のステップ）、`detail::coarse_time_step()`、`RunTiming::actual_speed()` | `src/time_plan.cpp` |

- 試験: `tests/config`（ルールと読み取り。ランチャーのライブラリ、core、SIL Kit なしでリンクする。境界）、`tests/launch`（照合の出力と、participant の実行ファイルの確認。この計算機のファイルを見る）
- `ctest -R`: `^launch\.`（ルール。`tests/config` の分は `-L config`）、`^config\.`（境界）

### `libs/scenario`

シナリオ（純粋: プロセスも SIL Kit も使いません。`launch.*` が試験し、`scenario.*` が境界を守ります）。`scenario/detail/` のヘッダーは、ライブラリの部品、`probe.cpp`（`scenario_csv.hpp`）、試験が読むためのものです。

公開ヘッダー:

| 名前 | 場所 |
|---|---|
| `ScenarioCheck`（`check --scenario` が読んだシナリオ）と `scenario_catalog()`（照合した構成の participant を、シナリオが使える形にしたもの） | `scenario/scenario_check.hpp` |
| シナリオの `read_scenario()`、時間の関数のインターフェース `Waveform` | `scenario/scenario.hpp` |
| `ScenarioEngine` | `scenario/scenario_run.hpp` |
| `run --timeline` の記録 `TimelineRecord` | `scenario/timeline.hpp` |

読み手、エンジン、タイムラインのファイルは [code-map-scenario.md](code-map-scenario.md) に並べています。`scenario_catalog()` は `src/scenario_check.cpp` です。

- 試験: `tests/scenario`（読み手とエンジン。ランチャーのライブラリ、core、SIL Kit なしでリンクする。境界）、`tests/launch`（ランチャーが書く timeline ファイル、`test_scenario_timeline.cpp`）
- `ctest -R`: `^launch\.`（読み手とエンジン。`tests/scenario` の分は `-L scenario`）、`^scenario\.`（境界）

### `libs/runner`

シミュレーションの実行（レジストリ、コントローラー、子プロセス、Hub、シナリオのステップ、時刻と停止のコマンドとその規則、実行が見たこと。内部用。`launch.*` が試験し、`runner.*` が境界を守ります）。SIL Kit とプロセスを使い、要求ファイル、出力の形式、コマンドラインは知りません。`libs/launch` がこれをリンクし、逆はありません。公開ヘッダーは SIL Kit のヘッダーに届かず、実行の部品は `RunnerImpl` の後ろにあります。`runner/detail/` のヘッダーは、ランチャー自身の部品（出力が `run_words.hpp` を読みます）と試験が読むためのものです。

公開ヘッダー:

| 名前 | 場所 |
|---|---|
| 実行が見たこと。部分に分けた値: `RunOutcome` の `setup`、`start`、`end`（シミュレーションがどう、なぜ終わったか）、`control`（制御の agent が捨てたもの）、`scenario`（シナリオとタイムラインのファイル）、`close`（ランチャーの接続の閉じ方）と、`timing`、`participants`、`outside`、`time_changed`、`time_lock`、`real_time_peers`、`traffic`。`assess_run()` が判定し、`end_reason()` が終わった理由を言う | `runner/run_outcome.hpp` |
| 実行の前に決まることを値にしたもの: `RunPlan`（通った check に `--connect` のブリッジを末尾に足したもの、`members`: participant ごとの `RunMember`。実行が `--connect` のために足したブリッジかどうかと、つなぐ先を持つ、ファイルのディレクトリ、コントローラーの刻みと開始時の速さ、最初の刻みの前で保留するか、期限、ログとタイムラインの書き先）、`plan_run()`（純粋: check、そのシナリオ、`RunChoices`、`RunLimits` から。後の 2 つも同じヘッダー。要求は知らない）、`planned_outcome()`（実行が最初に持つ `RunOutcome`）。`run_file()` は、要求（`RunRequest`）、観察者（`runner/run_events.hpp` の `RunObservers`: `write_line`、`interrupt_name`、`before_start`）、実行が借りるもの（`RunLent`: 操作盤か停止の要求、Control Server、イベントの受け口（`RunEventSink`））、試験だけが短くする待ち時間（`RunLimits`）を受けます。計画が読む要求の欄を `RunChoices` に写し（`launch/run.hpp` の `run_choices()`）、それと待ち時間からこの計画を作り、要求は持ち続けないので、実行のどのスレッドも要求を読みません。実行の部品は、計画、観察者、操作盤を `detail::RunContext` から読みます。プロセスなしの試験は `tests/runner/test_run_plan.cpp` | `runner/run_plan.hpp`、`src/run_plan.cpp` |
| `StopRequests`。`run_file()` や `test_file()` を呼ぶ側が持って貸す、停止の要求（run が読む回数と、最初の要求の出どころ）、`StopKind`（operator、signal、supervisor）、ライブラリが知る 4 つの出どころ（`stop_source::kStopCommand` など）とその名前、`stop_kind()`。シグナルとコンソールのイベントの名前は CLI に残る | `runner/stop_requests.hpp` |
| `RunControls`。1 回の run の操作盤で、run が呼ぶ側から借りるか、自分で作ります: 停止の要求（借りたもの）、`TimeCommands`、覚えて観察者に知らせる段階（`Phase::Ended` は先に時間のコマンドを終えます）。`Phase`、`phase_name()`。`run_file()` は、別の要求のために作った操作盤を断ります: `start_paused()`、`scenario()`、`control_server()`（`set_control_server()`）が要求と合わなければなりません。`RunState`（イベント `state` が書く run の状態）。`RunControls::publish()` が、その 3 つから組み立て、変わったときに観察者に知らせます | `runner/run_controls.hpp` |
| `TimeCommands` | `runner/time_commands.hpp` |
| 実行の部品が共有し、試験も読む道具: participant の引数（`detail::participant_arguments()`）、待ち、この PC のアドレス、`--connect` のブリッジの数とリンクの行、participant の行とログの名前 | `runner/detail/run_detail.hpp` |
| 操作盤の規則: `decide()`（run がいまその操作を受けるか。入口と run の状態で決まります。行の表は `src/control_rules.cpp`）、`allowed()`（引数なしの同じ答えと、引数の範囲）、`always_refused()`（ある操作が run の間ずっと断られるか。シナリオの実行中の Control Server の書き込み）、理由のコード `reason::*`。純粋 | `runner/control_rules.hpp` |
| `RunTraffic`（結果の `traffic`） | `runner/traffic.hpp` |
| `RunEvents`。run の部品が、実行中の出来事（レジストリ、参加者、行、所見、仮想時刻）を知らせるインターフェースと、渡す値（`ParticipantState`、`TimeEvent`）。実行が呼び返す呼ぶ側の関数（`RunObservers`）と、Control Server の待ち受ける所（`ControlServerInfo`）。run の部品は、このヘッダーを include し、`events.hpp` を include しません。イベントの書き方を知りません | `runner/run_events.hpp` |
| `Command`（読んだコマンドと、`TimeCommands` が決めた結果） | `runner/command.hpp` |

実装:

| 内容 | 場所 |
|---|---|
| `run --connect`: ブリッジを、ファイルの participant の後に、その実行だけの participant として足す（`detail::with_bridges()`）。そのブリッジの印は、構成の欄ではなく、計画の `RunMember::outside`（`RunPlan::members`。`plan_run()` が作る）。ファイルに role を `bridge` と書いた participant には付かない。先に起こして待つ所（`RunStart::wait_for_bridges()`）と、終わったら実行を止める所（`look_at_children()`、`RunWatch::watch()` の `detail::stops_run()`。`gone_before_start()` は計画の印を読む）は `run.cpp`、`run_participants.cpp`、`run_start.cpp`、`run_watch.cpp`。指摘 `connect-failed` と `connect-lost` は `run_assess.cpp`。`outside-time-changed` は、時間を変えた最初の命令を `TimeCommands::apply()` が鍵の下で覚え（命令のスレッド）、`RunWatch::settle()` が `detach()` の後に読み（主スレッド）、`run_assess.cpp`（`time_changed_findings()`）で判断する。最後の行は `RunOutput::on_line()`（読み手のスレッドが出力の鍵の下で書く） | `src/run_connect.cpp` |
| `run --connect`: ブリッジの数。`detail::read_outside_counts()` がブリッジの統計の行を読む（キーは `ethernet_bridge::format_statistics()` と `can_bridge::format_statistics()` のもの。`launch.read_outside_counts_reads_the_lines_the_bridges_print` が両側を守る）。`detail::OutsideNote` が `outside` イベントを出す時を決める（時刻は引数）。`RunOutput::on_line()`（読み手のスレッド）が、出力の鍵の下で行を読み、数を覚え、イベントを書き、行は出さない（最後に読んだ 1 行だけを `take_counts()` が 1 回出す）。主スレッドは `RunWatch::watch()` の見回りごとに控えの数を書き（`outside_due()`）、`RunParticipants::note_last_lines()` が出力を読み終えた後に最後の数を結果に取る（`take_counts()`。結果のイベントより前）。`detail::OutsideState` が、ブリッジのリンクの行（`read_outside_link_line()`）と統計の行の数のすべて（`read_outside_stats()`）から、リンクやバスが落ちていた区間と error passive の時間を足し上げる（時刻は引数）。`RunOutput` が同じ鍵の下でブリッジごとに 1 つ持ち、リンクの変化を言い、`outside_lost()` が、リンクやバスが `RunLimits::outside_lost_after` 落ちたことを `RunWatch` に知らせる（ブリッジが終わったときと同じに実行を止める。`connect-lost`）。`take_link()` が合計と数を `RunOutside::link` に入れる。リンクの行の共有のキー（`device`、`marked`、`shared`、`peers`）も同じ `OutsideState` にあり、`detail::foreign_sharing()`（純粋）が、ブリッジの `shared` から自分の実行のブリッジを引く（`RunParticipants::own_bridges()` がそのプロセス ID と生きているかを渡す）。`RunOutput::outside_shared()` が見つけたものを鍵の下で持ち、`--strict` では止めるブリッジを挙げる。`RunStart::wait_for_bridges()` が何かを起こす前に 1 度、`RunWatch` が見るたびに（`kOutsideSharedAfter`）これを呼ぶ。所見 `outside-device-shared` は `run_assess.cpp` で作る。フレームを失った数のキーの表は 1 つだけ（`run_detail.hpp` の `kOutsideDroppedKeys`）。注意 `connect-nothing-received` とバスの警告 `outside-*` は `run_assess.cpp` | `src/run_outside.cpp` |
| 準備から participant の終わりまでの、1 回の実行の順（`Runner`）。`run_file()` は `runner/detail/runner.hpp` から見る。このヘッダーは SIL Kit のヘッダーにも、実行の私的なヘッダーにも届かない。実行の部品はポインターの先（`src/runner_impl.hpp` の `RunnerImpl`） | `runner/detail/runner.hpp`、`src/runner_impl.hpp`、`src/run.cpp` |
| `run` を分担するクラス | `src/run_*.cpp`、`src/time_commands.cpp` |
| 1 回の実行の Hub と、registry、コントローラー、Hub の transport を 1 つにまとめたもの（`detail::ConnectionOf`）: 閉じる順と、持ち主。`Runner::run()` が、`RunControl` の作った Hub の上に作り、1 人で持つ。終わりに、閉じるスレッドが引き取り（`detail::close_connection_within()`。`runner/detail/run_close.hpp` の `close_within()` の上）、期限を過ぎて放置されたら持ち続ける。放置されたスレッドは実行のほかのものに触らないので、`run_file()` は `Runner` をほかの実行の後と同じに破棄する。放置された閉じるスレッドが触りうるものの一覧は `src/run.cpp` の頭のコメント | `src/run_connection.hpp` |
| この PC のアドレス（ほかの PC に示す URI） | `src/host_addresses.cpp` |
| 動いているシミュレーションの見張り（`RunWatch::watch()`）、終わったときに確定すること（`RunWatch::settle()`: シナリオの終わり、時間、traffic、シミュレーションの終わり方）、仮想時間が進まないことを報告する `StandStill` | `src/run_watch.cpp` |

`run` を分担するクラスの役割と、どのスレッドがどの状態をどのロックで扱うかは、`run.cpp` の先頭のコメントと、各 `run_*.hpp` のクラスのコメントにあります。

- 試験: `tests/runner`（ランチャーのライブラリなしで試せる実行の部品: 判定、終了の順、トラフィック、操作の規則、計画。境界）、`tests/launch`（イベントの流れと報告を伴う実行。`test_run.cpp`、`test_run_controls.cpp`、`test_events.cpp`）
- `ctest -R`: `^launch\.`、`^launch_run\.`（部品と実行。`tests/runner` の分は `-L runner`）、`^runner\.`（境界）

### `libs/launch`、`tools/launch`

`libs/launch/src` は 5 つの部分に分かれ、ディレクトリが 1 つずつあります。`usecases/`（`check.cpp`、`connections.cpp`、`execution_runtime.cpp`、`run_file.cpp`、`serve.cpp`、`test.cpp`）、`request/`（`request.cpp`、`request_read.cpp`）、`probe/`（`probe.cpp`）、`format/`（出力と `exit_codes.hpp`）、`endpoints/`（`command_input.cpp`、`control_server*.cpp`、`run_control_server.hpp`）です。下の公開ヘッダーは `include/vehicle_sil/launch/` に平らなまま置きます。どの部分が何を include してよいかは [architecture.md](architecture.md#libslaunch-の層) の「`libs/launch` の層」にあります。新しいファイルは、その仕事をする部分に置きます。

公開ヘッダー:

| 名前 | 場所 |
|---|---|
| `check_file()`、`Checked`（`--file-only` 時の `file_sha256` と `hot_eligibility` を含む）、`HotEligibility`、`evaluate_hot_eligibility()`、`--scenario`（`format_text()` と `format_json()`） | `launch/checked.hpp` |
| ランチャーが、動いているコンピューターに尋ねること: `find_executable()` と `executable_status()`（participant の実行ファイル）、`find_tool()` と `bridge_tools()`（ブリッジ）、`run_tool()`（`--describe` やブリッジの一覧の 1 回の実行）、`read_config_file()`、`read_config_stdin()`（`check --text-from-stdin`）、`read_scenario_file()`、`files_beside()`、`environment_value()`。`check_file()` が participant の `--describe` の出力を取るところ（`DescribeSource`: `describe_by_running()`、またはプロセスを起動しない `describe_from_saved()`）と、保存した説明のファイル（`read_saved_descriptions()`、`write_saved_descriptions()`、`executable_stamp()`） | `launch/probe.hpp` |
| `vehicle_sil_launch connections` の `list_connections()`、`connections_of()`（backend ごとに各ブリッジに 1 回だけ聞く。`prefix_targets()` が `--connect` の受ける値にする）、`interpret_list()`（ブリッジの一覧か、ランチャーが作る一覧: `tool-*`）、`check_requested()`（`--request`: 要求ファイルの先が一覧にあるか。backend と名前で比べる。ファイルを読み構成に合わせて切るのは、要求ファイルの行の関数）、`quoted_for_shells()`、出力 | `launch/connections.hpp` |
| `run_file()`、その結果（`RunReport`。書くのは `format_run_text()` と `format_run_json()`）、実行が借りるもの（`RunLent`）、実行から見た Control Server（`ControlEndpoint`） | `launch/run.hpp` |
| `ExecutionRuntime`、`ExecutionSink`、`ExecutionStatus`、`LauncherState`、`ConfigMetadata`、`LauncherEnd`、`ExecutionEnd`: Prepare、Run、Stop、時刻命令、Status、Results、Shutdown を管理するトランスポート非依存の常駐実行ランタイム | `launch/execution_runtime.hpp` |
| `Serve`（`ExecutionRuntime` 上で `vehicle_sil_launch serve` を実装する薄い NDJSON アダプター）と `prepare_serve_run()` | `launch/serve.hpp` |
| check、run、test が何を頼まれたかを、値にしたもの: `RunRequest`。2 つの部分に分かれる。`RunIntent`（利用者のもの: variant、パラメーター、利用者が書いたとおりのシナリオ、`--disable`、`--enable`、`--manual`、`--connect`、`--time-policy`、速さ、`--start-paused`、`--strict`）と `RunHost`（呼ぶ側のもの: 利用者が書いたとおりのファイル、道具とログの場所、`--describe` と実行の期限、利用者が書いたとおりのタイムラインのファイル、求められた Control Server（アドレスとファイルの名前）、標準入力の命令を求めているか）。`validate_request()`（純粋）が、一緒には使えない欄の規則（`--param` と `--timeline` にはシナリオが要る、`--start-paused` をシナリオと使う、続ける手段なしに使う）を、code が `usage` の指摘として持つ。`check_file()`、`run_file()`、`test_file()`（`TestOptions::request`）がこれを受け、名前からパスを作る。`CheckOptions` に残るのは、頼まれたことではないものだけ（`file_only`、`params_for_many`、`fastest_by_default`）。`tools/launch/main.cpp` の `read_request()` と `run()` が、誤りを報告する順に引数を読み、3 か所で `validate_request()` を呼んで最初の指摘を断る（`--request` があるときは、全部の引数を読んだ後に 1 度）。プロセスなしの試験は `tests/launch/test_request.cpp`（規則と、check と計画が欄ごとに読むもの） | `launch/request.hpp`、`src/request/request.cpp` |
| `--request` の要求ファイル（意図を JSON にしたもの）: `read_request_file()` と `read_request_intent()` が文を読む（純粋。この形式の読み手は 1 つで、`YamlReader` から作ってあるので、知らないキーと 2 回書いたキーの文がほかのファイルと同じになる）。`with_request_file()` が欄ごとに引数を重ね、`connect` は `merge_connect()` が重ねる。`RunRequest::origin`（`config/config.hpp` の `RequestOrigin`: ファイルの名前と、値がファイルから来たキーとネットワーク）を見て、`validate_request()` と `time_lock_findings()` の文がファイル名を書く。`connect_for_config()`（`config/check.hpp`。実体は `check_report.cpp`）が、設定ファイルにないファイルのネットワークを外す（`connect-network-unused`）。`check_file()` は、ファイルのシナリオを設定ファイルのディレクトリから読む。ファイルを読むのは `probe.cpp`（`read_request_file_text()`）で、`tools/launch/main.cpp` は名前を渡すだけ（`read_request_argument()`）。プロセスなしの試験は `tests/launch/test_request_file.cpp`、ランチャーを使う試験は `launch_cli.request`、`launch_cli.connect`、`launch_run.connect_request` | `launch/request.hpp`、`src/request/request_read.cpp`、`src/request/request.cpp` |
| `RunEventSink`（`RunEvents` を `phase()`、`check()`、`command()`、`state()` で広げた実行ごとのイベントのインターフェース。`run_file()` と `RunLent::events` がこれを使います） | `launch/run_sink.hpp` |
| `EventStream`（`run --events` のイベントを書き、`RunEventSink` を実装します） | `launch/events.hpp` |
| `parse_command()` と `CommandInput`（`--stdin-commands` のコマンドを読む） | `launch/command_input.hpp` |
| 記録とシナリオの結果から `run --timeline` のファイルを作る `format_timeline_json()` | `launch/timeline_format.hpp` |

実装:

| 内容 | 場所 |
|---|---|
| 照合の全体の流れ（`check_file()`）、`evaluate_hot_eligibility()`、純粋 C++20 FIPS 180-4 SHA-256（`detail::sha256_hex()`） | `src/usecases/check.cpp`、`src/usecases/sha256.{hpp,cpp}` |
| コンピューターを調べる: ファイルシステム（実行ファイルやブリッジの場所、設定ファイル、シナリオ、点のファイル）、環境変数（`PATH`、`VEHICLE_SIL_LISTEN_ON_NETWORK`）、出力を読むために動かすプロセス。ルールは調べた結果を値（`config.cpp` の `executable_finding()` はファイルの状態を受け取る）か関数（`ConnectTools`、`ScenarioContext::read_file`）で受け取る。例外が 1 つ残る: `connect.cpp` の `bridge_working_directory()` は相対のディレクトリを絶対にする | `src/probe/probe.cpp` |
| 保存した説明のファイル（`check --write-descriptions`、`--descriptions`。ランチャー独自の JSON で、同じ版だけが読む）と、それから取る `DescribeSource` | `src/probe/saved_descriptions.cpp` |
| check の JSON に、実行ファイルが申告したとおりのネットワークを書く（`write_reported_networks()`） | `src/format/reported_networks.cpp` |
| `connections`: ブリッジの一覧を読み（`TargetReader`）、キーをすべてそのまま渡す（`write_node()`）こと、ランチャーが作る一覧、文と JSON。ブリッジは、`--describe` と同じく `src/probe/probe.cpp` の `run_tool()`（`bridge_tools()`）で起こします | `src/usecases/connections.cpp` |
| 照合の出力 | `src/format/check_format.cpp` |
| 間違ったコマンドラインや内部の誤りの結果（JSON、結果のイベント） | `src/format/error_result.cpp` |
| `run_file()`: 照合、実行が自分で作る操作盤と Control Server、`Runner` の 2 つの段、タイムラインのファイル | `src/usecases/run_file.cpp` |
| `ExecutionRuntime`: 2 軸のランチャー・実行状態機械、`prepare()`、`start_run()`、`stop_run()`、`send_time_command()`、`status()`、`result()`、`request_shutdown()`、`run_loop()` | `src/usecases/execution_runtime.cpp` |
| `Serve`: `ServeCommand` と `EventStream` を `ExecutionRuntime` に接続する NDJSON アダプター | `src/usecases/serve.cpp` |
| `--stdin-commands` と `serve` のコマンドを読む（`parse_command()`、`parse_serve_command()`、`prepare`、`status`、`stop.force`、インライン `intent`） | `src/endpoints/command_input.cpp` |
| コマンドラインと、標準入力・標準出力のスレッド | `tools/launch/main.cpp` |
| `run --control` の Control Server（`VEHICLE_SIL_BUILD_CONTROL_SERVER=ON` のときだけ）。実行からは `ControlEndpoint` として見える（`launch/run.hpp`）。`run_file()` が、照合の前に要求（`RunHost::control`）から作り、実行の 2 つの段（`src/run.cpp` の `Runner::prepare()` と `Runner::run()`）の間で起動し、戻るときに終える。破棄は、呼ぶ側が結果の後に行う（`RunReport::ended_control_server`） | `libs/launch/src/endpoints/control_server.cpp`（サーバーと、要求がそのために指すものの読み取りと検査）、`libs/launch/src/endpoints/run_control_server.hpp`（`run_file()` が呼ぶもの） |

- テスト: `tests/launch`、`tests/multi_host`
- `ctest -R`: `^launch\.`（プロセスなし）、`^launch_cli\.`（`check`）、`^launch_run\.`（`run`）、`^multi_host\.`

`tests/multi_host` は、Linux のネットワーク名前空間で 2 つの PC を作ります。作れない所では、理由を表示してスキップします。`VEHICLE_SIL_REQUIRE_MULTI_HOST=1` なら、スキップせずに失敗にします。

### `tools/common`

実行ファイル共通のコマンドライン処理です（ターゲット `vehicle_sil_cli`）。

| 内容 | 場所 |
|---|---|
| 引数の処理 | `vehicle_sil/cli/args.hpp` |
| 標準出力と標準エラーに UTF-8 の文を書く `write_text()`（Windows のコンソールには UTF-16 で） | `vehicle_sil/cli/console.hpp` |
| 引数とパスを UTF-8 で扱う | `vehicle_sil/cli/utf8.hpp` |

- テスト: `tests/unit`
- `ctest -R`: `^unit\.`

### `examples/integrate_ecus`（ガイド「複数の ECU をつないで動かす」の例）

| ファイル | 内容 |
|---|---|
| `dashboard_v1/main.cpp` | `examples/ecu_main` の表示の ECU と同じコードで、ネットワーク名だけが `VehicleCAN` |
| `simulation.yaml` | エンジンとこの ECU（`check` が `unknown-network` を報告する） |
| `simulation.two-buses.yaml` | `networks` に `VehicleCAN` を追加したもの |
| `simulation.fixed.yaml` | 表示の ECU を、`examples/ecu_main` のものに差し替えたもの |
| `simulation.network-names.yaml` | `simulation.yaml` の表示の ECU に `network_names` を足したもの |
| `simulation.second-engine.yaml` | `simulation.fixed.yaml` に、`j1939_nodes` で変えた 2 つ目のエンジンの ECU を足したもの |
| `scenarios/engine_speed.yaml` | シナリオ |

- ビルド: `VEHICLE_SIL_BUILD_EXAMPLES` で、`build/<preset>/examples/integrate_ecus/` に実行ファイルと YAML を置きます。エンジンと、作り直した表示の ECU は、隣の `examples/ecu_main/` のものを相対パスで使います。
- `ctest -R`: `^launch_docs\.`

`launch_docs.integrate_ecus`（`tests/launch/integrate_ecus.cmake`）が照合するもの:

- ファイルとガイドのブロック
- `--describe` と `check --json` の抜粋のキー
- 4 つの設定ファイルの `check`、`--disable Dashboard`
- `run` の結果と、表示の ECU の出力
- シナリオつきの `run`

### `examples/ecu_main` の `simulation.two-pcs.yaml`（ガイド「複数の PC で動かす」の例）

- テスト: `multi_host.docs` が、このファイルとその `check` の出力を、ガイド [howto/multi-pc.md](howto/multi-pc.md) のブロックと照合します。
- `ctest -R`: `^multi_host\.docs`

### `schema/`（設定ファイルとシナリオの JSON Schema）

| ファイル | 内容 |
|---|---|
| `schema/simulation.schema.json` | 設定ファイル |
| `schema/scenario.schema.json` | シナリオ |
| `schema/run-request.schema.json` | `check --request` と `run --request` の要求ファイル（読むコードは `libs/launch/src/request/request_read.cpp`） |
| `schema/timeline.schema.json` | `run --timeline` が書くファイル。ランチャーの出力の形で、読む道具のためのもの。ツールのアーカイブの `schema/` に入れ、テンプレートの `template/schema/` には置かない |
| `gui/protocol/dashboard.schema.json` | ダッシュボード |

設定ファイルとシナリオのスキーマは、エディターの補完と検査のためのもので、ランチャーは読みません。ランチャーが読むコードは `libs/config/src/config.cpp` と、シナリオの読み手（`libs/scenario/src/scenario_reader.hpp` と、そこに挙げたファイル）です。

| コピーの場所 | 内容 |
|---|---|
| `template/schema/` | 設定ファイル、シナリオ、要求ファイル、ダッシュボードのスキーマのコピー |
| ツールのアーカイブの `schema/` | 同じスキーマと `timeline.schema.json`（`template/get_tools.py` が、全部をプロジェクトの `schema/` にコピーする） |
| ビルドのディレクトリ | 例をビルドするとき、configure のときにコピーする（最上位の `CMakeLists.txt`） |

ビルドのディレクトリにコピーされた例の YAML の 1 行目は、そこにコピーされたスキーマを指します。

- テスト: `tests/schema/test_schemas.py`
- `ctest -R`: `^schema\.`（ランチャーなしの分は、`scripts/verify-python.py` の `schema` でも実行されます）

`test_schemas.py` が確認すること:

- 例、テンプレート、ドキュメントの YAML がスキーマに通る
- 通る見本と失敗する見本を、スキーマと `check` の両方に掛ける
- 対応付けごとのキー、ネットワークの種類、手順の種類が、ランチャーのエラーの文が並べるものと同じである
- キーがドキュメントの表にある
- テンプレートのコピーが同じである
- 例とテンプレートの YAML の 1 行目がスキーマを指す
- ビルドのディレクトリにコピーされた例の 1 行目の先に、スキーマのコピーがある
- 時間の軸のファイルは、基準のファイル（`tests/launch/timeline/`）、時間の軸のページの試験用のファイル（`gui/timeline/tests/fixtures/`）、例の実行の出力がスキーマに通り、エラーのある形が落ちる

このテストには、PyYAML と jsonschema のある Python が必要です（`uv sync`。Windows は MSYS2 の `mingw-w64-ucrt-x86_64-python-jsonschema`）。なければ、configure のときに理由を表示して登録しません。CI は、`linux-gcc-debug` で `schema.files_and_launcher` が登録されたことを確認します。

## 変更するときに一緒に直すもの

### `check` と `run` の出力

変える場所:

- `vehicle_sil_launch check` と `run` の出力（`libs/launch/src`）
- 実行の結果: 終わった理由（`end_reason()`）、結果（`RunReport::result()`）、人に見せる 1 文（`RunReport::message()`）は `libs/launch/src/format/run_report.cpp`。結果ごとの終了コードと `ok` は `libs/launch/src/format/exit_codes.hpp` の 1 つの表（`check` と `connections` の終了コードも）
- `examples/ecu_main/simulation.yaml`

合わせて直すドキュメント:

- [simulation-config.md](simulation-config.md)、[simulation-check.md](simulation-check.md)、[simulation-run.md](simulation-run.md)、[simulation-events.md](simulation-events.md): `<!-- ... -->` の行の直後のコードブロック（`launch_cli.docs` と `launch_run.docs` が照合）
- [howto/](howto/) のガイド: 同じ印のブロックのコピー（次の項目）

合わせて直すコードとファイル:

- `examples/ecu_main/simulation.live.yaml`（GUI で操作するための設定ファイル。速度と停止時刻は実行時に選択）: `simulation.yaml` と participant、ネットワークが同じであることを、`launch_run.live` が確認します。
- テンプレートの同じファイルは、`template.check_and_run` が確認します。
- ビルドのディレクトリの `simulation.live.dashboard.yaml` は、CMake が `simulation.dashboard.yaml` をコピーしたものです。
- GUI のテストが再生する、ランチャーの出力の記録（`gui/server/tests/recordings/`）: `gui/server/tests/record_launcher.py --launcher <vehicle_sil_launch>` で記録し直します。
- 記録と本物のランチャーの不整合は、`scripts/verify-gui.py --launcher <path>` が見つけます。

照合の方法と、エラーのある例の置き場所は、「[ドキュメントの例とテスト](code-map-testing.md#ドキュメントの例とテスト)」にあります。

### ガイドにある出力とコードのコピー

変える場所:

- `check` と `run` の出力（前の項目と同じ）
- `examples/ecu_main`（`scenarios/` を含む）と `examples/launch_control` のコード

合わせて直すドキュメント:

- [howto/](howto/) のガイド: 出力のコードブロック（`launch_cli.docs` と `launch_run.docs` が、リファレンスの同じ印のブロックと照合）
- [howto/](howto/) のガイド: 印のないコードの引用（照合しないので、手で合わせる）

ガイドごとの印とテストは、「[ドキュメントの例とテスト](code-map-testing.md#ドキュメントの例とテスト)」にあります。

### 照合のルールと `code`

変える場所:

- `libs/config/src/check_rules.cpp`
- 設定ファイルを読むコードは `config.cpp`

合わせて直すドキュメント:

- [simulation-check.md](simulation-check.md) の「照合」の `code` の表（テストはこの表を照合しないので、手で合わせる）

### 要求の欄と要求ファイル

照合・実行・テストが何を頼まれるかは、`RunRequest` という 1 つの値です。コマンドラインと要求ファイルのどちらもこの値を埋め、ライブラリが検証します。

変更する場所:

- 欄: `libs/launch/include/vehicle_sil/launch/request.hpp` の `RunIntent`（利用者の選択）か `RunHost`（呼び出し側の指定）。照合・実行・テストが読む欄は、別のオプションにせず、要求を通して `check_file()`、`run_file()`、`test_file()` に渡します。
- 一緒には成り立たない欄のルール: `libs/launch/src/request/request.cpp` の `validate_request()`。この種のルールを置く場所はここだけです（`code` が `usage` の指摘）。値が設定ファイルにとって何を意味するかのルールは、照合のルールです（「照合のルールと `code`」）。
- コマンドライン: `tools/launch/main.cpp` が引数を欄に写し、`validate_request()` を呼びます。ここの `cli::UsageError` は引数の形（値がない、形が違う、別のコマンドのオプション）のためで、意味のルールには使いません。
- 要求ファイル: 読み手は `libs/launch/src/request/request_read.cpp` の `read_request_file()` だけです。引数は欄ごとにファイルに勝ちます（`with_request_file()`）。GUI サーバーはファイルを書くだけです。
- 終了コード: `libs/launch/src/format/exit_codes.hpp` の 1 つの表（0 は合格かオペレーターの停止、1 は失敗、3 はファイルを読めないか準備の失敗、4 は使い方の誤り、`kExitIncomplete` はシナリオが途中で切れたとき、`kExitInterrupted` は中断）。`check`、`connections`、`test` も同じファイルです。

更新するコードとファイル:

- `schema/run-request.schema.json` と、そのコピー `template/schema/run-request.schema.json`（`schema.files_and_launcher` がキーをランチャーと突き合わせます）
- GUI サーバーが受けて書く欄（`gui/server/src/vehicle_sil_gui/launcher.py` の `INTENT_FIELDS` と `request_text()`。[GUI の地図](code-map-gui.md#ランチャーへの要求と接続先)）

更新するドキュメント:

- [simulation-run.md](simulation-run.md#要求ファイル--request) の「要求ファイル（`--request`）」と、[simulation-run.md](simulation-run.md) と [simulation-config.md](simulation-config.md) のオプションの表
- [simulation-run.md](simulation-run.md#終了コード) の「終了コード」と、[simulation-check.md](simulation-check.md) の終了コードの表

テスト: `tests/launch/test_request.cpp`（ルール）、`tests/launch/test_request_file.cpp`（ファイル）、`launch_cli.request`、`launch_run.connect_request`（プロセスとしてのランチャー）。

### 設定ファイルかシナリオのキー

シナリオ固有の変更は [機種とパラメーター](code-map-scenario.md#機種とシナリオのパラメーター) と [シナリオの変更](code-map-scenario.md#シナリオ) も参照。

変える場所:

- 設定ファイルかシナリオのキー、値の形、ネットワークの種類、手順の種類（`libs/config/src/config.cpp` と、シナリオの読み手（`libs/scenario/src/scenario_reader.hpp` と、そこに挙げたファイル）の、ファイルを読むコード）

合わせて直すコードとファイル:

- `schema/simulation.schema.json`、`schema/scenario.schema.json`: キーには `description` を付けます。エディターが、そのキーの説明として表示する文です。
- テンプレートのコピー: `cp schema/*.schema.json template/schema/`（ダッシュボードは `gui/protocol/dashboard.schema.json` から）
- 知らないキーのエラーの文が使う一覧（`config.cpp` と `libs/scenario/src/scenario_reader.hpp` の先頭の `k...Keys`）
- `tests/schema/test_schemas.py` の見本（`CONFIG_SAMPLES`、`SCENARIO_SAMPLES`）
- 要求ファイルのキーなら: `schema/run-request.schema.json` とそのコピー、`request_read.cpp` の `kKeys`、`REQUEST_SAMPLES`、`docs/simulation-run.md` のキーの表、`with_request_file()` での引数の重ね方

キーを受ける分岐と一覧の関係:

- ファイルを読むコードの多くは、キーを分岐（`key == "..."`）で受け、知らないキーの文には別の一覧を表示します。
- 分岐を追加したら、一覧にも追加します。
- テストが比べるのは一覧のほうなので、追加し忘れると、スキーマが新しいキーを知らないまま通ります。

手順の種類を追加するとき:

1. `scenario.schema.json` の `definitions` に `step_<種類>` を 1 つ追加します（その種類のキーと、時刻の 2 つのキー）。
2. `definitions.step` の `oneOf` に 1 行追加します。

守ること:

- スキーマは、ランチャーが読むファイルを拒否してはいけません（スキーマで拒否できないエラーは、`check` が報告します）。
- 迷う所は緩くし、`test_schemas.py` の見本に、通る形と失敗する形を追加します。
- 例とテンプレートの YAML（`examples/`、`template/`）を追加するときは、1 行目に `# yaml-language-server: $schema=<スキーマへの相対パス>` を書きます。
- 既にある例の先頭のコメントは、行数を変えません。指摘と手順の行番号が、ドキュメントの出力と、GUI のテストが再生する記録に出ているためです。

テスト:

- `schema.files_and_launcher`

### 設定ファイルの `network_names`

`network_names` は、participant ごとの、ネットワークの名前の対応です。

変える場所:

- 読むコードと、設定ファイルだけで分かるエラー: `libs/config/src/config.cpp` の `read_network_names()` と `check_network_names()`
- 機種の行の重ね方: `lay_network_names()`、引数にするコード: `map_network_arguments()`
- 照合のルールと出力が見るネットワークの名前

守ること:

- 照合のルール、表、JSON の `networks` は、自己申告のネットワークを `seen_networks()`（`libs/config/src/check_networks.cpp`。対応を当てるただ 1 つの関数。`SeenEcu::seen_networks()`（`libs/config/include/vehicle_sil/config/detail/seen_ecu.hpp`）が呼ぶ）からだけ読みます。`description.networks` を自分で読むルールは、名前を替えたネットワークを実行ファイルの名前で見てしまいます。`launch.described_networks_are_read_in_one_place`（`cmake/VehicleSilCheckDescribedNetworks.cmake`）が、`libs/launch` と `tools/launch` の `.networks`、`->networks`、`::networks` の行を読んで確認します。設定の `config.networks` だけは数えないので、ほかの構造体のメンバーに `networks` という名前を付けると落ちます。別の名前にします。ルールや出力を追加したら、名前を替えたネットワークの場合を `tests/config/test_check.cpp` に追加します。
- `run` の引数は `detail::participant_arguments()`（`libs/runner/src/run_participants.cpp`）の 1 か所で作ります。起動、`manual` のコマンド、イベント `participant` が同じ列を使います（`--map-network` と `--j1939-node` はほかの後。`describe: false` ではプレースホルダーを置き換えた後。その後に、実行のシナリオが値を読む participant には `--sample-every-step`。`RunMember::sample_every_step`）。

合わせて直すドキュメント:

- [simulation-config.md](simulation-config.md) の「ネットワークの名前を合わせる（`network_names`）」と、キーの表、機種の表
- [simulation-check.md](simulation-check.md) の `network-map-*`、`unknown-network`、`single-participant` と、JSON の `network_map`
- [simulation-events.md](simulation-events.md) の `features` の `network-names`
- スキーマ（`schema/simulation.schema.json` とテンプレートのコピー）

テスト:

- `^launch\.`（`tests/config/test_config.cpp`、`tests/config/test_check.cpp`、`tests/runner/test_run.cpp`）
- `launch_run.network_names`: 名前の違う ECU が 1 つのバスで通信する、機種で対応が替わる、`manual` のコマンドとイベント、`--map-network` を受け取れない実行ファイル
- `schema.files_and_launcher`

### 設定ファイルの `j1939_nodes`

エンジン側の型と実装は [J1939 エンジン](code-map-j1939.md#libsj1939エンジン) を参照。

`j1939_nodes` は、participant の J1939 のノードに、実行ファイルのコードと別のアドレスと NAME の欄を与えます。

変える場所:

- 読むコードと、設定ファイルだけで分かるエラー: `libs/config/src/config.cpp` の `read_j1939_nodes()` と `check_j1939_nodes()`
- 機種の行の重ね方: `lay_j1939_nodes()`、引数にするコード: `j1939_node_arguments()`（行の文字列は `ecu_executable::format_j1939_node_argument()`。`ecu_main` が読み戻します）
- 自己申告のノードに行を当てるコード: `libs/config/src/check_networks.cpp` の `seen_networks()`（`j1939::apply_node_overrides()` を使う）
- 行についての指摘と、`address-conflict`、`address-shared`、`name-conflict` の直し方: `libs/config/src/check_rules.cpp` の `check_j1939_node_entries()` と `conflict_fix()`
- 設定ごとに要る `features` の語: `vehicle_sil/config/config.hpp` の `kJ1939NodeSettingFeatures`（後で足す設定は、自分の語を持ちます）

守ること:

- ランチャーで行を当てるのは `seen_networks()` だけです。ルールは、その結果（`SeenNetwork::j1939_matches`、`unmatched_j1939_entries()`）を読み、当て直しません。
- 照合と実行ファイルは同じ決まり（`vehicle_sil/j1939/node_override.hpp`）で当てます。`launch.j1939_nodes_match_like_the_network_does` が、同じ場面を `seen_networks()` と `j1939::Network` の両方に通します。
- ノードに当たらない行があると、その participant のそのネットワークのノードを、アドレスと NAME のルールに入れません（`Rules::j1939_held`）。

合わせて直すドキュメント:

- [simulation-config.md](simulation-config.md) の「J1939 のアドレスと NAME を合わせる（`j1939_nodes`）」と、キーの表、機種の表
- [simulation-check.md](simulation-check.md) の `j1939-node-*`、`address-conflict`、`address-shared`、`name-conflict`、participant の表、JSON の `j1939_nodes`
- [simulation-events.md](simulation-events.md) の `features` の `j1939-nodes`
- スキーマ（`schema/simulation.schema.json` とテンプレートのコピー）

テスト:

- `^launch\.`（`tests/config/test_config.cpp`、`tests/config/test_check.cpp`、`tests/runner/test_run.cpp`、`tests/config/test_j1939_nodes_match.cpp`）
- `launch_run.j1939_nodes`: 同じ ECU の 2 つが行で通信する、行がないと照合が書く行を言う、機種で行が替わる、`manual` のコマンドとイベント、`describe: false` の participant、`--j1939-node` を受け取れない実行ファイル
- `schema.files_and_launcher`

### イベント、標準入力のコマンド、`version --json`

GUI が消費するイベントやコマンドを変える場合は、[GUI の実装とテスト](code-map-gui.md#guiserverguiprotocolguiclientguiweb)、[実行の送受信](#実行の送受信traffic) にある録画とプロトコルの例も確認する。

変える場所:

- `run --events` のイベント、`--stdin-commands` のコマンド、`version --json`（`libs/launch/src/format/events.cpp`、`libs/launch/src/endpoints/command_input.cpp`、`tools/launch/main.cpp`）
- `result` イベントの中身: `libs/launch/src/format/run_format.cpp` の `format_result_event()`（`EventStream::result()` は、書かれたものを受け取ります）。時刻のコマンドか stop の `command` イベント: `TimeCommands::apply()` が、渡された関数（`TimeCommands::OnCommand`。`main.cpp` は `EventStream::command()` を渡します）を呼びます。`phase` イベント: `RunControls::set_phase()` が、操作盤に渡された関数（`RunControls::Observers`。`main.cpp` は `EventStream::phase()` を渡し、自分で操作盤を作る run は `RunLent::events`（`RunEventSink`）に書きます）を呼びます

合わせて直すドキュメント:

- [simulation-events.md](simulation-events.md) の「プログラムから使う」: イベントと `state` の表、コマンドの表、`features` の表
- [simulation-events.md](simulation-events.md) の「プログラムから使う」: イベントの例（`launch_run.events` が、置き換えの後にバイト単位で照合）
- [simulation-events.md](simulation-events.md) の「プログラムから使う」の「読む側が遅いとき、読まないとき」（下の不変条件を変えたとき）
- [simulation-config.md](simulation-config.md): 引数の表（`--events-stall-ms` を変えたとき）

変更ごとに決めること:

- イベントやキーを追加したら、`features` に名前を追加するかを決めます。
- 意味を変えるなら、`kEventsFormat` を上げます。
- `events.hpp` の `kFeatures` と、ドキュメントの `features` の表は、手で合わせます。
- `participant` の `state` を追加したら、`state` の表に追加します。
- 指摘を追加したら、`detail::settled_findings()`（途中で流してよいか）を見直します。

守ること:

- イベントを書くスレッドと待ち行列の不変条件は、`events.hpp` の `EventStream` のコメントに書いてあり、ドキュメントをそれに合わせます。
- 不変条件は、イベントを作るスレッドは読む側を待たないこと、捨ててよいのは `log` だけであること、読まない読む側を待つのをやめるまでの時間です。
- その時間は `--events-stall-ms` で決まります（`tools/launch/main.cpp` の `events_stall_timeout()` と `kMinEventsStallMs`、既定は `EventStreamOptions::stall_timeout`）。

テスト:

- `^launch\.`（`tests/launch/test_events.cpp`）
- `launch_run.events_stalled`、`events_slow`、`events_nonblocking`

### `run` の時間

速度、一時停止、ステップ実行、実時間への遅れです。

変える場所:

- `libs/runner/src/time_commands.cpp` の `TimeCommands`
- 操作盤の時間のロック: `check_file()` が、構成を読んだ直後（どの `--describe` よりも前）に `on_time_lock` を呼び、`run_file()` がそこで `RunControls::set_time_lock()` にロックを渡します。それまで `TimeCommands::apply()` は停止のほかを決めません。コマンドは届いた順に待ち、`set_time_lock()` が決めます（ファイルから構成ができなければ `detach()` が断ります: 読めない、構文などで読み取れない、上書きがファイルに合わない。`RunControls::set_phase(Phase::Ended)` は段階を伝える前に時間のコマンドを切り離し、段階とその状態を TimeCommands のミューテックスの中で伝えるので、始まらなかった実行も含め、どの実行も同じ形で終わります。例外で抜ける実行も `run_file()` が終えます）。ロックの行と `--time-policy free` の注意は `control_rules.cpp`（`ControlSituation::time_locked`、`time_free`）
- 注記 `outside-time-changed` と `unsynchronized-time-changed`: `run_assess.cpp` の `time_changed_findings()`。材料は `RunOutcome::time_changed`、`time_lock`、`real_time_peers`（`time_plan.cpp` の `detail::real_time_peers()`）
- 時刻のコマンド、停止、書き込みを受けるかどうかと、断るときの文: `libs/runner/src/control_rules.cpp` の行（`TimeCommands::apply()`、`RunControls::allowed()`、Control Server の `observe_only` が読む 1 か所。標準入力の停止も、Control Server の停止と同じく `TimeCommands::apply()` を通ります。行のテストは `tests/runner/test_control_rules.cpp`）
- `time_plan.cpp` の `detail::plan_time_step()`（controller のタイムステップの既定）、`detail::coarse_time_step()`、`RunTiming::actual_speed()`（宣言は `time_plan.hpp`。`check.cpp` は `run.hpp` なしで使います）
- `time_plan.cpp` の `time_lock()`、`planned_speed()`、`time_lock_findings()`: シミュレーションの外につながる実行が実時間を保つかどうか（`--time-policy`）、始めるときの速さ、照合の指摘 `outside-*`。`check_file()` が結果を `CheckReport::time_lock` に置き、実行はそこから速さを読みます
- `run_notes.cpp` の `RunNotes::note_time()`（`time` を書く時点。止まっていると数えるかは `run_notes.hpp` の `detail::noted_standing()`。`state` の `paused` は `run_controls.cpp` の `RunControls`）
- イベント `state`: 中身と知らせる時は、`run_controls.cpp` の `RunControls::compose()` と `publish()`（`paused` と `speed` は `time_commands.cpp` の `facts_of()`、`pacing` は `run_pacing.hpp` の `detail::PacingWatch`）。最新の 1 つだけが書かれるのを待つことは、`events.cpp` の `EventStream::emit()`
- `run_watch.cpp` の `RunWatch::note_timing()`
- 時間のゲート: `libs/core/src/simulation_controller.cpp`
- ゲートの状態と遅れの計算: `libs/core/src/time_gate.cpp`（時計を持たない）
- 期限までの待ち: `libs/core/src/deadline_waiter.cpp`

合わせて直すドキュメント:

- [simulation-run.md](simulation-run.md) の「速度を指定して動かす」（遅れの定義、遅れたときに起きること、タイムステップと ECU のステップ）
- [simulation-run.md](simulation-run.md): 設定ファイルのキーの表、`time` と `command` のイベント、コマンドの表、`timing` の表、`features` の表
- [simulation-run.md](simulation-run.md): 指摘の表（`behind-real-time`、`coarse-time-step`）
- [internals/virtual-time.md](internals/virtual-time.md) の「一時停止、ステップ実行、速度（時間ゲート）」（遅れの定義を変えたとき）
- simulation-config.md: 遅れの定義を変えたとき

守ること:

- 遅れの定義は、`TimeControlOptions` のコメント（`simulation_controller.hpp`）に書いてあり、ドキュメントをそれに合わせます。
- コマンドは、標準入力を読むスレッドがゲートに渡します（待ちません）。`TimeCommands` のロックの中で待つものを追加しません。
- 遅れの数は、時刻を決めた例のテスト `core.time_gate_model_*` で確認します。実時間を使うテストは、片側の検査だけにします。

テスト:

- `core.time_gate_model_*`、`core.virtual_time_gate_*`、`core.*deadline_waiter*`
- `^launch\.`
- `launch_run.realtime`、`on_time`、`late`、`stall`、`speed_change`、`step_width`、`pause_step`、`run_until_stdin`、`state_on_arrival`、`paused_failures`、`time_usage`、`connect_time_changed`

### 自分で起動する participant

`manual` と `--manual` です。`run` はそのプロセスを持たないので、失敗は、シミュレーションから抜けたことでだけわかります。

変える場所:

- 読むコード: `libs/config/src/config.cpp`
- 起動の代わりにコマンドを書くコード: `libs/runner/src/run_participants.cpp` の `RunParticipants::show_manual()` と `detail::shell_command_line()`
- 開始の待ち: `run_start.cpp` の `RunStart::wait_for_start()`
- 終わり: `RunParticipants::end_manual()`

合わせて直すドキュメント:

- [simulation-config.md](simulation-config.md) の「participant を自分で起動する」と、その例（`launch_run.manual` が照合）
- [simulation-events.md](simulation-events.md): イベント `participant` の `manual`、`features` の `manual-participants`
- [howto/first-ecu.md](howto/first-ecu.md) の「デバッガーから動かす」（同じ標準エラーの行と結果の表を、`Cooler` の名前で載せている。照合しないので、手で合わせる）
- [internals/control.md](internals/control.md) の「ランチャーの Control Server」の開始の期限（下の値を変えたとき）

開始の上限、ECU に渡す `--start-timeout-ms`（`detail::ecu_start_timeout_ms()`）、registry の既定のポート（`kManualRegistryUri`）を変えたら、simulation-config.md の同じ節と、internals/control.md の開始の期限を直します。

テスト:

- `launch_run.manual`（docs の例）
- `launch_run.manual_*`（場面ごと。`ctest -R '^launch_run\.manual'`）
- `launch.a_participant_started_by_hand_from_the_file_or_the_command_line`
- `launch.the_command_to_start_a_participant_by_hand_is_quoted_for_the_shell`

### 実行の送受信（`traffic`）

実行の結果の `traffic` です（ネットワークごとの J1939 の送受信の数と `delivered`）。

変える場所:

| 内容 | 場所 |
|---|---|
| tap とシナリオの列 | `libs/runner/src/run_samples.cpp` の `RunSamples` |
| participant ごとに残す最新の数組 | `libs/runner/src/run_traffic.cpp` の `TrafficLatest` |
| 結果を作る | `count_traffic()` |
| ネットワークの `senders`、`receivers`、`delivered` | `TrafficNetwork` の関数 |
| hello のスキーマから、ネットワーク名と数の出力の位置を取り出す | `traffic_schema()` |
| 誰がどのネットワークにいるか | `traffic_plan()`（`seen_networks()` から） |
| 終わりに読む所 | `run_watch.cpp` の `RunWatch::settle()`（`RunSamples::finish()`）。`Runner::run()` が `RunWatch::watch()` の後に呼ぶ |
| 文と JSON | `run_format.cpp` の `traffic_text()` と `write_run_json()` |

合わせて直すドキュメント:

- [simulation-run.md](simulation-run.md) の「ネットワークの送受信（`traffic:`）」（行の読み方、数え方、`sent` の意味、数えない理由の表、実行ごとに変わる場合）と、「JSON」の `traffic` の表
- `run` の出力例（`launch_run.docs`、`launch_run.scenario` などが照合する `simulation-run.md`、`scenario.md`、`simulation-config.md`、`simulation-events.md`、`howto/first-ecu.md`、`howto/scenario-tests.md` のブロック）
- `features` の `run-traffic`
- [internals/control.md](internals/control.md) の「ランチャーが受け取るサンプル」
- GUI の録画（`gui/server/tests/recordings/`）と `gui/protocol/examples/`

守ること:

- tap の中は、participant ごとの数組を置き換えることと、シナリオの列に積むことだけにします（待たない、Hub を呼ばない）。
- `traffic` のために列を持ちません（誰も取り出さない間にあふれる状態を作らない）。
- 数は「終わりの時刻より前に始まった最後のステップ」の値で、それ以後のサンプルが届いたかどうかで出力が変わらないようにします。
- 数の出力は、Agent が申告した名前（`PeerInfo::sources`）で探し、対応が当たっているものは数えません。
- 行は割合で言いません（受けるだけの participant がいるのは正常なので）。`delivered` の `false` は、数だけで言い切れるときだけです。そのネットワークにいるかもしれない participant を全員数えている必要があります。`--describe` のない participant は、数を持たないネットワークのどれにもいるかもしれないので（`TrafficNetwork::unplaced`。数は、いる場所を言い、いない場所を言いません）、そのネットワークの `delivered` を `null` にし、行も `nothing sent` と言わずに数で言います。`RunTraffic::unplaced` は、どのネットワークにいるかも知らないものの一覧です。どちらの要素にも 2 つの理由のどちらかが付きます。実行の終わりまでに hello が来なかった（実行ごとに変わりうる）か、数を持たない（変わらない）かです。
- シナリオに渡すサンプルを変えません（シナリオが始まった後に tap が受け取った、始まったときの Agent のものを全部。上限なし。`RunSamples::scenario_begins()`）。

テスト:

- `launch.the_numbers_are_those_of_the_last_step_*`、`launch.when_the_samples_come_*`、`launch.only_a_few_samples_are_kept_*`、`launch.a_flood_while_nobody_looks_*`、`launch.a_network_says_whether_*`、`launch.every_network_of_the_file_*`、`launch.a_participant_that_is_not_counted_says_why`（`tests/runner/test_traffic.cpp`）
- `launch.the_text_for_people`、`launch.the_json_report`
- `launch_run.traffic`: 例の 2 つの ECU で実行ごとに同じ出力、`network_names` で分かれたバスとそろえたバス、変数を宣言しない ECU、数えない participant、誰がいるかわからないネットワーク
- `^launch_run\.scenario`、`control_server.launcher`

### ランチャーの上限

変える場所:

- `kMaxMillis`、`kMinSpeed`、`kMaxSpeed`（`libs/config/include/vehicle_sil/config/config.hpp`）

合わせて直すコード:

- GUI のコピー: `gui/server/src/vehicle_sil_gui/launcher.py`、`variables.py`
- GUI のコピー: `gui/client/src/client.ts`、`protocol/server.ts`
- GUI のコピー: `gui/web/src/app/run/speed-picker.ts`

テスト:

- `launch.the_gui_takes_the_launchers_limits` が、不整合を見つけます。
- ほかのツールの終了コードと上限（`tools/common/include/vehicle_sil/cli/args.hpp`）は、同じテストのファイルの `static_assert` が確認します。
