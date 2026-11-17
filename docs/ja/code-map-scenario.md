# シナリオ・時間の軸・一括実行

[English](../code-map-scenario.md)

<!-- 書き方: 項目は、同じ形の短いブロックにします（小見出し、1 行に 1 つの事実の箇条書き、セルが 1 文の表）。1 つのセルや 1 つの箇条書きに複数の事実を詰めません。1 行が長くなり、別々の変更が同じ行に当たるためです。 -->

この分野を変更するときの地図です。全体の入口は [code-map.md](code-map.md)、テストの共通ルールは [code-map-testing.md](code-map-testing.md) にあります。

関連する仕様と変更範囲: [scenario.md](scenario.md) / [simulation-run.md](simulation-run.md) / [ランチャー・プロセス・設定・実行](code-map-launcher.md) / [Control API・gRPC・ランチャーの Control Server](code-map-control.md)

## ディレクトリごとのコードとテスト

### `examples/scenario_tests`（ガイド「シナリオでテストを書く」の例）

ガイド [howto/scenario-tests.md](howto/scenario-tests.md) が、利用者に書かせるファイルです。ファイルはガイドのブロックと同じ内容で、利用者はガイドのとおりに、`examples/ecu_main` のビルドのディレクトリで自分で書きます。

| ファイル | 内容 |
|---|---|
| `my_tests/throttle_up.yaml`、`my_tests/response_time.yaml`、`my_tests/throttle_motion.yaml`、`my_tests/dashboard_speed.yaml` | 4 つのシナリオ |
| `simulation.variants.yaml` | 機種を 2 つ追加した設定ファイル |

ファイルの先頭のコメントは、ガイドのブロックにはありません。ガイドの出力の行番号は、コメントのないファイルのものです。

- ビルド: しません（実行ファイルは `examples/ecu_main` の 2 つです）。
- `ctest -R`: `^launch_run\.scenario_guide`

`launch_run.scenario_guide`（`tests/launch/scenario_guide.cmake`）は、作業用のディレクトリで、ガイドの順に次のことをします。

- ファイルをガイドのブロックと照合してから書く
- `check`、`run`、`test` の出力と終了コードを照合する
- `test --json` の抜粋のキーを照合する
- `examples/ecu_main/scenarios` の `throttle_step.yaml` と `engine_disconnect.yaml` のブロックと出力も照合する

## 変更するときに一緒に直すもの

### 機種とシナリオのパラメーター

設定ファイルの `variants` と `params`、`--variant`、`--param`、シナリオの `params`・`${name}`・`only` です。`absent: true` の変数や、機種が無効にした participant の変数を使う手順のスキップも含みます。

変える場所（設定ファイルの側。`libs/launch/src/config.cpp`）:

- `read_variants()`
- `kVariantKeys`、`kVariantParticipantKeys`（機種で変えられるキーの一覧）
- `apply_variant()`、`choose_variant()`

変える場所（シナリオの側とその周り）:

- `params` の読み方: `yaml_reader.hpp` の `read_params()`
- `scenario.cpp` の `take_params()`、`resolve_params()`、`read_only()`、`skip()`
- 渡すコード: `check.cpp` の `ScenarioContext`
- スキップするコード: `scenario_run.cpp` の `issue_step()`（`StepStatus::Skipped`）
- 引数: `tools/launch/main.cpp` の `common()`

合わせて直すドキュメント:

- [simulation-config.md](simulation-config.md) の「機種」と「シナリオのパラメーター」
- [scenario.md](scenario.md) の「機種とパラメーター」と `code` の表
- [simulation-check.md](simulation-check.md): `code` の表と、JSON のキー（`variant`、`variants`、`params`）
- [simulation-run.md](simulation-run.md)
- [simulation-events.md](simulation-events.md) の「実行に進まないときのイベント」

機種で変えられる項目を追加するときは、キーの一覧、`VariantParticipant`、`read_variant_participants()`、`apply_variant()` と、simulation-config.md の表を一緒に直します。

テスト:

- `launch.a_variant_*`、`launch.every_variant_*`
- `launch.scenario_parameter*`、`launch.scenario_only_*`
- `launch.scenario_steps_that_use_*`、`launch.scenario_times_are_the_same_*`、`launch.scenario_that_checks_nothing_*`
- `launch_run.variants`

### シナリオ

`run --scenario` と `check --scenario` です。

変える場所:

| 内容 | 場所 |
|---|---|
| 読み方と照合 | `libs/launch/src/scenario.cpp`（ファイル、パラメーター、variant、手順）、`scenario_values.cpp`（変数と値）、`scenario_waves.cpp`（波形と点）、`scenario_inputs.cpp`（`inputs` と dither）。クラス `Reader` は `scenario_reader.hpp` |
| 手順を進める処理 | `libs/launch/src/scenario_run.cpp` |
| 手順が失敗した理由: 満たさなかった条件と、反映されなかった書き込み。メッセージと記録（`failed.conditions[]`、`failed.writes[]`）の両方 | `libs/launch/src/scenario_failure.cpp`（`unmet()`、`unmet_values()`、`failed_write()`、`applied_text()`、`during()`） |
| `ramp` と `wave` の値を計算する関数（`Waveform` の実装） | `libs/launch/src/scenario_shapes.cpp` |
| `points` の CSV ファイル | 分けるのは `libs/launch/src/scenario_csv.cpp` の `read_csv()`、読むのはシナリオのファイルの隣から `files_beside()`、パスの検査とセルの意味は `scenario_waves.cpp` の `path_problem()`、`file_points()`、`cell_value()` |
| 波形の計算 | `libs/signal` の `signal/waveform.hpp` |
| ランチャーへの組み込み | `libs/launch/src/run_scenario.cpp` の `RunScenario`（`begin()`、`loop()`、`finish()`） |
| 時間のゲートのガード | `SimulationController::guard_from()` と `wait_guarded()` |
| 全サンプルを受け取る tap | `control::Hub::set_values_tap()` |
| 実行の tap（シナリオは、ここからサンプルを取り出す） | `libs/launch/src/run_samples.cpp` の `RunSamples` |
| `dither` の読み手（最上位の `inputs`、手順） | `scenario_inputs.cpp` の `read_inputs()`、`read_dither()`、`read_overlay()`。波形の中身は、`wave` と同じ `read_shape()` で読む（`read_wave_of()` と共用） |
| 指摘 `scenario-dither-*` と、出力の一覧（`Scenario::lines`） | `scenario_inputs.cpp` の `note_lines()` と `check_dithers()` |
| エンジンの揺れの書き込み | `scenario_run.cpp` の `submit()`（動きのない間の、ECU のステップごとの書き込み。手順のものでない書き込みで、同じ時刻の分を 1 つの要求にまとめる）、`place()`（揺れを線に置く）、`fail_grid()`（その書き込みが断られたとき、まだ決まっていない最初の手順を失敗にする）。約束は `ScenarioEngine` のコメントの「Dithers」 |

合わせて直すドキュメント:

- [scenario.md](scenario.md): 形式、時刻、値の見え方、書き込み、結果、指摘の `code` の表、出力
- [scenario.md](scenario.md): `<!-- vehicle_sil_launch run simulation.yaml --scenario ... -->` の後のブロック（`launch_run.scenario` がバイト単位で照合）
- [scenario.md](scenario.md): `<!-- throttle_mistakes.yaml -->`、`<!-- throttle_response.yaml -->` のブロック
- [scenario.md](scenario.md): `<!-- vehicle_sil_launch check simulation.yaml --scenario scenarios/throttle_dither.yaml: the lines -->` の後のブロック（`launch_run.scenario_dither` が照合）
- [howto/scenario-tests.md](howto/scenario-tests.md): ガイドが載せているファイル、出力、終了コード（`launch_run.scenario_guide` が、ガイドの順に実行して照合）
- [simulation-config.md](simulation-config.md): 引数の表
- [simulation-events.md](simulation-events.md): イベント `scenario_step`、`features` の `scenario`
- [internals/control.md](internals/control.md) の「シナリオ」

例のファイル:

- 照合する例のファイルは `examples/ecu_main/scenarios/throttle_step.yaml` です。
- `wave` の例は `examples/ecu_main/scenarios/throttle_wave.yaml` で、`launch_run.scenario_wave` が実行します。
- `dither` の例は `examples/ecu_main/scenarios/throttle_dither.yaml` で、`launch_run.scenario_dither` が実行します。
- CSV ファイルの `points` の例は `examples/ecu_main/scenarios/throttle_trace.yaml` と `.csv` で、`launch_run.scenario_wave` が実行します。
- ガイド howto/scenario-tests.md は、`throttle_step.yaml`、`engine_disconnect.yaml` と、`examples/scenario_tests` のファイルを載せています（「[`examples/scenario_tests`](#examplesscenario_testsガイドシナリオでテストを書くの例)」）。
- エラーのある例と失敗する例は、ファイルも scenario.md のブロックです。`launch_run.scenario` が書き出して、`check --scenario` の出力、`run --scenario` の出力、`--json` の `scenario` を照合します。

守ること（仕様は、`scenario_run.hpp` の `ScenarioEngine` のコメントに書いてあります）:

- 判定に使うのは仮想時刻だけです。実時間の待ちと時間切れで、結果を決めません。
- `ramp` と `wave` の値は、手順の始まりからの仮想時間だけの関数にします（`Waveform::value()`）。呼ばれた回数や順に依存させません。
- 乱数は、種、変数の名前、時刻から作ります（`scenario_shapes.cpp` の `seed_of_variable()`）。整数だけの計算で、どの OS でも同じです。
- 入力ごとの線（シナリオが置いた値と、いま動かしている `ramp` や `wave`）は、`libs/launch/src/scenario_line.hpp` の `Line` です。`from` や `around` を省いた動きの始まりの値は、手順の時刻の書き込みを送る前に、手順の順で線から読みます（`scenario_run.cpp` の `place()`）。
- 動きの値を出す関数は `motion_number()`（丸める前）の 1 つで、書く値は、線の `Line::written()` の 1 か所で、揺れ（`dither`）を足してから型に丸めます。エンジンの `submit_one()` はこれを呼びます。値を出す所を追加するときも、同じ関数を呼びます。
- ECU に見えるかどうかの判定（短い `pulse`、`hold` の `points`）は、`scenario_waves.cpp` の `seen_between()` のルール 1 つに従います。
- `read_scenario()` は、`ScenarioContext::read_file` のほかにファイルに触りません（`check.cpp` と `test.cpp` が `files_beside()` を渡します）。
- 点を足す口（文面と CSV ファイル）は `read_points()` の 1 つで、同じ照合を通ります。
- controller のステップ P では、P より前に始まったステップのサンプルと結果だけを見ます。P 以降のものは、受信していても次のステップで扱います。
- 時刻 T の書き込みは、T より前の controller のステップを止めている間に積みます（時刻 0 は開始前の窓）。
- シナリオのある実行の controller のタイムステップは、速度によりません（`detail::plan_time_step()` の `fine`）。
- Hub の tap は、controller の SIL Kit のスレッドで、Hub のロックの中から呼ばれます。待つものと、Hub を呼ぶものを追加しません。

波形を追加するときに合わせるもの:

- `signal/waveform.hpp` の関数
- `scenario_shapes.cpp` の実装
- `scenario_waves.cpp` の `read_wave_of()`（キー、型、範囲、ステップ幅との比較）
- `schema/scenario.schema.json` の `definitions.wave`（と `template/schema` のコピー）
- `tests/schema/test_schemas.py` の `WAVE_SHAPES_WITH_KEYS` と見本
- [scenario.md](scenario.md) の波形の表

テスト:

- `^launch\.scenario_`: 偽の ECU のモデルで、受信の順を変えても結果が同じ
- `launch.scenario_wave_*`: 波形の値、終わった後の値、指摘
- `launch.scenario_line_*`（`tests/launch/test_scenario_line.cpp`）: 線だけのテスト
- `launch.scenario_dither_*`、`launch.timeline_has_the_dithers_*`（基準は `tests/launch/timeline/dither.yaml`）
- `launch.scenario_expect_beside_*`: `ramp` と `wave` の横の `expect` の区間、手順が決まる順、機種で `expect` だけスキップすること
- `^signal\.`
- `launch_run.scenario`
- `launch_run.scenario_wave`: 波形を ECU が見た時刻、タイムステップを変えても通ること、警告と `undersampled: ok`
- `launch_run.scenario_expect_beside`: 横の `expect` が外れたサンプル、`expect_skipped`
- `launch_run.scenario_dither`: 本物の ECU、範囲の外、例 `examples/ecu_main/scenarios/throttle_dither.yaml`
- `launch_run.scenario_same`: 最速、等速、遅い ECU、一時停止で同じ結果。書き込みが反映されたステップを、ECU が報告する
- `launch_run.scenario_failures`
- `core.time_gate_model_the_guard_*`、`core.virtual_time_gate_guard_*`
- `control.control_hub_tap_*`

### 時間の軸のファイル

`run --scenario --timeline` です（[simulation-run.md](simulation-run.md) の「シナリオの実行を時間の軸で記録する（`--timeline`）」）。

変える場所:

| 内容 | 場所 |
|---|---|
| 記録、上限、古い側を捨てる決まり | `libs/launch/src/timeline.cpp` の `TimelineRecord` |
| 記録に入れる所 | `scenario_run.cpp` の `submit_one()`（書き込み）、`observe()`（サンプル）、`place()`（入力の線 `lines[]`。`shape` の中身はシナリオの読み手が読んだ `ScenarioMotion::described`）、`issue_step()`（見張り）、`fail()`（失敗の中身。書く内容は `scenario_failure.cpp` が作る）、`close_record()`（記録の終わり `recorded_to_ms`） |
| ファイルの形 | `timeline.cpp` の `format_timeline_json()` |
| 1 行に 1 点 | `libs/json/include/vehicle_sil/json/json_writer.hpp` の `JsonWriter::begin_array_on_one_line()` |
| ファイルを開く所と書く所 | `run.cpp` の `Runner::open_timeline()` と `write_timeline()` |
| 指摘 `timeline-write` と `timeline-limit` | `run_assess.cpp` |
| 引数 | `tools/launch/main.cpp` |
| ファイルを描く要素 `<vehicle-sil-timeline>` | `gui/timeline/viewer.js`（「The element」より上は DOM なしで動き、単体テストが呼ぶ部分。その下は、`enter()` がいまの要素にしたものに描く関数と、属性・プロパティ・事象を持つクラス `VehicleSilTimelineElement`）。CSS は `gui/timeline/timeline.css`（shadow root の中。`hide`、`compact`、`theme` をここで読む） |
| 利用者が読む 1 つのファイル | `gui/timeline/vehicle-sil-timeline.js`。`gui/timeline/bundle.js` が `messages.js`、`timeline.css`、`viewer.js` から作る（`gui/web` で `npm run bundle:timeline`。古いと `npm run test:timeline` が落ちる）。型は `gui/timeline/vehicle-sil-timeline.d.ts`（手で書く。クラスと合わせる） |
| 時間の軸のページ | `gui/timeline/timeline.html` と `page.js`: 要素 1 つと、ページが足すもの（`?src=`、`?lang=`、`#token=`、落とした・選んだファイル、埋め込んだファイル）。利用者の最小のページは `gui/timeline/example.html` |
| 文言（英語と日本語） | `gui/timeline/messages.js`: 要素とページが出す文言のすべてを、鍵ごとに両方の言語で（鍵は単体テストが確かめる。`viewer.js` と `page.js` の単語の並んだ文字列は、lint が断る）。ページは、要素の `load.*` の文を、要素の `messages` のプロパティで自分の言い方（`pageLoad.*`）にする |
| GUI サーバー: シナリオの実行に付ける `--timeline`、残すファイル、配るページとファイル | `gui/server/src/vehicle_sil_gui/timelines.py`、`server.py` の `_start()`、`_keep_timeline()`、`get_timeline()`、`get_timeline_page()` |
| サンプルの画面のリンク「時間の軸」 | `gui/web/src/app/run/scenario-panel.ts` の `ScenarioPanel` と `timelinePageUrl()` |
| サンプルの画面の中の時間の軸（Angular の手本） | `gui/web/src/app/run/timeline-view.ts` の `TimelineView` |
| npm パッケージとツールの圧縮ファイルの中の要素 | `gui/client/pack.mjs`（`@vehicle-sil/gui-client/timeline`）、`scripts/package.py` の `TIMELINE_ELEMENT`（`client/`） |
| 新しい種類の段、送った値の新しい状態 | `laneKind()` と `variableLane()` の分岐、状態は `pointState()` と `holdSegments()`、`timeline.css` の CSS のクラス（その後に `npm run bundle:timeline`）。名前を出す代わりに描かせる値は `KNOWN` |

合わせて直すもの:

- `schema/timeline.schema.json`
- [simulation-run.md](simulation-run.md) の「シナリオの実行を時間の軸で記録する（`--timeline`）」
- 基準のファイル `tests/launch/timeline/*.timeline.json`。手で直しません。作り直し方と比べないキーは、そのディレクトリの `README.md` にあります。
- ページに新しいキーや値を描かせるときは `gui/timeline/viewer.js`。それまでは、知らないキーは無視し、知らない値は描かずに名前を出します。形の版や `stage` が新しいファイルは、表示できないと言います。

守ること:

- 記録は、シナリオのスレッド（と、始まる前の窓のスレッドの `begin()`）でだけ行います。Hub の tap の中では何もしません。
- 記録の口がない（`--timeline` なし）とき、エンジンの動きと `run` の出力は 1 つも変わりません。
- ファイルに入れるのは、仮想時刻で決まるものだけです（実時間、プロセス ID、遅れの数を入れません）。
- キーと値は足すだけにし、読む側が知らないものを無視できる形を保ちます（スキーマは値を `enum` で閉じません）。
- 基準のシナリオには `sine`、`chirp`、`lag` を入れません。最後のビットが C ライブラリで違いうるので、バイト単位で比べられません。

テスト:

- `launch.timeline_*`: タイムステップによらないこと、タイムステップで変わる手順、基準のファイル、上限、サンプルの数え方、失敗した `set` のどの書き込みが入ったかと、それぞれの点、途中で終わった実行
- `launch.a_timeline_that_dropped_points_or_was_not_written_is_said`
- `launch_run.timeline`（本物のランチャー）: 2 回実行して同じ、速度を変えて同じ、開けないファイル、書けないファイル、失敗する実行、`stop_at_ms` と `--timeout-ms` で終わる実行、始まらない実行、引数
- `schema.files_and_launcher`
- 基準のファイルと、ページの試験用のファイル `gui/timeline/tests/fixtures/`（基準のファイルにない形。`tests/schema/test_schemas.py` がスキーマに通ることを確かめる）でページを確かめるもの: `gui/timeline/tests/viewer.test.js`（読み込み、モデル、配置。Node で。`gui/web` で `npm run test:timeline`）と `gui/web/e2e/timeline.spec.ts`（ブラウザで開いたページ）、`gui/web/e2e/timeline-element.spec.ts`（利用者のページの上の要素: 素の例、1 つのページに 2 つ、すべてのプロパティと事象、CSS が中に入らず外に出ないこと、インラインのスタイルを許さない CSP）。CTest にはありません。`scripts/verify-web.py` が回します（`--e2e-grep 'timeline page'`、`--e2e-grep 'timeline element'`）。
- GUI サーバーの部分: `gui/server/tests/test_timelines.py`（機能のあるランチャーとないランチャーでの引数、トークンのあるときとないときのファイルとページ、残すファイルの数）。記録 `run-scenario-*-timeline`（`--timeline` を付けて記録したもの。`record_launcher.py --compare` が本物のランチャーと比べる）を使います。GUI のリンクから開くページと、サンプルの画面の中の要素は `gui/web/e2e/timeline-link.spec.ts`（`--e2e-grep 'timeline'`）。画面のテンプレートの中は `gui/web/e2e/starter.spec.ts`。

### シナリオをまとめて実行する

`vehicle_sil_launch test` です。1 組の中身は `run` のままなので、`run --json` を変えたら `runs[].run` も変わります。

変える場所（`libs/launch/include/vehicle_sil/launch/test.hpp` と `src/test.cpp`）:

| 関数など | 内容 |
|---|---|
| `list_scenarios()` | ディレクトリからのシナリオの集め方と順 |
| `test_file()` | 組ごとに `run_file()` を呼ぶ |
| `format_test_text()` | 1 組だけを実行し直すコマンドに並べる引数は `TestArguments` |
| `format_test_json()` | `kTestFormat` |
| `TestReport::remarks()` | `test` 全体の指摘 `test-nothing-run`、`test-runs-left` |
| `TestReport::result()` | 結果と終了コード |
| `ScenarioCheck::not_for_variant` | `only` に合わない組を `skipped` にする |
| `CheckOptions::params_for_many` | `--param` の検査を緩める |
| `CheckOptions::file_only` | 設定ファイルだけ読む |
| `tools/launch/main.cpp` の `test()` と `spread_scenarios()` | 引数 |

`test_file()` は、`run_file()` の前に設定ファイルを読み、機種を確認し、どのシナリオにもない `--param` と、有効で manual の participant を拒否します。

合わせて直すドキュメント:

- [simulation-run.md](simulation-run.md) の「シナリオをまとめて実行する」（表、終了コード、JSON のキー）
- [simulation-config.md](simulation-config.md): 引数の表と終了コードの表
- `vehicle_sil_launch` の使い方の文: `kUsage`

テスト:

- `launch.the_scenarios_of_a_directory_*`、`launch.a_test_*`、`launch.the_report_of_a_test_*`、`launch.the_command_of_one_run_alone_*`（`tests/launch/test_batch.cpp`。プロセスを起動しない）
- `launch_run.test_command`、`launch_run.test_interrupt`
