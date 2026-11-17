# ビルド・配布・依存ライブラリ・ライブラリの追加

[English](../code-map-build.md)

<!-- 書き方: 項目は、同じ形の短いブロックにします（小見出し、1 行に 1 つの事実の箇条書き、セルが 1 文の表）。1 つのセルや 1 つの箇条書きに複数の事実を詰めません。1 行が長くなり、別々の変更が同じ行に当たるためです。 -->

この分野を変更するときの地図です。全体の入口は [code-map.md](code-map.md)、テストの共通ルールは [code-map-testing.md](code-map-testing.md) にあります。

関連する仕様と変更範囲: [build.md](build.md) / [dependencies.md](dependencies.md) / [テストの開発](code-map-testing.md) / [ECU・Peer・ローカル実行・Simulation](code-map-execution.md) / [基盤・ステップ・信号・SIL Kit の既定](code-map-core.md)

## テストのラベル

### `fetchcontent`

既定では無効です。

- このリポジトリのコミットを FetchContent で取り込み、利用例をビルドして実行します。
- `fetchcontent.template`: テンプレートを別のディレクトリにコピーし、いまの作業ツリーを FetchContent で取り込んでビルドし、照合と実行とシナリオが通ることを確認します。

## ディレクトリごとのコードとテスト

### `examples/fetchcontent_consumer`（FetchContent での利用）

- テスト: `tests/fetchcontent`（`VEHICLE_SIL_TEST_FETCHCONTENT=ON` のときだけ）
- `ctest -R`: `^fetchcontent\.`

### `template`（ECU のプロジェクトのテンプレート）、`scripts/package.py`（ビルド済みのツールとテンプレートのアーカイブ）

| 名前 | 内容 |
|---|---|
| `template/get_tools.py` | ツールをダウンロードする |

| テスト | 内容 |
|---|---|
| `tests/template` | テンプレートを本体のビルドに入れて、照合と実行をする |
| 同上 | `get_tools.py` と、`package.py` の `template`、`version` を、手元のアーカイブで確認する |
| `fetchcontent.template` | テンプレートをコピーして、FetchContent で取り込む（opt-in） |
| `scripts/package.py verify` | ツールのアーカイブを、利用者の手順で確認する |

- `ctest -R`: `^template\.`

### `scripts/`

開発と CI のスクリプトです。一覧は [AGENTS.md](../../AGENTS.md#verification-and-scripts) にあります。

| テスト | 対象 |
|---|---|
| `scripts/verify-python.py` | 各スクリプトの `--help` |
| `tests/scripts/test_run_gui.py` | `scripts/run-gui.py`: ランチャーの選び方、足りないものごとの文言 |
| `tests/scripts/test_job_summary.py` | `scripts/job_summary.py`: GitHub Actions の job の Summary に書く中身、書かないとき |
| `tests/scripts/test_check_docs.py` | `scripts/check-docs.py`: 英語のページと日本語のページの対（見出し、コードブロック、リンク）、英語版がまだないページ |
| `tests/scripts/test_verify.py` | `scripts/verify.py --summarize`: 要約に出す、落ちたテストの出力の行（先頭の行と、その先の FAIL や Error を含む行） |
| `tests/template/test_get_tools.py` | `package.py template`、`version` |

- `ctest -R`: `^template\.get_tools`、`^scripts\.`

### `patches/sil-kit`

SIL Kit に当てるパッチです。configure のときに、ビルドのディレクトリのコピーに当てます（`cmake/VehicleSilDependencies.cmake`）。書き方は [patches/README.md](../../patches/README.md) にあります。

- テスト: configure が通ること

### `cmake/`

| 内容 | 場所 |
|---|---|
| 依存の取り込み | `VehicleSilDependencies.cmake`、`VehicleSilGrpc.cmake` |
| テストの登録 | `VehicleSilTestHelpers.cmake` |
| 境界の検査 | `VehicleSilCheck*.cmake` |

## 依存の向き

矢印の意味と全体の境界テストは [code-map.md](code-map.md#依存の向き) にあります。

### ほかの実行ファイル

どれも、非公開で `vehicle_sil_cli`（`tools/common`、ヘッダーのみ）をリンクします。

| 実行ファイル | ほかにリンクするもの |
|---|---|
| `vehicle_sil_sim_controller` | `core` |
| `vehicle_sil_can_bus_simulator` | `can_netsim` |
| `vehicle_sil_launch` | `launch` と `process` |
| `vehicle_sil_launch`（Windows） | `shell32` も |
| `vehicle_sil_launch`（`VEHICLE_SIL_BUILD_CONTROL_SERVER=ON`） | `control_grpc` と `core` も |

## 変更するときに一緒に直すもの

### バージョンの番号

変える場所:

- ルートの `CMakeLists.txt` の `project(... VERSION)`

合わせて直す所は、[CHANGELOG.md](../../CHANGELOG.md) に書いてあります。

- `pyproject.toml`、`gui/server/pyproject.toml`
- `template/get_tools.py`
- `template/CMakeLists.txt` の `GIT_TAG`
- `template/README.md` のバージョン
- `CHANGELOG.md` の見出し `## [<version>] - Unreleased`

リリースのとき:

- タグを付けるコミットで、見出しの `Unreleased` をリリースの日付（`YYYY-MM-DD`）にします。
- リリースノートは、`.github/workflows/release.yml` が、`CHANGELOG.md` のこのバージョンの節から切り出します。

テスト:

- `scripts/package.py version`（と `template.get_tools`）が、不整合を見つけます。
- `--expect v<version>`（リリースのワークフロー）は、日付のない見出しも拒否します。

### `VEHICLE_SIL_STATIC`

変える場所:

- `cmake/VehicleSilDependencies.cmake` の SIL Kit の取り込みと、`SilKit::SilKit` に付ける静的なリンクの指定

合わせて直すドキュメント:

- [build.md](build.md): `VEHICLE_SIL_STATIC` の節（配布するときに添えるライセンスの表示も、この節にある）
- [dependencies.md](dependencies.md) の「取り込み方」の表

関係するもの:

- プリセット `linux-gcc-static`、`windows-mingw-static`（CI は `linux-gcc-static` だけ）
- ツールのアーカイブのランチャーも、これで作ります。

### ツールのアーカイブの中身

変える場所:

- `scripts/package.py tools`

合わせて直すドキュメントとファイル:

- [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md): 同じ中身を書いた所（同梱する物と、ライセンスの文の場所）
- [build.md](build.md) の「ビルド済みのツール」
- `.github/workflows/release.yml`

gRPC のバージョンを変えたら、`package.py` の `GRPC_LICENSES` と、THIRD_PARTY_NOTICES.md の表のライセンスの名前を直します。gRPC が取り込む物のライセンスは、バージョンで変わるためです。

### SIL Kit の API と上流の挙動への前提

変える場所:

- 使う SIL Kit の API
- 上流の挙動への前提

合わせて直すドキュメント:

- [dependencies.md](dependencies.md) の「使っている API」と「実装が前提にしている上流の挙動」

### `libs/` にライブラリを追加する

追加する場所:

- ルートの `CMakeLists.txt` の `add_subdirectory`

合わせて直すドキュメント:

- [architecture.md](architecture.md) の「ディレクトリとターゲット」
