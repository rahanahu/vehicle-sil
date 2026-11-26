# ビルドと取り込み

[English](../build.md)

vehicle-sil 自体のビルド方法と、自分の CMake プロジェクトに vehicle-sil を取り込む方法をまとめたドキュメントです。

| したいこと | 読む節 |
|---|---|
| リポジトリをビルドして、テストを実行する | [必要なもの](#必要なもの)、[vehicle-sil 自体をビルドする](#vehicle-sil-自体をビルドする) |
| ビルドせずにツールを使う | [ビルド済みのツール](#ビルド済みのツール) |
| 自分の CMake プロジェクトから使う | [外部プロジェクトから使う（FetchContent）](#外部プロジェクトから使うfetchcontent) |
| ECU を、ほかのファイルなしで動く 1 つの実行ファイルにして配布する | [1 つの実行ファイルにする（`VEHICLE_SIL_STATIC`）](#1-つの実行ファイルにするvehicle_sil_static) |

## 必要なもの

| 必要なもの | 用途 | Linux | Windows |
|---|---|---|---|
| コンパイラ | すべてのビルド | C++20 に対応した GCC（標準）か Clang | MSYS2 UCRT64 の MinGW-w64 GCC |
| CMake 3.21 以上、Ninja | すべてのビルド | ディストリビューションのパッケージ | MSYS2 のパッケージ |
| Git | クローンと、SIL Kit へのパッチの適用 | ディストリビューションのパッケージ | MSYS2 のパッケージ |
| Python 3.11 以上と PyYAML | テストとサンプルのビルド、型付きメッセージの生成 | [uv](https://docs.astral.sh/uv/) でのインストールを勧めます。 | MSYS2 のパッケージ |
| Python 3.11 以上と grpcio、grpcio-tools、protobuf | Control API の Python クライアント | uv でのインストールを勧めます。 | MSYS2 のパッケージ |

Control Server をビルドし、gRPC を FetchContent で取り込むときは、CMake 3.28 以上が必要です（[dependencies.md](dependencies.md) の「gRPC と Protocol Buffers」）。Visual Studio と MSVC は不要です。

必要なものがそろっているかは、クローンした後に `scripts/doctor.py` で確認できます。読むだけのスクリプトで、足りないものにはインストールの方法を表示します。

### Python（uv）

Python のバージョンと依存パッケージは、リポジトリ直下の `pyproject.toml`、`.python-version`、`uv.lock` で決めています。[uv](https://docs.astral.sh/uv/) があれば、次のコマンドで、開発用の Python と依存パッケージのすべてが `.venv` に入ります。`.python-version` の Python が PC になければ、uv がダウンロードします。

```sh
uv sync                        # すべて（既定の dev グループ）
uv sync --only-group codegen   # 型付きメッセージの生成だけ（PyYAML）
uv sync --only-group control   # Control API のクライアントだけ（grpcio、grpcio-tools、protobuf）
```

`--only-group` は、ほかのグループのパッケージを `.venv` から削除します。たとえば `uv sync --only-group control` の後は、型付きメッセージの生成に `.venv` を使えません。迷ったら `uv sync` にします。

CMake が使う Python は、ビルドのしかたで決まります（[dependencies.md](dependencies.md) の「Python の探し方」）。

| ビルド | CMake が使う Python |
|---|---|
| Linux で、vehicle-sil を最上位のプロジェクトとしてビルドする（`.venv` に必要なパッケージがある） | `.venv` の Python を先に使います（環境の有効化も Python の指定も不要です）。 |
| 同じビルドで、`.venv` がないか、必要なパッケージがない | `find_package(Python3)` で探します。 |
| FetchContent などで取り込んだ利用側のビルド | vehicle-sil の `.venv` を使いません。 |
| Windows のビルド | vehicle-sil の `.venv` を使いません。 |

依存パッケージを変えたら、`uv lock` で `uv.lock` を更新し、`pyproject.toml` と `uv.lock` の両方をコミットします。`uv sync --locked` は、`uv.lock` が `pyproject.toml` と合わないと失敗します。

対応する最も古いバージョン（3.11）と `.python-version` のバージョンで Python のコードを確認するには、`scripts/verify-python.py` を使います（C++ のビルドは不要です）。このスクリプトは、`pyproject.toml` に書いた依存の下限そのものでも 3.11 で確認します。そのため、下限は、テストを実行したバージョンになります。

uv を使わない場合は、`pyproject.toml` の `[dependency-groups]` にあるパッケージを pip でインストールしても構いません。Windows（MSYS2 UCRT64）では、uv を使わずに、下の pacman のパッケージの Python のままで構いません。

## vehicle-sil 自体をビルドする

SIL Kit のパッチ（`patches/sil-kit/`）は、クローンした時点では適用されていません。`cmake --preset …` の configure のときに、ビルドのディレクトリの中のコピーに適用します（`external/sil-kit` は上流のままです）。使うコミット、適用したパッチ、コピーの場所は、configure の出力の `vehicle-sil: sil-kit (…) + … patch(es) … -> …` の行でわかります。

### Linux

WSL では、`/mnt/c` のようなマウントされた Windows ドライブではなく、WSL の Linux ファイルシステム上（たとえば `~/` 配下）にクローンしてビルドしてください（9p ファイルシステムではビルドが非常に遅くなります）。また、Windows と WSL で同じ作業ツリーや `node_modules` を共有しないでください。

```sh
git clone --recursive https://github.com/rahanahu/vehicle-sil.git
cd vehicle-sil
scripts/doctor.py                      # 必要なものがそろっているかを 1 行ずつ表示する
uv sync                                # ビルドが使う Python のパッケージをインストールする
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

3 つのコマンドの `linux-gcc-debug` は、次のプリセットに替えられます。README の「[リポジトリからビルドする](../../README.ja.md#リポジトリからビルドする)」は `linux-gcc-control` を使っています。

| プリセット | 内容 | 追加で必要なもの |
|---|---|---|
| `linux-gcc-debug`、`linux-gcc-release` | GCC | なし |
| `linux-clang-debug` | Clang | なし |
| `linux-clang-asan`、`linux-clang-tsan` | Clang と、AddressSanitizer + UBSan または ThreadSanitizer | なし |
| `linux-gcc-asan`、`linux-gcc-tsan` | GCC とサニタイザー | `libasan`、`libubsan`、`libtsan` のパッケージ |
| `linux-gcc-static` | Release で `VEHICLE_SIL_STATIC=ON`（下の「1 つの実行ファイルにする（`VEHICLE_SIL_STATIC`）」） | 静的な libstdc++（Fedora は `libstdc++-static`） |
| `linux-gcc-control` | GCC の Debug で `VEHICLE_SIL_BUILD_CONTROL_SERVER=ON` | gRPC が PC になければ、CMake 3.28 以上と、最初の configure のときの Git とネットワーク |

`linux-gcc-control` は、Control Server とそのテストもビルドします（[control-api.md](control-api.md) の「ビルド」）。gRPC が見つからなければ、固定したバージョンを FetchContent で取り込んでビルドします。Python のクライアントのテストは、grpcio と grpcio-tools のある Python（`uv sync`）があるときだけ登録します。

`scripts/verify.py --linux` は `CMakePresets.json` で既定に指定した 5 つのプリセット（`linux-gcc-debug`、`linux-gcc-release`、`linux-clang-debug`、`linux-clang-asan`、`linux-clang-tsan`）を実行し、`linux-gcc-static`、`linux-gcc-asan`、`linux-gcc-tsan`、`linux-gcc-control` は含みません。これらは名前を指定して実行するか、`--all-linux` で Linux のプリセットをすべて実行します。

ThreadSanitizer のプリセットのテストは、誤検出とわかっている報告だけを `cmake/tsan-suppressions.txt` で抑えます（1 件ごとに理由を書いています）。

`-DVEHICLE_SIL_WARNINGS_AS_ERRORS=ON` を付けると、vehicle-sil 自身のターゲットの警告をエラーにします。

コミット前の確認には `scripts/verify.py --linux` が使えます（[AGENTS.md](../../AGENTS.md#verification-and-scripts)）。

### Windows（MSYS2 UCRT64）

MSYS2 の GCC（MinGW-w64、UCRT 版）でビルドします。作業は MSYS2 の「UCRT64」シェルで行います。UCRT64 シェルは、この GCC や CMake が `PATH` に入った bash です。

まず、PowerShell かコマンドプロンプトで、MSYS2 をインストールして UCRT64 シェルに入ります。

```powershell
winget install MSYS2.MSYS2
C:\msys64\msys2_shell.cmd -ucrt64 -defterm -here -no-start
```

2 行目は、今のターミナルのまま、今のディレクトリで UCRT64 シェルに入ります（スタートメニューの「MSYS2 UCRT64」でも同じシェルが開きます）。`echo $MSYSTEM` が `UCRT64` を表示すれば、正しいシェルです。これ以降のコマンドは、このシェルで実行します。

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-yaml
git clone --recursive https://github.com/rahanahu/vehicle-sil.git
cd vehicle-sil
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --preset windows-mingw-release
```

プリセットは `windows-mingw-debug`、`windows-mingw-release`、`windows-mingw-static`（Release で `VEHICLE_SIL_STATIC=ON`）、`windows-mingw-control`（Debug で `VEHICLE_SIL_BUILD_CONTROL_SERVER=ON`。[control-api.md](control-api.md) の「ビルド」）の 4 つです。

実行ファイルが読み込む DLL は、次の場所にあります。

| DLL | 場所 |
|---|---|
| `SilKit.dll` | ビルド後に、各実行ファイルの隣へ自動でコピーされます。 |
| MinGW のランタイム DLL（`libstdc++-6.dll` など） | UCRT64 シェルの `PATH` から読み込まれます。 |

UCRT64 シェルの外（PowerShell など）から実行するときは、`C:\msys64\ucrt64\bin` を `PATH` に追加します。ほかの MinGW の `bin` より前に置きます。DLL の読み込みに失敗すると、実行ファイルは何も表示せずに終了します。

| 終了コード | 原因 |
|---|---|
| `0xC0000135` | DLL が見つかりません。 |
| `0xC0000139` | 別のバージョンの GCC の DLL を先に読み込みました。 |

## ビルド済みのツール

リリースのページで、ビルド済みのツールを OS ごとのアーカイブで配布します。

| アーカイブ | ビルドした環境 | 実行に必要なもの |
|---|---|---|
| `vehicle-sil-tools-<version>-linux-x86_64.tar.gz` | Ubuntu 22.04（x86_64） | glibc 2.35 以上（Ubuntu 22.04 と同じか新しい glibc）だけで、ほかの共有ライブラリは不要です。 |
| `vehicle-sil-tools-<version>-windows-x86_64.zip` | MSYS2 UCRT64 の MinGW | DLL を同梱しない 1 つの実行ファイルです（SIL Kit、gRPC、C++ のランタイムを静的にリンク）。 |

アーカイブには次のものが入っています。詳しい内容はアーカイブの `README.md` に、同梱しているものとそのライセンスは [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) にあります。

| 入っているもの | 補足 |
|---|---|
| `vehicle_sil_launch` | Control Server つきです。 |
| `vehicle_sil_ethernet_bridge` | Ethernet のブリッジです（[ethernet-bridge.md](ethernet-bridge.md)）。`vehicle_sil_launch` の隣にあり、`vehicle_sil_launch run --connect` がそこで見つけます。`npcap`（Windows）と `pcap`（Linux）のバックエンドには Npcap か libpcap が要ります。アーカイブには入っていないので、自分でインストールします。 |
| `vehicle_sil_socketcan_bridge`（Linux のアーカイブだけ） | SocketCAN のブリッジです（[socketcan-bridge.md](socketcan-bridge.md)）。この PC の CAN アダプターか `vcan0` につなぎます。特権は要りません。`vehicle_sil_launch run --connect CAN=can0` で起動します。 |
| GUI サーバーと画面 | — |
| Control API の Python クライアント | gRPC のスタブつきです。 |
| GUI のクライアントのライブラリ | 1 つの ES モジュールと、型つきの npm のパッケージです。 |
| 自分の画面のテンプレート（`gui_starter/`） | `vehicle-sil-gui --web gui_starter simulation.yaml` で使います。 |
| ダッシュボードの JSON Schema | — |
| ライセンスの表示 | — |

Vector XL の CAN のブリッジはアーカイブに入っていません。Vector の XL Driver Library がないとビルドできないためです（[bridges.md](bridges.md) の「ビルド」）。

ブラウザの GUI に自分の画面を作るときは、同じ版のクライアントのライブラリを `npm install @vehicle-sil/gui-client@<version>`（npm のレジストリから）でインストールするか、リリースのページの `vehicle-sil-gui-client-<version>.tgz` を `npm install <その URL かファイル>` でインストールします。クライアントのライブラリを `@vehicle-sil/gui-client` として使えます（[gui.md](gui.md) の「自分の画面を作る」）。同じファイルが、ツールのアーカイブの `client/` にも入っています。

アーカイブは `scripts/package.py` が作ります。

| サブコマンド | すること |
|---|---|
| `tools` | ツールのアーカイブを作ります。 |
| `verify` | アーカイブを、利用者の手順で確認します。 |
| `template` | テンプレートのアーカイブを作ります。 |

引数は `scripts/package.py --help` で表示します。タグ `v<version>` を push すると、GitHub Actions が両方の OS でアーカイブを作って確認し、リリースの下書きに添付します。下書きを公開すると、GitHub Actions がそのクライアントのライブラリのパッケージを npm のレジストリに公開します（[code-map-build.md](code-map-build.md) の「npm へのクライアントのライブラリの公開」）。

## 外部プロジェクトから使う（FetchContent）

自分の CMake プロジェクトの `CMakeLists.txt` に、次のように書きます。

```cmake
cmake_minimum_required(VERSION 3.21)
project(example LANGUAGES C CXX)

include(FetchContent)

FetchContent_Declare(
  vehicle_sil
  GIT_REPOSITORY https://github.com/rahanahu/vehicle-sil.git
  GIT_TAG <固定するリリースタグまたはコミットハッシュ>)   # v0.4.0 のようなリリースタグか完全なコミットハッシュを指定する

FetchContent_MakeAvailable(vehicle_sil)

add_executable(example main.cpp)
target_link_libraries(example PRIVATE vehicle_sil::simulation vehicle_sil::j1939_periodic_sender)   # core だけなら vehicle_sil::core
vehicle_sil_copy_runtime_dlls(example)   # Windows: SilKit.dll をコピーする（ほかの OS では何もしない）
```

新しく始めるときは、これを書いてあるテンプレート [template/](../../template) をコピーするのが早道です（README の「[クイックスタート](../../README.ja.md#クイックスタート)」）。完全な例は [examples/fetchcontent_consumer](../../examples/fetchcontent_consumer) にあります。

| 項目 | 取り込んだときの動作 |
|---|---|
| SIL Kit | vehicle-sil のサブモジュールとして自動で取得、設定されるので、利用側で個別に設定する必要はありません。 |
| SIL Kit へのパッチ | ビルドツリーにコピーしたソースに `patches/sil-kit/` のパッチを適用したものを使います（[dependencies.md](dependencies.md)）。 |
| Git | パッチの適用に使います。 |
| リンクしないライブラリ | ビルドされません。 |

最小の `main.cpp` は次のとおりです。

```cpp
#include <vehicle_sil/j1939/periodic_sender.hpp>
#include <vehicle_sil/simulation/simulation.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace j1939 = vehicle_sil::j1939;
using namespace std::chrono_literals;

// ECU は Ecu& を受け取るクラスにする。モデルの状態（ここでは rpm_）も ECU のクラスが持つ
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        auto& node = ecu.j1939("J1939_CAN1").add_node(
            {.address = 0x00, .name = j1939::Name{}.set_identity_number(0x2345).set_manufacturer_code(0x7F0)});   // Address Claim はスタックが行う
        node.attach<j1939::PeriodicSender>(j1939::Pgn{0xFF10}, 100ms,   // アドレスを得てから 100 ms ごとに送信する
                                           [this](std::span<std::uint8_t> out) -> std::size_t {
                                               out[0] = static_cast<std::uint8_t>(rpm_ & 0xFF);
                                               out[1] = static_cast<std::uint8_t>(rpm_ >> 8);
                                               return 2;
                                           });
        ecu.every(10ms, [this](vehicle_sil::SimTime) { rpm_ = static_cast<std::uint16_t>(rpm_ + 10); });
    }
private:
    std::uint16_t rpm_ = 0;
};

int main() {
    vehicle_sil::Simulation sim(vehicle_sil::SilKitOptions{});   // SIL Kit の仮想時間。registry はプロセス内に立つ
    sim.add<Engine>("Engine", 10ms);
    sim.run_for(3s);
}
```

受信、Request と応答、受信の監視、テストの書き方は [j1939-guide.md](j1939-guide.md) にあります。

信号を YAML で定義して型付きメッセージにするときは、`vehicle_sil_add_j1939_codec(<target> <definition.yaml>)` を呼び、`<target>` をリンクします。Python 3.11 以上と PyYAML が必要です（[j1939-codec.md](j1939-codec.md)）。

### 1 つの実行ファイルにする（`VEHICLE_SIL_STATIC`）

ECU の実行ファイルを、ほかのファイルなしで配布したいときは、`-DVEHICLE_SIL_STATIC=ON` を付けて configure します（既定は OFF）。SIL Kit を静的ライブラリとしてビルドし、SIL Kit をリンクする実行ファイルに、C++ のランタイムも静的にリンクします。

| ビルドするもの | 指定のしかた |
|---|---|
| vehicle-sil 自体 | `cmake --preset linux-gcc-static`（Windows は `windows-mingw-static`） |
| FetchContent で取り込む自分のプロジェクト | configure に `-DVEHICLE_SIL_STATIC=ON` を渡すか、`FetchContent_MakeAvailable` の前に `set(VEHICLE_SIL_STATIC ON)` を書きます。 |
| テンプレートのプロジェクト | `cmake --preset linux -DVEHICLE_SIL_STATIC=ON` |

| OS | 不要になるファイル | ビルドに追加で必要なもの |
|---|---|---|
| Linux | `libSilKit.so`、`libstdc++`、`libgcc_s` | 静的な libstdc++（Fedora は `libstdc++-static`。Debian と Ubuntu は `libstdc++-<version>-dev` に入っています） |
| Windows（MinGW） | `SilKit.dll`、`libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll` | なし（`-static` でリンクします） |

Linux の glibc は共有ライブラリのままです。glibc を静的にリンクしたプログラムは、名前の解決などのモジュールを読み込めず、かえって動く環境が限られるためです。

Windows では、コピーする DLL がなくなるので、`vehicle_sil_copy_runtime_dlls()` は何もしません。

実行ファイルは、SIL Kit の分だけ大きくなります。

#### 配布するときに添えるライセンスの表示

`VEHICLE_SIL_STATIC=ON` でリンクした実行ファイルには、SIL Kit、SIL Kit がコンパイルして取り込むコード、C++ のランタイムが入っています。配布するときは、次のライセンスの表示を実行ファイルに添えます。

| 入っているもの | 添えるもの | 補足 |
|---|---|---|
| vehicle-sil | `LICENSE`（MIT） | vehicle-sil のライブラリは静的ライブラリなので、`VEHICLE_SIL_STATIC` を付けない ECU の実行ファイルにも添えます。 |
| SIL Kit | `external/sil-kit/LICENSE` | — |
| SIL Kit がコンパイルして取り込むコード | `external/sil-kit/ThirdParty/LICENSES.rst`、`external/sil-kit/ThirdParty/` の下にある fmt、spdlog、asio のライセンスのファイル、`external/sil-kit/ThirdParty/rapidyaml/rapidyaml.hpp` の中の表示 | — |
| libstdc++、libgcc（Linux、Windows） | なし | GCC Runtime Library Exception は、GCC でコンパイルしたプログラムに表示を求めません。 |
| winpthreads、mingw-w64 の C ランタイム（Windows の MinGW） | MSYS2 が `/ucrt64/share/licenses/winpthreads/` と `/ucrt64/share/licenses/crt/` にインストールするファイル | C ランタイムとスタートアップコードは、`VEHICLE_SIL_STATIC` を付けなくても、MinGW のすべての実行ファイルに入っています。 |

ビルド済みのツールのアーカイブに、同じファイルが入っています。そのままコピーして添えられます。

| アーカイブの中のディレクトリ | 入っているもの |
|---|---|
| `licenses/sil-kit/` | 上の表の SIL Kit の 2 行のファイル一式です（`rapidyaml.hpp` の中の表示は、`rapidyaml-notices.txt` に抜き出してあります）。 |
| `licenses/msys2/`（Windows のアーカイブ） | MSYS2 がインストールする winpthreads と C ランタイムのライセンスのファイルです。 |

## 関連するドキュメント

- 最初に試すコマンド → README の「[クイックスタート](../../README.ja.md#クイックスタート)」
- 検証のスクリプト → [AGENTS.md](../../AGENTS.md)
- テストのラベルと実行方法 → [code-map.md](code-map.md)
- ECU の書き方 → [j1939-guide.md](j1939-guide.md)
- SIL Kit の固定、パッチ、gRPC、Python の探し方 → [dependencies.md](dependencies.md)
