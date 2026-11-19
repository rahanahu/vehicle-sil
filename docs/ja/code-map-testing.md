# テストの開発

[English](../code-map-testing.md)

共通のテスト規則と文書の照合です。分野別のテストは [code-map.md](code-map.md) から探します。

関連する仕様と変更範囲: [build.md](build.md)

**このページ:** [テストの名前と実行方法](#テストの名前と実行方法) / [テストのラベル](#テストのラベル) / [ドキュメントの例とテスト](#ドキュメントの例とテスト) / [境界を検査するテスト](#境界を検査するテスト)

## テストの名前と実行方法

```sh
ctest --preset linux-gcc-debug -R '^launch\.'
scripts/verify.py linux-gcc-debug --tests '^launch_run\.'
```

### 名前と登録

テストの名前は `<接頭辞>.<名前>` です。接頭辞は `tests/<領域>/CMakeLists.txt` の `PREFIX` です。接頭辞を複数の領域が共有することがあります。`launch.*` は `tests/config`、`tests/scenario`、`tests/runner`、`tests/launch` にあり（実行ファイルは各 1 つ。前の 3 つはディレクトリのラベル `config`、`scenario`、`runner` も持ちます）、層ごとの試験がその層だけをリンクするようにしてあります。

| テストの種類 | 名前 | 登録の方法 |
|---|---|---|
| テストの実行ファイルのテスト（ハーネス） | `<接頭辞>.<TEST_CASE の名前>` | ソースの `TEST_CASE(名前)` から、configure のときに登録されます（`vehicle_sil_add_harness_tests()`、`cmake/VehicleSilTestHelpers.cmake`）。 |
| プロセスを起動するテスト（シナリオ） | `<接頭辞>.<シナリオ>` | `vehicle_sil_add_scenario_tests()` で登録します。 |

ハーネスのテストを追加するとき:

- 既存のソースに `TEST_CASE` を追加するときは、CMake を書き換えません。
- 新しいソースのファイルは、その `CMakeLists.txt` の `SOURCES` に追加します。
- マクロ（`TEST_CASE`、`CHECK`、`CHECK_EQ`）は `tests/unit/test_harness.hpp` にあります。

シナリオのテストの場所:

- シナリオの一覧は `CMakeLists.txt` の `SCENARIOS` です。
- 中身は同じディレクトリの `*launcher.cpp`（`tests/integration/launcher.cpp`、`tests/launch/launch_run_launcher.cpp` など）です。
- 共通のコードは `tests/integration/launcher_support.hpp` にあります。
- `launch_run` のシナリオは、題ごとに `tests/launch/launch_run_<題>.cpp` に分けてあります。`main()` は `launch_run_launcher.cpp` に、共通のコードは `launch_run_support.hpp` にあります。

領域（`tests/` の直下のディレクトリ）のルール:

- `CMakeLists.txt` のあるディレクトリは、名前の順に自動で加わります（`tests/CMakeLists.txt` に行を追加しません）。
- 条件のある領域（`control_server`、`fetchcontent`、`integration`、`virtual_time` など）は、自分の `CMakeLists.txt` の先頭で条件を見て `return()` します。
- 領域は、ほかの領域のターゲットを `if(TARGET ...)` で確認しません。領域が加わる順に依存してしまうためです。
- CMake の一覧（`add_subdirectory`、ソース、シナリオ）は、1 行に 1 つ、名前の順に書きます。順に意味がある所は、コメントに書いてあります。

### 一部のテストだけを実行する

- 1 つのテストは、テストの実行ファイルに名前を渡すと動きます（`<実行ファイル> <名前>`。ctest もそうしています）。
- 接頭辞だけを指定するときは、`\.` まで書きます。`^j1939` は `j1939_parts.`、`j1939_codec.`、`j1939_silkit.`、`j1939_spy.` にも一致します。
- ラベルで選ぶときは `-L` を使います（[テストのラベル](#テストのラベル)）。

### リスクに応じて検証を選ぶ

変更が壊しうるものを手元で実行し、プリセットの全組み合わせは CI に任せます。変更する分野のページに、その分野のテストと `ctest -R` の式があります（[作業対象から探す](code-map.md#作業対象から探す)）。

| 変更 | 手元で実行するもの |
|---|---|
| どの変更でも（既定） | 1 つのプリセットと、触ったもののテスト: `scripts/verify.py linux-gcc-debug --tests '<正規表現>'` |
| 並行性、スレッド、時間 | 同じテストを `linux-clang-tsan` でも |
| 多くのターゲットが含む公開ヘッダーや、クラス・関数テンプレート | 触ったターゲットを `linux-clang-tsan` でビルドする: `scripts/verify.py linux-clang-tsan --targets <target> --tests '<regex>'` |
| ランチャーが出力・計数するもの（所見、イベント、`run` の出力）と、それを写したドキュメント | `^launch`、`^multi_host\.`、`^schema\.`、`^template\.`（ECU のプロジェクトテンプレート）。ドキュメントと照合します（[ドキュメントの例とテスト](#ドキュメントの例とテスト)） |
| GUI が再生するランチャーの出力（`gui/server/tests/recordings/`）、または Control API | `scripts/verify.py linux-gcc-control --tests '^control'`、続けて `scripts/verify-gui.py --skip-web --launcher build/linux-gcc-control/tools/launch/vehicle_sil_launch` |
| ブラウザ GUI だけ | `scripts/verify-web.py --skip-install`（`--e2e-grep <パターン>` は、名前が一致する e2e テストだけを実行） |
| ドキュメントだけ | `scripts/check-docs.py` |
| スクリプトだけ | `python3 tests/scripts/test_<name>.py` |

作業の報告と pull request の説明は、次の 2 行で終えます。

```text
Verified: <実行したものと、それが確かめたこと>
Not run: <CI に任せたもの、実行しなかったものと、その理由>
```

実行していないものを `Verified:` に書きません。何も実行しなかったときは、両方の行にそう書きます。

### CI が実行するもの

`.github/workflows/ci.yml` のジョブ `all checks` が、`main` へのマージに必要なチェックです。ほかのジョブがすべて成功したか省かれたときに成功し、1 つでも失敗したか取り消されたときに失敗します。プルリクエストはマトリクスの一部を、`main` への push はすべてを実行します。

| 変更 | 実行するもの |
|---|---|
| 文書だけ | リンク（`python`）、文書を比べる `linux-gcc-debug` のテスト、GUI の README の書式。変更したファイルが `gui/` にあるか、GUI のテストが読む文書（`docs/gui.md`）であるときは、GUI サーバーのテストとタイムライン要素のテストも。変更したファイルが `docs/howto/first-ecu.md` か `docs/howto/operate-signals.md`（`linux-gcc-control` のテストが例と比べる）であるときは、`linux-gcc-control` も |
| コード | `linux-gcc-debug`、`linux-clang-tsan`、`linux-gcc-control`、Windows、GUI（変更したファイルがすべて GUI の外なら除く）、`python` |
| ビルドのファイル（CMake、プリセット、パッチ、`external/`、`.github/`） | さらに `linux-gcc-release`、`linux-gcc-static`、最も古い CMake、ECU プロジェクトのテンプレートを FetchContent でビルドするテスト |
| `main` への push | 上のすべてと `linux-clang-asan` |

プルリクエストでは、`linux-clang-tsan` の失敗はそのチェックに表示されますが、`all checks` を失敗させません。`main` では失敗させます。`python` は、プルリクエストの題と、コミットが 1 つのときはそのコミットのメッセージ（`scripts/check-commits.py --message`）、`.local/` と `build/` 以下のファイルや `CLAUDE.local.md` が変更に入っていないこと（`--changed`）も確かめます。再実行は古い題を読みます。題を直したら、コミットを push するか、プルリクエストを閉じて開き直します。

### ポートとロック

テストは決まったポートで待ち受けません。同じ PC で、別の作業ツリーのビルドのテストと同時に実行できます。

| 待ち受けるもの | ポートの決め方 |
|---|---|
| registry | ポート 0（`RegistryProcess`、`registry_uri` を書かない設定ファイル） |
| Control Server | `--control 0` |
| GUI サーバー | `--port 0` か、e2e が起動のたびに選ぶ空いているポート |
| `multi_host.two_hosts` の 18500 | テストが作る network namespace の中 |

ドキュメントが示す決まったポート（`manual` の 8500 など）が必要なシナリオは、`vehicle_sil_add_scenario_tests()` の `MACHINE_LOCK` で PC 全体のロックを取り、順に実行されます。

- ロックは `/tmp` のファイルへの `flock` です（`cmake/vehicle_sil_machine_lock.sh`。Windows ではロックしません）。
- 待つ間は 1 行表示し、待った時間はそのテストの CTest の `TIMEOUT` に数えます。
- ロックのファイルを開けないとき（別の利用者のファイルなど）は、1 行表示してロックなしで実行します。

### 待ち時間と倍率

起きるはずのことは、`eventually(条件, 上限)`（`test_harness.hpp`）か、起きたことを通知する手段で待ちます。決まった時間だけ眠って「進んだはず」としません。

待ちの上限には倍率が掛かります。

| 項目 | 内容 |
|---|---|
| 倍率の決まり方 | テストを動かすときの環境変数 `VEHICLE_SIL_TEST_TIMEOUT_SCALE`、なければビルドの既定です。 |
| ビルドの既定 | CMake のキャッシュ変数 `VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT` で、既定は 1、TSan のプリセットは 4 です。 |
| 値の書き方 | 数字と小数点 1 つだけで書き、0 より大きくします（`3`、`2.5`）。 |
| ほかの書き方（空白、指数、16 進） | C++ のテスト、GUI サーバーのテスト、e2e が、変数の名前と直し方を表示して止まります。 |
| CI | 3 を与えます。TSan のジョブは与えずに、プリセットの既定を使います（`.github/workflows/ci.yml`）。 |

倍率を掛けるかどうかは、値の種類で決まります。

| 値の種類 | 倍率 | 理由と注意 |
|---|---|---|
| 起きるはずのことを待つ上限。上限として製品に渡す時間（ランチャーが製品の `--timeout-ms` に渡す値など）も同じ | 掛ける | 通るテストは待つものが起きた時点で進むので、倍率を上げても遅くなりません。 |
| 時間切れになることを確認する値のうち、時間切れの前に子プロセスや別のスレッドが何かを終えていなければならないもの（子が 1 行書いてから打ち切られる、など） | 掛ける | 期待の文に出る数も同じ値から作ります。このテストは、倍率の分だけ必ず長くなります。 |
| 「起きないこと」を確認する待ち、周期、時間切れの値そのものを確認するテストの値（`receiver-timeout` の 1500 ms など） | 掛けない | 掛けると、テストが確認することが変わります。 |

上限を書くときのルール:

- 関数に上限を渡すときは `scaled_timeout(20s)`（`tests/unit/test_timeout_scale.hpp`）を通します。GUI サーバーのテストは `conftest.py` の `scaled()`、e2e は `scaled()` です。
- 上限を受け取って中で倍率を掛ける関数（`eventually`、`tests/process/spawn_sleeper.hpp` の待ち、シナリオのランチャーの `--timeout-ms` など）は、そのドキュメントコメントに「scaled」と書きます。
- そうした関数に渡す値には、呼ぶ側で倍率を掛けません。

CTest の `TIMEOUT`:

- ハーネスのテストとシナリオのテストの `TIMEOUT` は、configure のときの倍率（環境変数、なければビルドの既定。切り上げた整数）で伸びます（`cmake/VehicleSilTestHelpers.cmake`）。
- configure の後で倍率を大きくしたら、configure し直します。そのままだと、止まったテストは自分で失敗を報告する前に CTest に止められます。
- ハーネスとランチャーは、そのときに `warning: ... configure the build again ...` の 1 行を表示します。

## テストのラベル

```sh
ctest --preset linux-gcc-release -L integration
# FetchContent テスト（依存ライブラリを一から再ビルドするので時間がかかる）
cmake --preset linux-gcc-release -DVEHICLE_SIL_TEST_FETCHCONTENT=ON
ctest --preset linux-gcc-release -L fetchcontent
```

どのラベルにも共通すること:

- 各プロセスのログは `build/<preset>/tests/` の下の `integration/<シナリオ>/`、`virtual_time/<シナリオ>/`、`lin_cluster/<シナリオ>/`、`vector_xl/integration/<シナリオ>/` に残ります。失敗したときは、ランチャーがすべてのログを出力します。
- テストケースの登録漏れは、テストの実行ファイルごとの `<ラベル>.registration` テストが検出します（実行ファイルの `--list` と登録した名前を比べます）。接頭辞を複数のディレクトリの実行ファイルが共有するときは、`tests/launch` のものが `launch.registration`、ほかが `launch.registration.<ターゲット>` です。
- クロスビルドでは、`CMAKE_CROSSCOMPILING_EMULATOR`（たとえば `wine`）を指定すると、`ctest` がテストの実行ファイルをそれで動かします（`registration` テストも含みます）。Windows の DLL（MinGW の実行時ライブラリ）は、実行ファイルの隣に置くなどして見つかるようにしてください。
- テストプリセットは 8 並列で実行します。`scripts/verify.py` は、CPU がそれより少ない PC では CPU の数だけ並べます。
- 壁時計の所要時間を測るテスト（`virtual_time.*`、`core.virtual_time_*`、LIN の速度のテスト）は単独で実行されます。

### `unit`

コマンドライン解析と、標準出力・標準エラーへの書き出しのテスト（`unit.*`）に付きます。

| ほかに付くもの | 内容 |
|---|---|
| `core`、`lin`、`step`、`can`、`can_bridge`、`eth`、`j1939`、`peer`、`testkit`、`ecu`、`simulation`、`control`、`control_server`、`launch`、`config`、`scenario`、`runner`、`process`、`vector_xl` | 単体テストと境界の検査 |
| `template.get_tools` | テンプレートの `get_tools.py` と、`scripts/package.py` の `template`、`version` を、手元で作ったアーカイブで確認する |
| `param_service.demo` | サンプルの実行のうち、これにだけ付く |

付かないのは、プロセスを起動するシナリオのテスト（`integration`）、サンプルの実行（`examples`）、Python のクライアントのテスト（`python`）、`fetchcontent` です。

### `integration`

`integration.*` は、`sil-kit-registry`、receiver、sender（実時間の `Peer` と J1939 スタック）を別プロセスで起動し、終了コードで判定します。

| シナリオ | 確認すること |
|---|---|
| `address-claim-and-pgn` | Address Claim と PGN の CAN ID とペイロードの一致 |
| `receiver-timeout` | タイムアウト処理 |
| `receiver-detects-wrong-name`、`receiver-detects-wrong-payload` | receiver が、誤った NAME と壊れたペイロードを失敗として報告すること |

このラベルは、プロセスを起動するほかのシナリオのテストにも付きます。

- `virtual_time.*`、`lin_cluster.*`、`launch_cli.*`、`launch_run.*`、`ecu_main.*`、`vector_xl.integration.*`、`control_server.launcher`（内容は、それぞれのラベルの項目）
- `template.check_and_run`: テンプレートの ECU を本体のビルドで作り、ランチャーで照合、実行、シナリオが通ることを確認します。照合と実行の出力を、ガイド「最初の ECU を作る」と照合します。

## ドキュメントの例とテスト

ドキュメントの中の例（設定ファイル、コード、コマンドの出力）のうち、`<!-- ... -->` の印の行の直後にあるコードブロックは、テストが実物と照合します。印とブロックを変えたときに失敗するテストは、次のとおりです。

- テストが照合するのは英語のページです（`tests/CMakeLists.txt` の `VEHICLE_SIL_TESTED_DOCS` が `docs/`）。日本語のページ `docs/ja/` は、`scripts/check-docs.py` がコードブロックを照合して守ります（英語と日本語でブロックの数、順、言語のタグが同じ。日本語を含まないブロックはバイト単位で同じ）。
- ページそのものを見るテスト（`tests/schema/test_schemas.py`、GUI サーバーのテスト、`gui/client/pack.mjs`、`scripts/package.py version`）は、両方の言語のページを読みます。

### リファレンスの例

| ドキュメント | 印、ブロック | 照合するテスト |
|---|---|---|
| [simulation-config.md](simulation-config.md)、[simulation-check.md](simulation-check.md)、[simulation-run.md](simulation-run.md)、[simulation-events.md](simulation-events.md) | `check` の印 | `launch_cli.docs` |
| 同上 | `run` の印 | `launch_run.docs` |
| [simulation-events.md](simulation-events.md) | 「プログラムから使う」のイベントの例 | `launch_run.events` |
| [simulation-config.md](simulation-config.md) | 「participant を自分で起動する」の例 | `launch_run.manual` |
| [ecu-executable.md](ecu-executable.md) | `<!-- vehicle_sil_engine_ecu --describe -->` | `ecu_main.describe` |
| [scenario.md](scenario.md) | `<!-- vehicle_sil_launch run simulation.yaml --scenario ... -->` | `launch_run.scenario` |
| [scenario.md](scenario.md) | `<!-- throttle_mistakes.yaml -->`、`<!-- throttle_response.yaml -->` | `launch_run.scenario` |
| ドキュメントの YAML と、キーの表 | 印なし | `schema.files_and_launcher`（`tests/schema/test_schemas.py`） |

- `launch_cli.docs` と `launch_run.docs` は、vehicle-sil のバージョンを除いて、バイト単位で照合します。
- `launch_run.events` は、置き換えの後にバイト単位で照合します。
- エラーのある例（`two-engines.yaml`、`engine-alone.yaml`、シナリオの `throttle_mistakes.yaml`、`throttle_response.yaml`）は、ドキュメントのブロックが元で、テストが自分の作業ディレクトリに書き出します。
- 作業ディレクトリは `build/<preset>/tests/launch/` の下で、例のコピーと ECU へのリンクを置きます。動く例のディレクトリ（`build/<preset>/examples/ecu_main/`）には、エラーのある例を置きません。

### ガイドの例

| ガイド | 印 | 照合するテスト |
|---|---|---|
| [howto/first-ecu.md](howto/first-ecu.md) | `<!-- examples/first_ecu/... -->`、`<!-- first ECU: ... -->`、`<!-- cooler_ecu --describe: an excerpt -->` | `template.first_ecu`、`control_server.example_first_ecu` |
| 同上 | テンプレートだけの出力のブロック | `template.check_and_run` |
| [howto/scenario-tests.md](howto/scenario-tests.md) | `<!-- examples/scenario_tests/... -->`、`<!-- examples/ecu_main/scenarios/... -->`、`<!-- scenario guide: ... -->` | `launch_run.scenario_guide` |
| [howto/integrate-ecus.md](howto/integrate-ecus.md) | `<!-- examples/integrate_ecus/... -->`、`<!-- integrate ECUs: ... -->` | `launch_docs.integrate_ecus` |
| [howto/multi-pc.md](howto/multi-pc.md) | `<!-- examples/ecu_main/simulation.two-pcs.yaml -->`、`<!-- multi-pc guide: ... -->` | `multi_host.docs` |
| [howto/operate-signals.md](howto/operate-signals.md) | `<!-- examples/launch_control/... -->`、`<!-- vehicle_sil_control ... -->`、`<!-- standard input of run --start-paused --stdin-commands: ... -->`、`<!-- examples/ecu_main/dashboard/main.cpp: an excerpt -->` | `control_server.guide_operate_signals` |
| [howto/](howto/) のガイド全部 | リファレンスと同じ `check` の印、`run` の印 | `launch_cli.docs`、`launch_run.docs` |

- ガイドにある `check` と `run` の出力のブロックは、`docs/` の直下のドキュメント（simulation-*.md と [scenario.md](scenario.md)）の、同じ印の例のコピーです。`launch_cli.docs` と `launch_run.docs` は、ガイドのブロックがコピーのとおりかを照合します。
- howto/scenario-tests.md の `<!-- vehicle_sil_launch run ... -->` の 1 つは、scenario.md の同じブロックのコピーで、`launch_run.scenario` も照合します。
- 例のファイルの場所とテストの内容は、[作業対象から探す](code-map.md#作業対象から探す) の各領域ページにある `examples/` の項目にあります。

### 照合していないもの

次のものはテストが照合しないので、手で合わせます。

| ドキュメント | 照合していないもの |
|---|---|
| [howto/](howto/) のガイド | 印のないコードの引用 |
| [howto/first-ecu.md](howto/first-ecu.md) の「J1939 で通信させる」 | テンプレートのコードを、行の番号で引いている所 |
| [howto/first-ecu.md](howto/first-ecu.md) の「デバッガーから動かす」 | `launch_run.manual` の例と同じ標準エラーの行と結果の表を、`Cooler` の名前で載せている所 |
| [simulation-check.md](simulation-check.md) | 「照合」の `code` の表 |
| [simulation-events.md](simulation-events.md) | `features` の表と、`events.hpp` の `kFeatures` |
| [ethernet-bridge.md](ethernet-bridge.md) | ブリッジの出力とエラーの文のコピー |

## 境界を検査するテスト

次のテストは、ソースの `#include` などを読んで、層の境界を検査します。内部のヘッダーや SIL Kit のヘッダーを追加すると失敗します。

| テスト | 検査すること |
|---|---|
| `j1939_parts.boundary` | `libs/j1939/parts` が、エンジンの公開 API とツールキットだけを使い、`vehicle_sil::j1939` とほかのコンポーネントだけをリンクする |
| `local.boundary`、`testkit.boundary`、`peer.boundary`、`ecu.boundary`、`simulation.boundary`、`j1939_codec.boundary`、`param_service.boundary`、`j1939_spy.boundary` | そのディレクトリのソースが、エンジンの内部を使わない（`cmake/VehicleSilCheckJ1939Parts.cmake`） |
| `ecu.no_silkit_boundary`、`ecu.simulation_headers_boundary`、`lin.boundary`、`step.boundary`、`eth.boundary`、`signal.boundary` | `libs/ecu`、`libs/simulation/include`、`libs/lin`、`libs/step`、`libs/eth`、`libs/signal` が、SIL Kit のヘッダーを直接にも間接にも含まない（`cmake/VehicleSilCheckNoSilKit.cmake`） |
| `peer.controllers_are_made_on_the_simulation_name` | `libs/` で SIL Kit のバスのコントローラーを作るのは `libs/peer/src/peer.cpp` だけで、どの呼び出しも `simulation_name()` を通した名前のネットワークに作る（`cmake/VehicleSilCheckControllerNetworks.cmake`） |
| `core.silkit_configuration_defaults` | `libs/`、`tools/`、`bridges/`、`tests/`（`tests/core/configuration_fixtures` を除く）が文字列から作る SIL Kit の設定は、vehicle-sil の既定（`default_participant_configuration()` など）を通る（`cmake/VehicleSilCheckSilKitConfiguration.cmake`） |
| `launch.described_networks_are_read_in_one_place` | `libs/config`、`libs/launch`、`tools/launch` で自己申告のネットワーク（`description.networks`。メンバーへのポインター `::networks` も）を読むのは `libs/config/src/check_networks.cpp`（対応を当てる `seen_networks()`）、`libs/launch/src/format/reported_networks.cpp`（JSON の `reported` を書く所）、`libs/config/src/describe_read.cpp` だけ（`cmake/VehicleSilCheckDescribedNetworks.cmake`。設定の `config.networks` は数えない） |
| `launch.request_boundary`、`launch.probe_boundary`、`launch.format_boundary`、`launch.endpoints_boundary` | `libs/launch/src/request/`、`probe/`、`format/`、`endpoints/` のファイルと、その部分に属する公開ヘッダーが、その部分が届いてはいけないヘッダー（部分ごとの一覧は [architecture.md](architecture.md) の「`libs/launch` の層」。SIL Kit は `endpoints/` のほかの 3 つ。`endpoints/` の Control Server だけが例外）を、直接にも include の連なりを通しても含まない。落ちたときは連なりを示す（`cmake/VehicleSilCheckIncludes.cmake` の `DIR`、`FORBIDDEN_PREFIXES`、`ONLY_PREFIXES`。ディレクトリに足したファイルは一覧なしで見る。`usecases/` に制限はない） |
| `launch.parts_include_check_rejects_a_forbidden_include` | 上の検査が、ファイルの隣のヘッダーを通して、またほかの部分の私的なヘッダーを通して、禁止されたヘッダーに届く見本で落ち、連なりを示す |
| `launch.config_files_do_not_reach_the_scenario` | `libs/launch` の `connections.hpp` が、`vehicle_sil/scenario/` 以下のヘッダーを、直接にも include の連なりを通しても含まない。落ちたときは連なりを示す（`cmake/VehicleSilCheckIncludes.cmake` の `FORBIDDEN_PREFIXES`。要求は `launch.request_boundary` が守る） |
| `config.no_silkit_boundary` | `libs/config` が、SIL Kit のヘッダーを直接にも間接にも含まず、core のヘッダーは `vehicle_sil::sim_base` のものだけを含む（`cmake/VehicleSilCheckNoSilKit.cmake`） |
| `config.includes_no_runtime` | `libs/config` のすべてのヘッダーとソースが、`vehicle_sil/launch/`、`vehicle_sil/scenario/`、`vehicle_sil/runner/` 以下のヘッダー、`child_process.hpp`、`process.hpp` を、直接にも include の連なりを通しても含まない。落ちたときは連なりを示す（`cmake/VehicleSilCheckIncludes.cmake` の `DIR` と `FORBIDDEN_PREFIXES`。ディレクトリに足したファイルは、一覧なしで検査される） |
| `config.include_check_rejects_a_forbidden_include` | 上の検査が、それらを含む見本のディレクトリで落ち、連なりを示す |
| `scenario.no_silkit_boundary` | `libs/scenario` が、SIL Kit のヘッダーを直接にも間接にも含まず、core のヘッダーは `vehicle_sil::sim_base` のものだけを含む（`cmake/VehicleSilCheckNoSilKit.cmake`） |
| `scenario.includes_no_runtime` | `libs/scenario` のすべてのヘッダーとソースが、`vehicle_sil/launch/` と `vehicle_sil/runner/` 以下のヘッダー、`child_process.hpp`、`process.hpp` を、直接にも include の連なりを通しても含まない。落ちたときは連なりを示す（`cmake/VehicleSilCheckIncludes.cmake` の `DIR` と `FORBIDDEN_PREFIXES`） |
| `scenario.include_check_rejects_a_forbidden_include` | 上の検査が、それらを含む見本のディレクトリで落ち、連なりを示す |
| `runner.public_headers_need_no_silkit` | `libs/runner/include` 以下のヘッダー（`vehicle_sil/runner/`、`detail/` を含む）が、SIL Kit のヘッダーと core の実行時のヘッダーを、直接にも間接にも含まない。実行の SIL Kit の部分は `RunnerImpl` の後ろにある（`cmake/VehicleSilCheckNoSilKit.cmake`。runner は `core` をリンクするので、リンクでは分からない） |
| `runner.includes_no_launcher` | `libs/runner` のすべてのヘッダーとソースが、`vehicle_sil/launch/` 以下のヘッダーを、直接にも include の連なりを通しても含まない（`cmake/VehicleSilCheckIncludes.cmake` の `DIR`、`FORBIDDEN_PREFIXES`、`ONLY_PREFIXES`。runner 自身が、この検査が純粋な層に禁じるヘッダーを使うため） |
| `runner.pure_files_include_no_runtime` | `libs/runner` の `command.hpp`、`traffic.hpp`、`control_rules.*` が、SIL Kit、process、run、events、time_commands、stop_requests のヘッダーを、直接にも include の連なりを通しても含まない。落ちたときは連なりを示す（`cmake/VehicleSilCheckIncludes.cmake`） |
| `runner.include_check_rejects_a_forbidden_include` | 上の検査が、`vehicle_sil/launch/` を含む見本のディレクトリで落ち、連なりを示す |
| `j1939_codec.independent_of_the_engine` | `codec.hpp` が含む vehicle-sil のヘッダーは `j1939/types.hpp`（値の型）だけで、`Network` と `Node` を含まない |
| `eth_link.boundary` | `libs/eth_link` が、SIL Kit のヘッダーを直接にも間接にも含まない（`cmake/VehicleSilCheckNoSilKit.cmake`） |
