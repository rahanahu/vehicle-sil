# ECU のプロジェクト（vehicle-sil のひな形）

[vehicle-sil](https://github.com/rahanahu/vehicle-sil) で ECU を作るプロジェクトのひな形です。2 つの ECU、それをまとめて動かす構成ファイル、シナリオ、ブラウザの GUI のダッシュボードが入っています。これを写して、自分の ECU に書き換えます。

| ファイル | 中身 |
|---|---|
| `engine/main.cpp` | エンジンの ECU。入力の変数 `engine.throttle`（スロットル）、出力の変数 `engine.speed`（回転数）、回転数を J1939 で 100 ms ごとに送る |
| `dashboard/main.cpp` | 表示の ECU。回転数を J1939 で受けて、出力の変数 `dashboard.engine_speed` に出す |
| `engine_speed.hpp` | 2 つの ECU が取り決めるメッセージ（PGN とデータの形） |
| `CMakeLists.txt`、`CMakePresets.json` | ビルド。vehicle-sil を FetchContent で取り込む（`GIT_TAG` が vehicle-sil の版）。実行ファイルは `bin/` にできる |
| `simulation.yaml` | 2 つの ECU を 1 つのシミュレーションにする構成ファイル |
| `scenarios/throttle_step.yaml` | スロットルを踏んで回転数と表示を確かめるシナリオ |
| `simulation.dashboard.yaml` | GUI の「変数」の画面の並べ方 |
| `simulation.live.yaml`、`simulation.live.dashboard.yaml` | 同じ 2 つの ECU を、等速で、止めるまで動かす構成ファイル（`stop_at_ms` なし、`speed: 1`）と、その画面の並べ方。GUI で値を見ながら触るときに使う |
| `get_tools.py` | 道具（`vehicle_sil_launch` と GUI）を、版と SHA-256 を確かめて `tools/` に取ってくる |

## 要るもの

| | Linux | Windows |
|---|---|---|
| コンパイラ | GCC 12 以上か Clang（C++20 に対応したもの） | MSYS2 UCRT64 の MinGW-w64 GCC |
| ビルドの道具 | CMake 3.21 以上、Ninja、Git | 同左（MSYS2 のパッケージ） |
| `get_tools.py` | Python 3.9 以上 | 同左（MSYS2 の Python） |
| GUI | Python 3.11 以上（[uv](https://docs.astral.sh/uv/) があれば、足りない Python とパッケージを uv が入れます） | uv（MSYS2 の `mingw-w64-ucrt-x86_64-uv`。uv が自分の Python を取ってきます。MSYS2 の Python には GUI のパッケージを入れられません） |

Windows では、MSYS2 の「UCRT64」シェルで作業します。ビルドした ECU は MinGW の実行時の DLL（`libstdc++-6.dll` など）を UCRT64 の `PATH` から読むので、`vehicle_sil_launch` と `vehicle-sil-gui` も UCRT64 シェルから起動します（道具の `vehicle_sil_launch` は DLL の要らない 1 つの実行ファイルで、`bin` に DLL はありません）。

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-uv
```

## 1. ひな形を写す

ひな形と道具は、vehicle-sil の[リリースのページ](https://github.com/rahanahu/vehicle-sil/releases)に公開した版にだけあります。使う版（この README の版）がまだ公開されていなければ、`get_tools.py` は「may not be published yet」と言って止まります。そのときは vehicle-sil のリポジトリをクローンしてビルドし（vehicle-sil の README の「vehicle-sil 自体をビルドする」）、そのビルドの `vehicle_sil_launch` を使います。

vehicle-sil のリリースのページから `vehicle-sil-template-0.1.0.zip` を取って展開し、ディレクトリの名前を自分のプロジェクトの名前にします。Git で管理するなら、そこで `git init` します（`.gitignore` は、`build/`、`bin/`、`tools/` を除きます）。

vehicle-sil のリポジトリの `template/` を写しても同じものですが、`get_tools.py` に道具の SHA-256 が入っていません（2. で `--sha256` を渡します）。

## 2. 道具を取ってくる

```sh
python3 get_tools.py     # Windows（UCRT64 シェル）: python get_tools.py
```

リリースから自分の OS の道具の圧縮ファイルを取り、SHA-256 を確かめて、`tools/vehicle-sil-tools-0.1.0-<OS>/` に展開します。取ってくる先と版は `get_tools.py` の `VERSION` と `URL` に書いてあり、最新の版を勝手に取ることはありません。最後に、`bin` を `PATH` に入れるコマンドを表示します。開いたシェルごとに実行します。

- ネットワークから取れないときは、ブラウザでリリースのページから `vehicle-sil-tools-0.1.0-linux-x86_64.tar.gz`（Windows は `-windows-x86_64.zip`）を取り、`python3 get_tools.py --archive <取ったファイル>` とします。確かめ方は同じです。
- 手で展開するときは、同じページの `SHA256SUMS` と照合してから展開し、その中の `bin` を `PATH` に入れます。
- 道具の中身は、展開したディレクトリの `README.md` にあります。

## 3. ビルドして動かす

```sh
cmake --preset linux             # Windows: cmake --preset windows
cmake --build --preset linux     # Windows: cmake --build --preset windows
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml
vehicle-sil-gui simulation.live.yaml   # 等速で、止めるまで動く構成（simulation.yaml は 1000 ms で終わる）
```

- 最初の構成で vehicle-sil と SIL Kit を取ってきて、最初のビルドで SIL Kit をコンパイルします（数分、最初の 1 回だけ）。
- `check` は、各 ECU の自己申告（`--describe`）と構成ファイルを突き合わせます。最後の行が `passed:` なら通っています。
- `run` は、registry と各 ECU を起動して、`stop_at_ms` まで動かします。`--scenario` を付けると、シナリオの手順どおりに値を入れて確かめ、全部通れば終了コード 0 です。
- `vehicle-sil-gui` は、ブラウザで構成ファイルを照合し、動かし、変数を見て書く画面を開きます。止めるときは Ctrl+C です。

## 自分の ECU にする

- `engine/main.cpp` のクラスの中身（ネットワーク、J1939 のノード、変数、ステップごとの処理）を書き換えます。ECU の名前は `main()` の `ecu_main<...>(..., {.name = "Engine", ...})` です。
- ECU を足すときは、`CMakeLists.txt` に `add_executable` から `vehicle_sil_copy_runtime_dlls` までの 4 行を足し、`simulation.yaml` の `participants` に足します。
- ネットワークの名前（`J1939_CAN`）は、ECU のコードと `simulation.yaml` の `networks` で同じにします。
- Manufacturer Code の 0x7F0 は仮の値です。

## 1 つの実行ファイルで配りたいとき

ECU の実行ファイルを、ほかのファイル（`libSilKit.so`、`SilKit.dll`、MinGW の DLL）なしで人に渡したいときは、`VEHICLE_SIL_STATIC` を付けて構成し直します。

```sh
cmake --preset linux -DVEHICLE_SIL_STATIC=ON      # Windows: cmake --preset windows -DVEHICLE_SIL_STATIC=ON
cmake --build --preset linux
```

- SIL Kit と C++ の実行時ライブラリを、実行ファイルに静的にリンクします（Linux の glibc は共有のまま）。Linux では静的な libstdc++ が要ります（Fedora は `libstdc++-static`）。
- 渡すときは、実行ファイルの中に入ったもののライセンスの表示を添えます（vehicle-sil の `THIRD_PARTY_NOTICES.md` の「Distributing executables built with `VEHICLE_SIL_STATIC`」。道具の `licenses/` に、そのまま添えられるファイルがあります）。
- 戻すときは `-DVEHICLE_SIL_STATIC=OFF` で構成し直します。

## vehicle-sil の版を上げる

ECU と道具は、同じ vehicle-sil の版でそろえます（版が違うと、`check` が `version-mismatch` を出します）。新しい版のリリースのひな形から、`get_tools.py` と、`CMakeLists.txt` の `GIT_TAG` の行を写し、`python3 get_tools.py` をやり直します。変更点と直し方は vehicle-sil の `CHANGELOG.md` にあります。

## 次に読むもの

- 手引き「[最初の ECU を作る](https://github.com/rahanahu/vehicle-sil/blob/v0.1.0/docs/howto/first-ecu.md)」: このひな形の ECU を例に、書き方、照合、実行、デバッガーからの起動、Python からの読み書き
- [J1939 のノードの作り方](https://github.com/rahanahu/vehicle-sil/blob/v0.1.0/docs/j1939-guide.md)、[型付きメッセージ](https://github.com/rahanahu/vehicle-sil/blob/v0.1.0/docs/j1939-codec.md)
- [構成ファイル](https://github.com/rahanahu/vehicle-sil/blob/v0.1.0/docs/simulation-config.md)、[照合](https://github.com/rahanahu/vehicle-sil/blob/v0.1.0/docs/simulation-check.md)、[実行](https://github.com/rahanahu/vehicle-sil/blob/v0.1.0/docs/simulation-run.md)、[シナリオ](https://github.com/rahanahu/vehicle-sil/blob/v0.1.0/docs/scenario.md)、[ブラウザの GUI](https://github.com/rahanahu/vehicle-sil/blob/v0.1.0/docs/gui.md)

## ライセンス

このひな形のファイルは MIT-0（MIT No Attribution、`LICENSE`）です。写して作ったプロジェクトに、このひな形の著作権やライセンスの表示を残す必要はありません。ひな形が取り込む vehicle-sil 本体とその依存は、それぞれのライセンス（vehicle-sil は MIT。依存は vehicle-sil の `THIRD_PARTY_NOTICES.md`）のままです。
