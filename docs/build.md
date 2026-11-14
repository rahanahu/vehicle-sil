# ビルドと取り込み

vehicle-sil 自体をビルドする人（直す人、試験を回す人、ビルド済みの道具がない環境で使う人）と、自分の CMake プロジェクトに vehicle-sil を取り込む人向けです。先に README の「[取得とビルド](../README.md#取得とビルド)」を読みます。次は、直す人なら [AGENTS.md](../AGENTS.md)（検証のスクリプト）と [code-map.md](code-map.md)（試験のラベルと回し方）、ECU を書く人なら [j1939-guide.md](j1939-guide.md) です。

## 必要なもの

| | Linux | Windows |
|---|---|---|
| コンパイラ | GCC（標準）または Clang。C++20 対応のもの | MSYS2 UCRT64 の MinGW-w64 GCC |
| ビルドツール | CMake 3.21 以上、Ninja（Control Server を作り、gRPC を FetchContent で取り込むときは CMake 3.28 以上。[dependencies.md](dependencies.md) の「gRPC と Protocol Buffers」） | 同左（MSYS2 のパッケージ） |
| その他 | Git（SIL Kit へのパッチ適用にも使う） | 同左 |
| 試験と例のビルド、型付きメッセージの生成 | Python 3.11 以上と PyYAML（[uv](https://docs.astral.sh/uv/) で入れることを勧めます） | 同左（MSYS2 のパッケージ） |
| Control API の Python クライアント | Python 3.11 以上と grpcio、grpcio-tools、protobuf（同上） | 同左（MSYS2 のパッケージ） |

Visual Studio と MSVC は不要です。

### Python（uv）

Python の版と依存は、リポジトリ直下の `pyproject.toml`、`.python-version`、`uv.lock` で決めています。[uv](https://docs.astral.sh/uv/) があれば、次のコマンドで `.venv` に開発用の Python と依存の全部が入ります（`.python-version` の Python がなければ uv が取ってきます）。

```sh
uv sync                        # すべて（既定の dev グループ）
uv sync --only-group codegen   # 型付きメッセージの生成だけ（PyYAML）
uv sync --only-group control   # Control API のクライアントだけ（grpcio、grpcio-tools、protobuf）
```

`--only-group` は、ほかのグループのパッケージを `.venv` から外します（`uv sync --only-group control` の後は、型付きメッセージの生成に `.venv` を使えません）。迷ったら `uv sync` にします。

- vehicle-sil を最上位のプロジェクトとして Linux でビルドするとき、CMake はこの `.venv` があれば、その Python を先に使います（環境を有効にしたり、Python を指定したりしなくてよい）。`.venv` がないか、必要なパッケージがないときは、今までどおり `find_package(Python3)` で探します。FetchContent などで取り込んだ利用側のビルドと Windows のビルドは、vehicle-sil の `.venv` を使いません（[依存ライブラリ](dependencies.md)の「Python の探し方」）。
- 依存を変えたら `uv lock` で `uv.lock` を更新し、両方をコミットします。`uv sync --locked` は、`uv.lock` が `pyproject.toml` と合わないと失敗します。
- 対応する最も古い版（3.11）と `.python-version` の版で Python のコードを確かめるには、`scripts/verify-python.py` を使います（C++ のビルドは要りません）。`pyproject.toml` に書いた依存の下限そのものでも 3.11 で確かめるので、下限は試験を回した版になります。
- uv を使わない場合は、`pyproject.toml` の `[dependency-groups]` にあるパッケージを pip で入れても構いません。Windows（MSYS2 UCRT64）では、uv を使わずに、下の pacman のパッケージの Python のままで構いません。

## ビルド済みの道具

- 道具の圧縮ファイル（`vehicle-sil-tools-<版>-linux-x86_64.tar.gz`、`-windows-x86_64.zip`）には、`vehicle_sil_launch`（Control Server つき）、GUI サーバーと画面、Control API の Python クライアント（gRPC のスタブつき）、GUI のクライアントのライブラリ（1 つの ES モジュールと、型つきの npm のパッケージ）、自分の画面のひな形（`gui_starter/`。`vehicle-sil-gui --web gui_starter simulation.yaml`）、ダッシュボードの JSON Schema、ライセンスの表示が入っています（中身は圧縮ファイルの `README.md`、同梱する物は [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)）。Linux 版は x86_64 で、Ubuntu 22.04 でビルドしているので、glibc がそれと同じか新しい（2.35 以上の）ディストリビューションで動き、glibc のほかに共有ライブラリを要りません。Windows 版は MSYS2 UCRT64 の MinGW でビルドし、DLL を同梱しない 1 つの実行ファイルです（SIL Kit、gRPC、C++ の実行時ライブラリを静的にリンク）。
- ブラウザの GUI に自分の画面を作るときは、リリースのページの `vehicle-sil-gui-client-<版>.tgz`（道具の圧縮ファイルの `client/` にも同じもの）を `npm install <その URL かファイル>` すると、クライアントのライブラリを `@vehicle-sil/gui-client` として使えます（[docs/gui.md](gui.md) の「自分の画面を作る」）。
- 道具の圧縮ファイルは `scripts/package.py` が作ります（`tools`、使う人の手順で確かめる `verify`、ひな形の圧縮ファイルの `template`。`--help`）。タグ `v<版>` を push すると、GitHub Actions が両方の OS で作って確かめ、リリースの下書きに付けます。

## vehicle-sil 自体をビルドする

要るものがそろっているかは、`scripts/doctor.py` で確かめられます（読むだけです。足りないものには、入れ方を出します）。

SIL Kit のパッチ（`patches/sil-kit/`）は、クローンしたときには当たりません。`cmake --preset …` の構成のときに、ビルドのディレクトリの中の写しに当てます（`external/sil-kit` は上流のままです）。構成の出力の `vehicle-sil: sil-kit (…) + … patch(es) … -> …` の行に、使うコミット、当てたパッチ、写しの場所が出ます。

### Linux

README の「[vehicle-sil 自体をビルドする](../README.md#vehicle-sil-自体をビルドする)」のコマンドの `linux-gcc-release` を、次のプリセットに替えられます。

| プリセット | 内容 |
|---|---|
| `linux-gcc-debug`, `linux-gcc-release` | GCC |
| `linux-clang-debug` | Clang |
| `linux-clang-asan`, `linux-clang-tsan` | Clang と AddressSanitizer + UBSan、または ThreadSanitizer |
| `linux-gcc-asan`, `linux-gcc-tsan` | GCC とサニタイザー。`libasan`、`libubsan`、`libtsan` のパッケージが必要 |
| `linux-gcc-static`、`windows-mingw-static` | Release で `VEHICLE_SIL_STATIC=ON`（下の「1 つの実行ファイルにする」）。静的な libstdc++ が要る（Fedora は `libstdc++-static`）。`scripts/verify.py --linux` には入れていない（名前を指定して回す） |
| `linux-gcc-control` | GCC の Debug で `VEHICLE_SIL_BUILD_CONTROL_SERVER=ON`（Control Server と、その試験も作る。[control-api.md](control-api.md) の「ビルド」）。gRPC が見つからなければ、固定した版を FetchContent で取り込んでビルドするので、CMake 3.28 以上と、初回の構成のときに git とネットワークが要る。Python のクライアントの試験は、grpcio と grpcio-tools のある Python（`uv sync`）があるときだけ登録する。`scripts/verify.py --linux` には入れていない（名前を指定して回す） |

ThreadSanitizer のプリセットの試験は、誤検出とわかっている報告だけを `cmake/tsan-suppressions.txt` で抑えます（1 件ごとに理由を書いています）。

`-DVEHICLE_SIL_WARNINGS_AS_ERRORS=ON` を付けると、vehicle-sil 自身のターゲットの警告をエラーにします。

コミット前の確認には `scripts/verify.py --linux` が使えます（[AGENTS.md](../AGENTS.md) の「検証とスクリプト」）。

### Windows（MSYS2 UCRT64）

MSYS2 の GCC（MinGW-w64、UCRT 版）でビルドします。MSYS2 を入れて、その「UCRT64」シェルに入ります。UCRT64 シェルは、この GCC や CMake が `PATH` に入った bash です。PowerShell かコマンドプロンプトで次を実行します。

```powershell
winget install MSYS2.MSYS2
C:\msys64\msys2_shell.cmd -ucrt64 -defterm -here -no-start
```

2 行目で、今のターミナルのまま、今のディレクトリで UCRT64 シェルに入ります（スタートメニューの「MSYS2 UCRT64」でも同じシェルが開きます）。`echo $MSYSTEM` が `UCRT64` を表示すれば、正しいシェルです。以降はこのシェルで実行します。

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-yaml
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --preset windows-mingw-release
```

`SilKit.dll` は、ビルド後に各実行ファイルの隣へ自動でコピーされます。MinGW のランタイム DLL（`libstdc++-6.dll` など）は、UCRT64 シェルの `PATH` から読み込まれます。UCRT64 シェルの外（PowerShell など）から実行するときは、`C:\msys64\ucrt64\bin` を `PATH` に入れます。ほかの MinGW の `bin` より前に置きます。DLL が見つからないとき（`0xC0000135`）や、別の版の GCC の DLL を先に読み込んだとき（`0xC0000139`）は、何も表示せずに終わります。

## 外部プロジェクトから使う（FetchContent）

```cmake
include(FetchContent)

FetchContent_Declare(
  vehicle_sil
  GIT_REPOSITORY https://github.com/rahanahu/vehicle-sil.git
  GIT_TAG <固定するコミットハッシュ>)

FetchContent_MakeAvailable(vehicle_sil)

add_executable(example main.cpp)
target_link_libraries(example PRIVATE vehicle_sil::simulation vehicle_sil::j1939_periodic_sender)   # core だけなら vehicle_sil::core
vehicle_sil_copy_runtime_dlls(example)   # Windows: SilKit.dll をコピーする（ほかの OS では何もしない）
```

新しく始めるときは、これを書いたひな形 [template/](../template) を写すのが早道です（「[取得とビルド](../README.md#取得とビルド)」）。

SIL Kit は、vehicle-sil のサブモジュールとして自動で取得・設定されます。利用側で個別に設定する必要はありません。SIL Kit には、ビルドツリーにコピーしたソースに `patches/sil-kit/` のパッチを当てたものを使います（[docs/dependencies.md](dependencies.md)）。パッチの適用に Git を使います。リンクしないライブラリはビルドされません。完全な例は [examples/fetchcontent_consumer](../examples/fetchcontent_consumer) にあります。

最小の使い方は次のとおりです。

```cpp
#include <vehicle_sil/j1939/periodic_sender.hpp>
#include <vehicle_sil/simulation/simulation.hpp>

namespace j1939 = vehicle_sil::j1939;
using namespace std::chrono_literals;

// ECU は Ecu& を受け取るクラスにする。モデルも ECU が持つ（emplace）ので、宣言の順序を気にしなくてよい
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        auto& node = ecu.j1939("J1939_CAN1").add_node(
            {.address = 0x00, .name = j1939::Name{}.set_identity_number(0x2345).set_manufacturer_code(0x7F0)});   // Address Claim はスタックが行う
        node.attach<j1939::PeriodicSender>(j1939::Pgn{0xFF10}, 100ms,   // アドレスを得てから 100 ms ごとに送る
                                           [this](std::span<std::uint8_t> out) { return model_.encode(out); });
        ecu.every(10ms, [this](vehicle_sil::SimTime) { model_.tick(); });
    }
private:
    EngineModel model_;
};

int main() {
    vehicle_sil::Simulation sim(vehicle_sil::SilKitOptions{});   // SIL Kit の仮想時間。registry はプロセス内に立つ
    sim.add<Engine>("Engine", 10ms);
    sim.run_for(3s);
}
```

受信、Request と応答、受信の監視、試験の書き方は [docs/j1939-guide.md](j1939-guide.md) にあります。

### 1 つの実行ファイルにする（`VEHICLE_SIL_STATIC`）

ECU の実行ファイルを、ほかのファイルなしで配りたいときは、`-DVEHICLE_SIL_STATIC=ON` で構成します（既定は OFF）。SIL Kit を静的なライブラリとして作り、SIL Kit をリンクする実行ファイルに C++ の実行時ライブラリも静的にリンクします。

- Linux: `libSilKit.so`、`libstdc++`、`libgcc_s` が要らなくなります。glibc は共有のままです（静的にした glibc のプログラムは、名前の解決などのモジュールを読み込めず、かえって動く環境を選ぶため）。ビルドには静的な libstdc++（Fedora は `libstdc++-static`、Debian と Ubuntu は `libstdc++-<版>-dev` に入っている）が要ります。
- Windows（MinGW）: `-static` でリンクし、`SilKit.dll`、`libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll` が要らなくなります。`vehicle_sil_copy_runtime_dlls()` は、写す DLL がないので何もしません。
- FetchContent で取り込む側でも同じです（configure に `-DVEHICLE_SIL_STATIC=ON` を渡すか、`FetchContent_MakeAvailable` の前に `set(VEHICLE_SIL_STATIC ON)`）。ひな形では `cmake --preset linux -DVEHICLE_SIL_STATIC=ON` です。
- 実行ファイルは、SIL Kit の分だけ大きくなります。
- 配るときは、実行ファイルの中に入ったもののライセンスの表示を添えます。何を添えるかは [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) の「Distributing executables built with `VEHICLE_SIL_STATIC`」にあります。

信号を YAML で定義して型付きメッセージにするときは、`vehicle_sil_add_j1939_codec(<target> <定義.yaml>)` を呼び、`<target>` をリンクします（Python 3.11 以上と PyYAML が要ります。[docs/j1939-codec.md](j1939-codec.md)）。
