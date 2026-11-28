# ECU のプロジェクト（vehicle-sil のテンプレート）

[English](README.md)

[vehicle-sil](https://github.com/rahanahu/vehicle-sil) で ECU を作るプロジェクトのテンプレートです。2 つの ECU をビルドし、1 つのシミュレーションとして実行し、ブラウザの GUI で値を操作するところまでを、この README の手順で進めます。その後、ECU を自分のものに書き換えます。

## 前提

| 必要なもの | Linux | Windows |
|---|---|---|
| コンパイラ | GCC 12 以上か Clang（C++20 に対応したもの） | MSYS2 UCRT64 の MinGW-w64 GCC |
| CMake 3.21 以上、Ninja、Git | ディストリビューションのパッケージ | MSYS2 のパッケージ |
| Python 3.9 以上（`get_tools.py` の実行） | ディストリビューションのパッケージ | MSYS2 の Python |
| Python 3.11 以上（GUI） | [uv](https://docs.astral.sh/uv/) があれば、足りない Python とパッケージを uv がインストールします。 | uv（MSYS2 の `mingw-w64-ucrt-x86_64-uv`）が必要です。 |

Windows では、MSYS2 の「UCRT64」シェルで作業します。MSVC（Visual Studio）はテンプレート ECU のビルドに対して公式に非対応です。必要なものは、次のコマンドでインストールします。

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-uv
```

Windows の GUI は、uv がダウンロードする Python（ビルド済み wheel 対応）で動きます。MSYS2 の Python には、GUI のパッケージをインストールできません。なお、ほかのコンパイラ（MSVC を含む）で作った外部の実行ファイルやツールも、SIL Kit や gRPC を介してプロセス境界越しに接続できます。

WSL では、`/mnt/<drive>` ではなく WSL の Linux ファイルシステム（`~/` 配下など）に置いてください（9p 上のビルドは遅くなります）。また、Windows と WSL で同じ作業ツリーや `node_modules` を共有しないでください。

コマンドは、すべてテンプレートを展開したディレクトリ（`simulation.yaml` のある所）で実行します。`vehicle_sil_launch` と `vehicle-sil-gui` は、手順 2 で `PATH` に追加したものを使います。

## 全体の流れ

| 手順 | すること | できるもの |
|---|---|---|
| 1 | テンプレートを展開する | 自分のプロジェクトのディレクトリ |
| 2 | ビルド済みのツールをダウンロードする | `tools/` の下の `vehicle_sil_launch` と `vehicle-sil-gui` |
| 3 | ビルドして実行する | `bin/` の 2 つの ECU と、照合、実行、シナリオの結果 |
| 4 | ブラウザの GUI で操作する | 変数を見て書き込める画面 |

## 手順 1: テンプレートを展開する

vehicle-sil の[リリースのページ](https://github.com/rahanahu/vehicle-sil/releases)から `vehicle-sil-template-0.6.0.zip` をダウンロードして展開し、ディレクトリの名前を自分のプロジェクトの名前にします。Git で管理するなら、そこで `git init` を実行します（`.gitignore` が `build/`、`bin/`、`tools/` を除外します）。

テンプレートとツールは、リリースしたバージョンにだけあります。vehicle-sil のリポジトリの `template/` をコピーしても同じものですが、そのコピーの `get_tools.py` には、ツールの SHA-256 が入っていません（手順 2 で `--sha256` を渡します）。

## 手順 2: ビルド済みのツールをダウンロードする

```sh
python3 get_tools.py     # Windows（UCRT64 シェル）: python get_tools.py
# 続けて、表示された PATH の行を実行する（コマンド プロンプトか PowerShell から実行したときは、自分のシェルの名前の行）
```

`get_tools.py` は、次の順に処理します。

| 順 | すること |
|---|---|
| 1 | リリースのページから、自分の OS のツールのアーカイブをダウンロードします。 |
| 2 | アーカイブの SHA-256 を確認します。 |
| 3 | `tools/vehicle-sil-tools-0.6.0-<OS>/` に展開します。 |
| 4 | ツールに入っている JSON Schema を `schema/` にコピーします。 |
| 5 | `bin` を `PATH` に追加するコマンドを表示します。 |

ダウンロードする URL とバージョンは、`get_tools.py` の `VERSION` と `URL` に書いてあります。最新のバージョンを自動でダウンロードすることはありません。

`put their bin directory on PATH` の後に表示された行を実行します。この行は、シェルを開くたびに実行します。Linux と MSYS2 のシェルでは、`export PATH=...` の 1 行が表示されます。Windows でコマンド プロンプトか PowerShell から実行したときは、その 2 つの行が表示されるので、自分のシェルの名前の行を実行します。

### 確認

`vehicle_sil_launch version` がバージョンを表示すれば、`PATH` に追加できています。ツールの内容は、展開したディレクトリの `README.md` にあります。

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `may not be published yet` と表示して止まる | この README のバージョンが、まだリリースされていません。 | そのバージョンのツールのアーカイブがファイルで手元にあれば（同僚からもらったものなど）、`python3 get_tools.py --archive <ファイル>` を実行します。なければ、vehicle-sil のリポジトリをクローンしてビルドし（vehicle-sil の README の「リポジトリからビルドする」）、そのビルドの `vehicle_sil_launch` を使います。 |
| ネットワークからダウンロードできない | PC からリリースのページに接続できません。 | ブラウザで `vehicle-sil-tools-0.6.0-linux-x86_64.tar.gz`（Windows は `-windows-x86_64.zip`）をダウンロードし、`python3 get_tools.py --archive <ダウンロードしたファイル>` を実行します（SHA-256 の確認は同じです）。 |
| `this copy of the template has no SHA-256` と表示して止まる | リポジトリの `template/` をコピーしたので、SHA-256 が入っていません。 | リリースのページの `SHA256SUMS` にある値を `--sha256` で渡します。 |

`get_tools.py` を使わずに手で展開するときは、リリースのページの `SHA256SUMS` と照合してから展開し、その中の `bin` を `PATH` に追加します。

## 手順 3: ビルドして実行する

```sh
cmake --preset linux             # Windows: cmake --preset windows
cmake --build --preset linux     # Windows: cmake --build --preset windows
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml --stop-at-ms 1000
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml
```

| コマンド | すること |
|---|---|
| `cmake --preset` | vehicle-sil と SIL Kit をダウンロードします。 |
| `cmake --build` | 2 つの ECU を `bin/` にビルドします（最初の 1 回だけ、SIL Kit のコンパイルに数分かかります）。 |
| `check` | 各 ECU を `--describe` で起動し、その出力と設定ファイルを照合します。 |
| `run` | registry と各 ECU を起動して、指定した停止時刻（1000 ms）まで実行します。 |
| `run --scenario` | シナリオの手順どおりに値を設定して確認します。 |

### 確認

| コマンド | 成功したときに見えるもの |
|---|---|
| `check` | 最後の行が `passed: 0 errors, 0 warnings, 0 notes` です。 |
| `run` | `ended` の列が、2 つの ECU とも `exit code 0` です。 |
| `run --scenario` | `result` の列がすべて `ok` で、終了コードが 0 です。 |

`check` と `run` の出力の全体は、ガイド「[最初の ECU を作る](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/ja/howto/first-ecu.md)」の手順 1 にあります。

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `vehicle_sil_launch` が見つからない | 新しいシェルで、`PATH` への追加を実行していません。 | `python3 get_tools.py` をもう一度実行し、表示された、自分のシェルの PATH の行を実行します。 |
| Windows で、ECU が何も表示せずに終了する（`0xC0000135` / `0xC0000139`） | 必要な DLL が見つからない（`0xC0000135`）、または互換性のない MinGW のランタイム DLL が読み込まれた（`0xC0000139`、実行ファイルの隣に残った古い DLL や `PATH` 上の不一致 DLL など）。 | 非静的ビルドでは `CMakeLists.txt` の `vehicle_sil_copy_runtime_dlls(<target>)` が `SilKit.dll` とツールチェーンの MinGW ランタイム DLL を各 ECU の隣にコピーします。その呼び出しがあるか確認してください（または `-DVEHICLE_SIL_STATIC=ON` でビルドします）。Windows は通常 `PATH` より先に実行ファイルの隣の DLL を読み込むため、`0xC0000139` の場合はまず `bin/<ecu>.exe` の隣にある DLL がビルドに使ったツールチェーンと一致しているか確認し、必要なら同じツールチェーンでクリーンリビルドして `vehicle_sil_copy_runtime_dlls()` に一致する DLL をコピーし直させてください。MSYS2 UCRT64 の `ldd bin/<ecu>.exe` はインポートされる DLL 一覧を表示し、PowerShell/cmd の `where.exe <dll>` は `PATH` 上の候補 DLL を確認するだけのツールです（Windows が実際にどの DLL を読み込んだかはわかりません）。 |
| `check` が `version-mismatch` を表示する | ECU とツールで、vehicle-sil のバージョンが違います。 | 下の「vehicle-sil のバージョンを上げる」で、バージョンをそろえます。 |

ツールの `vehicle_sil_launch` は、DLL が不要な 1 つの実行ファイルです（ツールの `bin` に DLL はありません）。

## 手順 4: ブラウザの GUI で操作する

```sh
vehicle-sil-gui simulation.live.yaml
```

ブラウザで、設定ファイルを照合し、シミュレーションを実行し、変数を見て書き込む画面を開きます。終了するときは Ctrl+C を押します。

`simulation.live.yaml` は、同じ 2 つの ECU と画面の配置の設定です。GUI で等速を選ぶと、停止するまで値を見ながら操作できます。

## 入っているファイル

| ファイル | 内容 |
|---|---|
| `engine/main.cpp` | エンジンの ECU です（入力の変数 `engine.throttle`、出力の変数 `engine.speed`、回転数を J1939 で 100 ms ごとに送信）。 |
| `dashboard/main.cpp` | 表示の ECU です（回転数を J1939 で受信して、出力の変数 `dashboard.engine_speed` に反映）。 |
| `engine_speed.hpp` | 2 つの ECU が共有するメッセージの定義です（PGN とデータの形式）。 |
| `CMakeLists.txt`、`CMakePresets.json` | vehicle-sil を FetchContent で取り込み、実行ファイルを `bin/` にビルドします（`GIT_TAG` が vehicle-sil のバージョン）。 |
| `simulation.yaml` | 2 つの ECU を 1 つのシミュレーションにする設定ファイルです。 |
| `scenarios/throttle_step.yaml` | スロットルを踏んで、回転数と表示を確認するシナリオです。 |
| `simulation.dashboard.yaml` | GUI の「変数」の画面の配置です。 |
| `simulation.live.yaml`、`simulation.live.dashboard.yaml` | ECU を見ながら操作するための設定ファイルと、その画面の配置です。 |
| `schema/` | 上の 3 種類の YAML の JSON Schema です。 |
| `get_tools.py` | ツール（`vehicle_sil_launch` と GUI）を、バージョンと SHA-256 を確認して `tools/` にダウンロードします。 |

`schema/` があるので、YAML の言語サーバーを使うエディターで、キーの補完と説明、形式の検査が使えます。各 YAML の 1 行目の `# yaml-language-server: $schema=...` がスキーマを指しています。自分で追加する YAML にも、同じ 1 行を書きます。

`schema/` のファイルは手で編集しません。`get_tools.py` が、ツールに入っているスキーマで上書きするためです。上書きするのは同じ名前のファイルだけで、自分で追加したファイルはそのまま残ります。

## 自分の ECU にする

| したいこと | 変える所 |
|---|---|
| ECU の中身を変える | `engine/main.cpp` のクラスの中身（ネットワーク、J1939 のノード、変数、ステップごとの処理）を書き換えます。 |
| ECU の名前を変える | `main()` の `ecu_main<...>(..., {.name = "Engine", ...})` を変えます。 |
| ECU を追加する | `CMakeLists.txt` に `add_executable` から `vehicle_sil_copy_runtime_dlls` までの 4 行を追加し、`simulation.yaml` の `participants` に追加します。 |
| ネットワークの名前（`J1939_CAN`）を変える | ECU のコードと `simulation.yaml` の `networks` で、同じ名前にします。 |

Manufacturer Code の 0x7F0 は仮の値です。

ECU を 1 つ追加する手順の全体は、ガイド「[最初の ECU を作る](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/ja/howto/first-ecu.md)」にあります。

## 1 つの実行ファイルで配布する

ECU の実行ファイルを、ほかのファイル（`libSilKit.so`、`SilKit.dll`、MinGW の DLL）なしで人に渡したいときは、`VEHICLE_SIL_STATIC` を付けて configure し直します。

```sh
cmake --preset linux -DVEHICLE_SIL_STATIC=ON      # Windows: cmake --preset windows -DVEHICLE_SIL_STATIC=ON
cmake --build --preset linux                      # Windows: cmake --build --preset windows
```

SIL Kit と C++ のランタイムを、実行ファイルに静的にリンクします（Linux の glibc は共有ライブラリのままです）。Linux では、静的な libstdc++ が必要です（Fedora は `libstdc++-static`）。元に戻すときは、`-DVEHICLE_SIL_STATIC=OFF` で configure し直します。

人に渡すときは、実行ファイルの中に入ったもののライセンスの表示を添えます。添えるものの一覧は、vehicle-sil の [docs/build.md](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/ja/build.md) の「配布するときに添えるライセンスの表示」にあります。ツールの `licenses/sil-kit/`（Windows は `licenses/msys2/` も）に、そのまま添えられるファイルがあります。

## vehicle-sil のバージョンを上げる

ECU とツールは、同じバージョンの vehicle-sil にそろえます。バージョンが違うと、`check` が `version-mismatch` を表示します。

| 順 | すること |
|---|---|
| 1 | 新しいバージョンのテンプレートから、`get_tools.py` と、`CMakeLists.txt` の `GIT_TAG` の行をコピーします。 |
| 2 | `python3 get_tools.py` をもう一度実行します。 |
| 3 | `cmake --build --preset linux` でビルドし直します（Windows は `windows`）。 |
| 4 | 形式 1 の設定ファイルはローカルのエディターで開いて `vehicle_sil_simulation: 2` に変更します。`stop_at_ms`、`speed`、`late_tolerance_ms` は実行時の引数か要求ファイルへ移します。`start_timeout_ms` と `stop_timeout_ms` は `--participant-start-timeout-ms` と `--participant-stop-timeout-ms` へ移します。刻みを固定する場合は `time_step_ms` を設定ファイルに残します。 |

`get_tools.py` は、ツールに入っている JSON Schema を `schema/` にコピーし直し、コピーしたファイルを 1 行で表示します。これで、エディターの補完と検査が、新しいバージョンのキーに対応します。変更点と対応のしかたは、vehicle-sil の `CHANGELOG.md` にあります。

## 次に読むもの

- ECU を 1 つ追加して、Python から変数を読み書きする → [最初の ECU を作る](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/ja/howto/first-ecu.md)（J1939 での通信と、デバッガーからの起動も）
- J1939 のノードを書く → [J1939 のノードの作り方](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/ja/j1939-guide.md)、[型付きメッセージ](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/ja/j1939-codec.md)
- 設定ファイルとコマンドを調べる → [設定ファイル](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/ja/simulation-config.md)、[照合](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/ja/simulation-check.md)、[実行](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/ja/simulation-run.md)、[シナリオ](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/ja/scenario.md)、[ブラウザの GUI](https://github.com/rahanahu/vehicle-sil/blob/v0.6.0/docs/ja/gui.md)

## ライセンス

このテンプレートのファイルは MIT-0（MIT No Attribution、`LICENSE`）です。コピーして作ったプロジェクトに、このテンプレートの著作権やライセンスの表示を残す必要はありません。テンプレートが取り込む vehicle-sil 本体とその依存ライブラリは、それぞれのライセンスのままです（vehicle-sil は MIT。依存ライブラリは vehicle-sil の `THIRD_PARTY_NOTICES.md`）。
