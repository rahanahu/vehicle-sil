# 依存ライブラリ

[English](../dependencies.md)

vehicle-sil が使う外部のライブラリと、その固定、ビルドへの取り込み方、使っている API、実装が前提にしている上流の挙動、SIL Kit へのパッチをまとめたドキュメントです。

| 知りたいこと | 読む節 |
|---|---|
| 使うライブラリ、入手のしかた、ライセンス | [使っているライブラリ](#使っているライブラリ) |
| SIL Kit をビルドに取り込む方法、パッチの適用 | [取り込み方](#取り込み方cmakevehiclesildependenciescmake)、[使っているターゲット](#使っているターゲット) |
| vehicle-sil が呼ぶ SIL Kit の API | [使っている API](#使っている-api) |
| 上流を更新するときに確認すること | [実装が前提にしている上流の挙動](#実装が前提にしている上流の挙動)、[混ぜてよい SIL Kit のバージョン](#混ぜてよい-sil-kit-のバージョン)、[上流を更新する手順](#上流を更新する手順) |
| 上流の不具合と、その対応 | [上流の既知の不具合](#上流の既知の不具合)、[SIL Kit へのパッチ](#sil-kit-へのパッチ) |

## 使っているライブラリ

vehicle-sil は上流ライブラリの SIL Kit を Git サブモジュールとして取り込み、特定のコミットに固定しています。SIL Kit にはパッチ（「SIL Kit へのパッチ」）を適用してビルドします。サブモジュールの作業ツリーは変更しません。J1939 のスタックは vehicle-sil 自身のもの（`vehicle_sil::j1939`）で、外部のライブラリを使いません。

| ライブラリ | パス | 固定コミット | 由来 | ライセンス |
|---|---|---|---|---|
| Vector SIL Kit | `external/sil-kit` | `52ad61d5399483c395585d5feee89d2e89fc74b1` | タグ `v5.0.8` | MIT |

SIL Kit は自身のサブモジュール（`ThirdParty/asio`、`fmt`、`spdlog`、`googletest`）を持つため、`git submodule update --init --recursive` が必要です。FetchContent はこれを自動で行います。

SIL Kit 本体は C++17 でビルドされます。C++ API はヘッダーのみで C API を包む構成なので、利用側を C++20 でコンパイルしても問題ありません。MinGW-w64 は上流が対応しています（`-Wa,-mbig-obj` など）。

### gRPC と Protocol Buffers（Control Server、任意）

`VEHICLE_SIL_BUILD_CONTROL_SERVER=ON`（既定 OFF）のときだけ使います。使うのは `vehicle_sil::control_grpc`、`proto/` の生成コード、`vehicle_sil_launch`（`run --control`）で、ほかのライブラリ（`vehicle_sil::launch` を含む）と ECU の実行ファイルは使いません。

| 場合 | 使う gRPC |
|---|---|
| `find_package(gRPC CONFIG)` で見つかる | 見つかったものを使います（MSYS2 UCRT64 は `pacman -S mingw-w64-ucrt-x86_64-grpc`）。 |
| 見つからない | FetchContent で gRPC を取り込みます（GitHub の `grpc/grpc`、タグ `v1.76.0`、`VEHICLE_SIL_GRPC_VERSION` で変えられる。サブモジュールは abseil、protobuf、re2、c-ares、BoringSSL、zlib だけ）。 |

取り込んだ gRPC はインストールのルールを作らず（abseil の export 用の設定だけ有効）、警告の対象外（`SYSTEM`）にします。FetchContent で取り込むには CMake 3.28 以上が必要です（それより古い CMake では、gRPC をインストールして `find_package` で見つけさせます）。ダウンロード済みのソースは `-DFETCHCONTENT_SOURCE_DIR_GRPC=<path>` で再利用できます。

| ライブラリ | ライセンス |
|---|---|
| gRPC | Apache-2.0 |
| Protocol Buffers | BSD-3-Clause |
| abseil | Apache-2.0 |
| re2 | BSD-3-Clause |
| c-ares | MIT |
| BoringSSL | Apache-2.0（一部に古い OpenSSL と ISC の条項が残る） |
| zlib | zlib ライセンス |

これらは vehicle-sil のリポジトリには含めません。

Python のクライアント（`python/vehicle_sil_control`）は `grpcio` と `protobuf` を、スタブの生成には `grpcio-tools` を使います（`pyproject.toml` の `control` グループ。uv がなければ pip、MSYS2 は `mingw-w64-ucrt-x86_64-python-grpcio` と `-python-grpcio-tools`）。テストは、これらがインストールされた Python 3.11 以上（`VEHICLE_SIL_CONTROL_PYTHON`。指定がなければ、下の「Python の探し方」の順）があるときだけ登録します。

### Python の探し方

Python のバージョンと依存パッケージは、リポジトリ直下の `pyproject.toml`（`requires-python` と `[dependency-groups]`）、`.python-version`（開発で使うバージョン）、`uv.lock`（解決したバージョンの固定）で決めています。

| グループ | パッケージ |
|---|---|
| `codegen` | PyYAML |
| `control` | grpcio、grpcio-tools、protobuf |
| `gui` | GUI サーバー（`gui/server`、workspace のメンバー。変数の読み書き用の grpcio と protobuf つき）と pytest |
| `schema` | jsonschema と PyYAML（`schema/` の JSON Schema のテスト） |
| `dev` | すべて（既定） |

バージョンの下限は、テストを実際に実行している最も古いバージョンです（`scripts/verify-python.py` が、uv の lowest-direct の解決で下限そのものを Python 3.11 にインストールして確認します）。上限は付けていません。

vehicle-sil が最上位のプロジェクトのとき、configure は次の順に Python を探します。

1. リポジトリの `.venv/bin/python`（`uv sync` が作る）が Python 3.11 以上で、必要なモジュールを import できれば、それを使います。
2. そうでなければ、`find_package(Python3 3.11)` で探します（有効にした仮想環境が先。`-DPython3_EXECUTABLE=` で指定できる）。
3. 見つかった Python にモジュールがなければ、`PATH` の `python3` と `python` を、`PATH` の順に試します（`-DPython3_EXECUTABLE=` で指定したときは試しません）。

Windows の `find_package(Python3)` は、レジストリに登録された Python（Windows 用にインストールしたもの）を `PATH` より先に見つけるので、MSYS2 の Python（`mingw-w64-ucrt-x86_64-python-yaml` などをインストールしたもの）は、この順で見つかります。処理は `cmake/VehicleSilPython.cmake` にあります。

| 場合 | 動作 |
|---|---|
| `uv sync` で `.venv` を作った、`.venv` にパッケージを追加した | 生成器の Python は configure のたびに探し直すので、普通に configure し直せば使います（`--fresh` は不要です）。 |
| FetchContent や `add_subdirectory` で取り込んだ利用側のビルド | vehicle-sil の `.venv` を使わず、利用側の Python（有効にした仮想環境、`Python3_EXECUTABLE`、`PATH`）を使います。 |
| Windows | `.venv` を候補にせず（uv の Python を MinGW のビルドから使うことは確認していないため）、MSYS2 UCRT64 の pacman のパッケージの Python を、今までどおり探して使います。 |

### Python 3 と PyYAML（J1939 の型付きメッセージの生成）

`vehicle_sil_add_j1939_codec()` が、ビルドのときに `tools/j1939_codegen/j1939_codegen.py` を実行します。Python 3.11 以上と PyYAML（`import yaml`）が必要です。configure のときに上の順で探し、PyYAML を import できるかを確認します。なければ、この関数を呼んだ時点で configure が失敗し、試した Python とそれぞれを使わなかった理由（`No module named 'yaml'`、3.11 より古いバージョンなど）と、インストールの方法（uv、pip、OS と MSYS2 のパッケージ）を表示します。Control API のテストを見送るときの configure の表示も、同じように試した Python と理由を表示します。

vehicle-sil のテストと例（`VEHICLE_SIL_BUILD_TESTS`、`VEHICLE_SIL_BUILD_EXAMPLES`。トップレベルのビルドでは既定で ON）はこの関数を使うので、そのビルドには Python と PyYAML が必要です。ライブラリだけを使う利用側（FetchContent など）は、関数を呼ばなければ不要です。

PyYAML は YAML 1.1 で読みます。生成器の読み方は次のとおりです。

| 書いたもの | 生成器の扱い |
|---|---|
| `on`、`off`、`yes`、`no` | 真偽値にせず、文字列として読みます。 |
| 整数 | 10 進（先頭の 0 なし）、16 進、2 進だけを受け付け、8 進と、整数と小数の 60 進をエラーにします。 |
| 同じキーの重複 | エラーにします（マージキーで取り込んだキーの上書きは可）。 |

Python や PyYAML が見つからなかった configure の結果はキャッシュに残りません。インストールしたあとで configure し直せば見つけます。

### aiohttp と PyYAML（ブラウザの GUI のサーバー）

GUI サーバー（`gui/server`、配布名 `vehicle-sil-gui`、[gui.md](gui.md)）は aiohttp と PyYAML（ダッシュボードのファイルの読み込み）に依存します。aiohttp は、HTTP のサーバー、WebSocket、テストで使うクライアントを 1 つで持ち、asyncio の Windows の既定のイベントループ（子プロセスを動かせる ProactorEventLoop）で動き、Windows（3.11 以降）の wheel があります。C++ のビルドは使いません。

ECU の変数を読み書きするときだけ、ランチャーの Control Server のクライアントとして grpcio と protobuf を使います（任意の依存 `vehicle-sil-gui[variables]`。`gui` のグループがインストールします）。なくても GUI サーバーは動き、変数だけが使えないと理由を表示します。この任意の依存だけでは足りず、スタブが必要です（リポジトリの中で `uv sync` して動かすか、スタブを同梱したビルド済みのツールを使うのが前提。スタブは Python のパッケージには入っていません）。gRPC のスタブは、起動のたびに `proto/` から grpcio-tools（`control` のグループ）で作るか、`python -m vehicle_sil_control.generate` が作ったもの、`VEHICLE_SIL_CONTROL_STUBS` のディレクトリのものを使います。

| ライブラリ | ライセンス |
|---|---|
| aiohttp | Apache-2.0（同梱の llhttp は MIT） |
| aiohttp が依存する multidict、yarl、frozenlist、aiosignal、propcache | Apache-2.0 |
| aiohappyeyeballs | PSF-2.0 |
| attrs | MIT |
| idna | BSD-3-Clause |

これらは vehicle-sil のリポジトリには含めず、uv がインストールします。

### Vector XL Driver Library（Vector XL CAN Bridge、Windows のみ）

XL Driver Library は vehicle-sil に含めず、サブモジュールにもしていません。利用者が Vector から入手し、`VEHICLE_SIL_VECTOR_XL_DIR` で場所を指定します。使うファイルは `vxlapi.h`、`vxlapi64.lib`（32 bit なら `vxlapi.lib`）、実行時の `vxlapi64.dll` です。見つからなければ、ブリッジ（`vehicle_sil::vector_xl` と `vehicle_sil_vector_xl_bridge`）はビルドされません。

使う API はクラシック CAN 用です（`XL_INTERFACE_VERSION`、`XL_BUS_TYPE_CAN`）。

| 用途 | API |
|---|---|
| ドライバ | `xlOpenDriver`、`xlCloseDriver`、`xlGetDriverConfig`、`xlGetErrorString` |
| ポート | `xlOpenPort`、`xlClosePort`、`xlActivateChannel`（`XL_ACTIVATE_NONE`）、`xlDeactivateChannel` |
| 設定 | `xlCanSetChannelBitrate`（init 権限が必要、15000〜1000000 bit/s）、`xlCanSetChannelMode(tx=1, txrq=0)`（送信レシートを有効にし、送信要求の通知を無効にする。init 権限が必要。権限があるときだけ呼ぶ） |
| 送受信 | `xlCanTransmit`（`XL_TRANSMIT_MSG`、DLC 0〜8）、`xlReceive`（`XL_RECEIVE_MSG`、1 回に 1 件）、`xlSetNotification` と `WaitForSingleObject` |

上の API を呼ぶのは `bridges/vector-xl/src/xl_backend_vector.cpp` だけです。このファイルは、vehicle-sil 独自の内部インターフェース（`src/xl_backend.hpp`。名前、型、値は vehicle-sil のもの）を XL Driver Library の呼び出しで実装します。行うのは型と値の変換だけで、判断も状態も持ちません。Windows で XL Driver Library が見つかったときだけビルドされます。

ブリッジの判断と状態（イベントの分類、オーバーランの扱い、受信スレッド、開閉の順序と後始末、エラーの組み立て）は `src/xl_driver.cpp` にあります。内部インターフェースだけに依存するので、どのプラットフォームでもビルドされます。

テストは、内部インターフェースを実装したテスト専用の偽物（`tests/vector_xl/fake_xl/`）と `src/xl_driver.cpp` をリンクして実行します。Vector のヘッダーもライブラリも使いません。リポジトリには、Vector のヘッダーもそのコピーも含めていません。

### Linux のカーネルの API（イーサネットのブリッジの `tap` と `packet`、Linux のみ）

外のライブラリは使いません。使うのはカーネルと libc のヘッダーだけで、ビルドにも実行にも、インストールするものはありません。呼ぶのは `libs/eth_link/src/linux` だけです（`vehicle_sil::eth_link` の中。Linux でだけビルドします）。

| 用途 | API |
|---|---|
| `tap` | `/dev/net/tun` を開き、`ioctl(TUNSETIFF)`（`IFF_TAP`、`IFF_NO_PI`。`--tap-create` では `IFF_TUN_EXCL` も）、`read` と `write` が 1 回に 1 フレーム |
| `packet` | `socket(AF_PACKET, SOCK_RAW)` をインターフェースに `bind`（`ETH_P_ALL`）、`recvmsg` と `send`、`PACKET_AUXDATA`（外された VLAN タグ）、`PACKET_ADD_MEMBERSHIP` の `PACKET_MR_PROMISC`、`PACKET_STATISTICS`（捨てた数） |
| インターフェース | `if_nameindex`、`if_nametoindex`、`ioctl(SIOCGIFFLAGS)`、`ioctl(SIOCGIFHWADDR)`（リンクの種類） |
| 受信スレッド | `poll`、`eventfd`（止める合図） |

実装が前提にしているカーネルの挙動は、次のとおりです（テスト `ethernet_bridge.netns.*` が確認します）。

- TAP デバイスに書いたフレームは、書いた側の読み取りに戻らない。
- packet ソケットは、自分が送信したフレームを受信せず、同じインターフェースのほかのソケットが送信したフレームは受信する。
- 受信したフレームの外側の VLAN タグは、フレームのバイト列から外され、`PACKET_AUXDATA` で渡される。

テストしていない前提もあります。グループのアドレス宛てに送信したフレームを、カーネルが同じ PC の受信側のために渡し直したコピー（`PACKET_LOOPBACK`）は、送信したときのコピーと重なるので、`packet` は捨てます。

### Npcap と libpcap（イーサネットのブリッジの `npcap` と `pcap`、実行時だけ）

どちらも vehicle-sil に含めず、サブモジュールにもせず、配布物にも入れません。ビルドにも使いません（ヘッダーもインポートライブラリも不要です）。利用者が PC にインストールしたものを、ブリッジが実行時に読み込みます。インストールされていなければ、ブリッジはインストールの方法を表示して終了します（[ethernet-bridge.md](ethernet-bridge.md) の「`npcap` と `pcap`」）。

| OS | ライブラリ | ライセンス |
|---|---|---|
| Windows | [Npcap](https://npcap.com) | 使える範囲と再配布の条件は Npcap のライセンスにあり、vehicle-sil のドキュメントはその内容を解釈しません。 |
| Linux | ディストリビューションの [libpcap](https://www.tcpdump.org) | BSD ライセンス |

リポジトリには、どちらのヘッダーもそのコピーも含めていません。関数の型、読む 3 つの構造体、結果の番号は、公開されている呼び出しの形に合わせて vehicle-sil が自分で宣言しています（`libs/eth_link/src/pcap/pcap_api.hpp`。名前は vehicle-sil のもの）。

ライブラリを読み込んで関数のアドレスを取得するのは `libs/eth_link/src/pcap/pcap_loader_win.cpp`（`LoadLibraryExW`、`GetProcAddress`。ライブラリが使う DLL は、ライブラリと同じディレクトリとシステムのディレクトリからだけ探させます）と `pcap_loader_posix.cpp`（`dlopen`、`dlsym`）だけです。待ち方（Windows はライブラリのイベントと `WaitForMultipleObjects`、Linux はライブラリのディスクリプターと `poll`）と、接続の名前（Windows の `GetAdaptersAddresses`。`iphlpapi` をリンクします）もここにあります。

pcap のインターフェースの判断と状態（開く順、受信スレッド、エラーの文、インターフェースの選び方）は `src/pcap/pcap_link.cpp` にあります。関数の表だけに依存するので、どのプラットフォームでもビルドします。

| 用途 | ライブラリの関数 |
|---|---|
| 一覧 | `pcap_findalldevs`、`pcap_freealldevs`、`pcap_lib_version` |
| 開く | `pcap_create`、`pcap_set_snaplen`、`pcap_set_promisc`、`pcap_set_timeout`、`pcap_set_immediate_mode`、`pcap_activate`、`pcap_datalink`、`pcap_setnonblock`、`pcap_close`、Npcap では `pcap_open` も（アダプターはこれで開きます。自分の送信を受けない指定が、この関数にしかないためです。libpcap には求めません） |
| 送受信 | `pcap_next_ex`、`pcap_sendpacket`、`pcap_geterr`、`pcap_stats` |
| 待つ | Windows は `pcap_getevent`、Linux は `pcap_get_selectable_fd` |

どれか 1 つでもなければ、ブリッジは開かずに、ライブラリのバージョンと足りない関数の名前を表示します。

テストは、同じ名前の関数を持つテスト専用の偽の共有ライブラリ（`tests/eth_link/fake_pcap`。vehicle-sil の宣言からビルドします）を、本物と同じ経路で読み込みます。本物の libpcap に対しては、Linux のテスト `ethernet_bridge.netns.pcap` が確認します（インストールされていない PC ではスキップします）。本物の Npcap に対するテストはありません。

実装が前提にしている libpcap（Linux）の挙動は、次のとおりです（`ethernet_bridge.netns.pcap` が確認します）。

- 送信したフレームは、送信したハンドルの受信に戻らない。
- 受信したフレームの VLAN タグは、元の位置に戻して渡される。
- ブロックしない読み取りでは、インターフェースが下がったことも、なくなったことも、ライブラリは通知しない（ブリッジがカーネルに問い合わせます）。

実装が前提にしている Npcap の挙動は、次のとおりです（本物に対して手で確認しました。範囲は [ethernet-bridge.md](ethernet-bridge.md) の「確認したことと、確認していないこと」）。

- 構造体の形が合う。
- ライブラリのイベント（`pcap_getevent`）は、受信したフレームを全部読むと、ライブラリが下げる（vehicle-sil は自分で下げない）。
- 送信したフレームは、送信したハンドルの受信に戻る。
- `pcap_open` に `PCAP_OPENFLAG_NOCAPTURE_LOCAL` を付けて開いたハンドルには戻らず、ほかのハンドルが送信したフレームは受信する。

pcap のインターフェースはこの指定で開き、「戻さない」と申告します。`pcap_open` に渡す指定の番号（プロミスキャス、自分の送信を受けない、すぐ渡す）は、vehicle-sil が自分で宣言しています。

確認していない前提は、次の 2 つです。

- `pcap_stats` の捨てた数が、開いてからの累計であること。
- Npcap の `pcap_open` の「すぐ渡す」の指定が、`pcap_set_immediate_mode` と同じ速度でフレームを渡すこと。

## 取り込み方（`cmake/VehicleSilDependencies.cmake`）

`add_subdirectory(... EXCLUDE_FROM_ALL)` で追加します。リンクされたターゲットだけがビルドされます。

### SIL Kit のコピーとパッチの適用

SIL Kit は、サブモジュール `external/sil-kit` をビルドツリーの `external/sil-kit-patched/` にコピーし（ネストしたサブモジュールも含めて `.git` を除く）、`patches/sil-kit/*.patch` をファイル名の順に `git apply` してから追加します。パッチがなければ、コピーせずにサブモジュールを直接追加します。

コピーとパッチの適用は、パッチかサブモジュールのファイル（`.git` を除く）の内容が前回の configure と違うときだけやり直します。判定には、全ファイルの SHA-256 を並べたスタンプ（`sil-kit-patched.stamp`）を使います。Git の状態は見ないので、サブモジュールが Git のチェックアウトでなくても動きます。やり直すときはコピーしたファイルをすべて touch するので、そのライブラリは全体が再ビルドされます。

パッチの追加、変更、削除と、サブモジュールのファイルの変更（チェックアウトの切り替えや手元での編集）では、CMake が自動で configure をやり直します。

SIL Kit のバージョン情報の Git ハッシュ（`SilKit::Version::GitHash()`）は `UNKNOWN` になります。SIL Kit は自分の `.git/HEAD` を探しますが、サブモジュールの `.git` はファイルなので、コピーしなくても同じです。

パッチを適用できなければ、configure は失敗します。`git apply` を使うので Git が必要です。親の Git リポジトリや、継承した `GIT_DIR` などの環境変数の影響は受けません。ビルドツリーがリポジトリの中にあっても、`GIT_CEILING_DIRECTORIES` で、コピーの上のリポジトリを Git に見せません。この値は、Git 自身が出すパスの形で渡します（MSYS2 の `git` は `/d/a/...` の形しか読まないため）。

失敗の文には、使った Git のパスとバージョン、`GIT_CEILING_DIRECTORIES` の値、Git がコピーの周りに見つけたリポジトリ（あれば、パッチのパスをそのリポジトリからの相対と取り違えて飛ばしたということです）、パッチが変えるファイルの改行（LF か CRLF か）が表示されます。

`--ignore-whitespace` で適用するので、CRLF でチェックアウトしたサブモジュール（Windows の `core.autocrlf`）にも適用できます。パッチファイル自体は `.gitattributes` で改行を変換しません。

### 利用側が `SilKit::SilKit` を定義しているとき

利用側がすでに `SilKit::SilKit` を定義している場合は追加しません。確認しているのは v5.0.8 だけです。それより古い SIL Kit には、vehicle-sil が頼っている挙動の一部がありません（「[混ぜてよい SIL Kit のバージョン](#混ぜてよい-sil-kit-のバージョン)」）。

利用側の SIL Kit には「SIL Kit へのパッチ」が適用されていません。パッチは、下の表のプロセスの SIL Kit に適用されているときだけ効きます。ほかのツール（CANoe、SIL Kit に付いてくる `sil-kit-registry` など）がそのプロセスを受け持つ構成でも、同じことが起きます。

| パッチ | 適用されている必要があるプロセス | ないと起きること |
|---|---|---|
| 0001 | LIN のヘッダーを送る Master | 同じ LIN ID に複数の `LinSlave` が交代で応答する構成（診断の 0x3D など）で、2 つ目の応答の後、その ID のヘッダーが送られなくなる |
| 0002 | CAN のコントローラーを持つ participant | SIL Kit のスレッドの外からコントローラーを呼ぶ所（閉じるとき、実時間のポートの送信、Vector XL CAN Bridge）が、ネットワークシミュレーターの参加・退出と重なると、データ競合になる（まれ） |
| 0003 | registry | 同じ名前で拒否された participant が切断した後、先にいた同じ名前の participant へ、直接接続できない participant が中継で接続できなくなる |
| 0004 | ネットワークシミュレーターを作る participant（`vehicle_sil_can_bus_simulator`） | まれに、ネットワークが告知されず、コントローラーが全部 trivial simulation のままになる。ビットレートとモードがシミュレーターに届かず、エラーも警告も出ない |
| 0005 | registry | participant が終わった直後に別の participant が参加すると、まれに、5 秒（`Middleware.ConnectTimeoutSeconds`）待ってから「Timeout during connection handshake with the SIL Kit Registry」で失敗する。作り直せば接続できる |
| 0006 | coordinated の participant（実行の ECU） | 実行が required にしていない participant（`--name` の打ち間違いなど）が、開始の途中で Error から先へ進むことがある。`ParticipantRuntime::start()` は失敗にするが、SIL Kit がその participant の停止を受け付けないことがあり、最大で `shutdown_timeout` の間残って、その間は全 participant の仮想時間が 0 のままになる。まれに、`start()` が戻り、その participant は 1 ステップも実行せず、何も報告しない |
| 0007 | SIL Kit のヘッダーでコンパイルし、lifecycle を始めるすべてのプログラム（変更はヘッダーだけの C++ API にある）。`vehicle_sil_launch serve` のように participant を次々に作っては破棄するプロセスで問題になる | MinGW では、lifecycle を始めた participant 1 つごとに Win32 の `Event` ハンドルが 1 つ増える（「[MinGW GCC（`winpthreads`）: `std::recursive_mutex` の破棄で Win32 の `Event` ハンドルがリークする](#mingw-gccwinpthreads-stdrecursive_mutex-の破棄で-win32-の-event-ハンドルがリークする)」） |

`vehicle_sil_launch run` が立てる registry と、vehicle-sil のビルドで作った実行ファイルには、全部のパッチが適用されています。

### SIL Kit のビルドの設定

上流の公開インクルードディレクトリを SYSTEM 扱いにします。上流ヘッダーの警告で、vehicle-sil のターゲットが `-Werror` 付きのビルドに失敗しないようにするためです。

上流のオプションは関数スコープの通常変数で上書きします（CMP0077）。利用側のキャッシュは変えません。

| ライブラリ | 設定 | 理由 |
|---|---|---|
| SIL Kit | `SILKIT_BUILD_DEMOS/TESTS/DOCS/DASHBOARD=OFF` | 不要なものを作らないためです。 |
| SIL Kit | `SILKIT_BUILD_UTILITIES=ON` | 統合テストに `sil-kit-registry` が必要なためです。 |
| SIL Kit | `SILKIT_BUILD_STATIC` は `VEHICLE_SIL_STATIC` と同じ（既定 OFF） | `VEHICLE_SIL_STATIC=ON` で、SIL Kit を静的なライブラリにするためです（下の段落）。 |

`VEHICLE_SIL_STATIC=ON` では、SIL Kit を静的なライブラリにし、`SilKit::SilKit` の `INTERFACE_LINK_OPTIONS` に C++ のランタイムの静的なリンク（Linux: `-static-libstdc++ -static-libgcc`、MinGW: `-static`）を付けます。SIL Kit をリンクする実行ファイル（利用側の ECU も）に適用されます。glibc は共有ライブラリのままです（[build.md](build.md) の「1 つの実行ファイルにする（`VEHICLE_SIL_STATIC`）」）。

## 使っているターゲット

| ターゲット | 内容 |
|---|---|
| `SilKit::SilKit` | SIL Kit 本体で、既定では共有ライブラリ（`VEHICLE_SIL_STATIC=ON` では静的）、Debug では `libSilKitd.so` / `SilKitd.dll` になり、出力先は `${CMAKE_BINARY_DIR}/$<CONFIG>` です。 |
| `sil-kit-registry` | レジストリの実行ファイルで、SIL Kit を静的にリンクしているので、共有ライブラリは不要です。 |
| `rapidyaml` | SIL Kit が `ThirdParty/rapidyaml` からビルドする YAML/JSON パーサー（MIT。静的ライブラリ）で、`vehicle_sil::can_netsim` と `vehicle_sil::launch` が設定ファイルの読み込みに使い、`vehicle_sil::core` が、渡された SIL Kit の構成に vehicle-sil の既定を追加するのに使います（`merged_participant_configuration()`）。 |

SIL Kit をサブモジュールから追加しないとき（利用側が `SilKit::SilKit` を定義済み）は `rapidyaml` のターゲットがないので、`vehicle_sil::can_netsim`、`vehicle_sil_can_bus_simulator`、`vehicle_sil::launch`、`vehicle_sil_launch` を作りません。`vehicle_sil::core` は作りますが、渡された SIL Kit の構成をそのまま使います（既定を追加せず、名前、registry の URI、`DynamicSimulationStep` の検査もしない。[internals/core.md](internals/core.md) の「SIL Kit の設定の既定」）。

Windows では `SilKit.dll` が実行ファイルと別のディレクトリに出力されます。`vehicle_sil_copy_runtime_dlls(<target>)` を呼ぶと、ビルド後に DLL が実行ファイルの隣へコピーされます。

## 使っている API

| ライブラリ | 用途 | API |
|---|---|---|
| SIL Kit | 参加 | `Config::ParticipantConfigurationFromString`、`CreateParticipant(config, name, registryUri)` |
| SIL Kit | 構成のキー | `Middleware.AcceptorUris`、`Middleware.EnableDomainSockets`、`Experimental.TimeSynchronization.DynamicSimulationStep`（experimental。既定の構成でいつも `false`） |
| SIL Kit | lifecycle | `CreateLifecycleService({OperationMode::Autonomous / Coordinated})`、`StartLifecycle`、`Stop`、`State`、`ReportError`、`SetStartingHandler`、`SetCommunicationReadyHandler`（コントローラーを始める所。`ParticipantRuntime::add_communication_ready_handler()`）、`SetCommunicationReadyHandlerAsync` と `CompleteCommunicationReadyHandlerAsync`（始まる前の窓。`SimulationController::set_start_window()`） |
| SIL Kit | 時刻同期 | `ILifecycleService::CreateTimeSyncService`、`ITimeSyncService::SetSimulationStepHandlerAsync` と `CompleteSimulationStep`（ハンドラの最後で完了させる） |
| SIL Kit | system controller（experimental） | `Experimental::Participant::CreateSystemController`、`SetWorkflowConfiguration`、`AbortSimulation` |
| SIL Kit | LIN（`vehicle_sil::lin_silkit`） | `CreateLinController`、`ILinController::Init`、`SetFrameResponse`、`SendFrame`（Master が送るフレーム）、`SendFrameHeader`、`GoToSleep`、`GoToSleepInternal`、`Wakeup`、`WakeupInternal`、`AddFrameStatusHandler`、`AddGoToSleepHandler`、`AddWakeupHandler` |
| SIL Kit | LIN dynamic response（experimental） | `Experimental::Services::Lin::InitDynamic`、`AddFrameHeaderHandler`（`InitDynamic` の後に登録する）、`SendDynamicResponse` |
| SIL Kit | publish/subscribe（`vehicle_sil::control_silkit`、examples/virtual_time） | `CreateDataPublisher`（履歴 0。hello だけ履歴 1）、`CreateDataSubscriber`、`IDataPublisher::Publish`、`PubSubSpec`（トピックとメディアタイプ。ラベルは使わない） |
| SIL Kit | 他参加者の監視 | `CreateSystemMonitor`、`AddParticipantStatusHandler`（`SetParticipantConnectedHandler` と `SetParticipantDisconnectedHandler` は使わない。「上流の既知の不具合」を参照） |
| SIL Kit | CAN | `CreateCanController`、`ICanController::Start/Stop/SendFrame/SetBaudRate`（`SetBaudRate` と `Start` は CommunicationReady のハンドラーから）、`AddFrameHandler`、`AddFrameTransmitHandler`、`AddErrorStateChangeHandler`（ネットワークシミュレーターがいるときだけ通知が来る）、`SendFrame` の userContext に送信時刻を入れる（簡易 CAN Network Simulator との取り決め） |
| SIL Kit | Ethernet（`vehicle_sil::eth_silkit`、コントローラーを作るのは `vehicle_sil::peer` の `Ecu::ethernet()`） | `CreateEthernetController`、`IEthernetController::Activate/Deactivate/SendFrame`（`Activate` は CommunicationReady のハンドラーから）、`AddFrameHandler`（RX だけ）、`AddFrameTransmitHandler`、`AddStateChangeHandler` |
| SIL Kit | Ethernet（`vehicle_sil::ethernet_bridge_core`） | `CreateEthernetController`、`IEthernetController::Activate/Deactivate/SendFrame`（`Activate` は CommunicationReady のハンドラーから）、`AddFrameHandler`（RX の向きだけ）、`AddFrameTransmitHandler`（`Transmitted` 以外の確認だけ） |
| SIL Kit | Network Simulator（experimental、`vehicle_sil::can_netsim`） | `Experimental::Participant::CreateNetworkSimulator`、`INetworkSimulator::SimulateNetwork/Start`、`ISimulatedNetwork`、`Can::ISimulatedCanController`、`Can::ICanEventProducer::Produce`（フレーム、送信確認、状態の変化） |
| SIL Kit | プロセス内レジストリ（`Simulation`、`vehicle_sil_launch run`） | `Vendor::Vector::CreateSilKitRegistry`、`StartListening` |

## 実装が前提にしている上流の挙動

上流を更新するときは、次の挙動が変わっていないかを確認してください。どれも SIL Kit の挙動です。

### participant と lifecycle

`ParticipantStatus` は履歴長 1 で配信されます。後から参加した participant も、既存の参加者の最新状態を受け取れます。`ParticipantRuntime::wait_for_peer` はこれを使います。

購読が確立する前に送信したフレームは、受信されないことがあります。そのため、コントローラは `ParticipantRuntime::start()` の前に作ります。

コントローラーの最初の設定（CAN の `SetBaudRate` と `Start`、LIN の `Init`、Ethernet の `Activate`）は、CommunicationReady のハンドラーで呼ぶよう、SIL Kit のドキュメントが勧めています。vehicle-sil のポートとブリッジは、そこで呼びます（`ParticipantRuntime::add_communication_ready_handler()`）。このハンドラーは、coordinated でも autonomous でも、participant の通信スレッドで、Running（最初のステップ）より前に呼ばれます（autonomous の開始も、`StartLifecycle()` が通信スレッドに積みます）。coordinated の participant では、必須の participant が全員サービスを作り終えた後なので、ネットワークシミュレーターが模擬するネットワークも見つかっています。autonomous の participant には、その保証がありません。テスト `core.communication_ready_handlers_that_were_added_run_in_order_before_the_one_that_was_set_and_the_first_step`（スレッドと順）と `can_netsim.simulator_learns_the_bit_rate_and_the_start_whichever_joins_first`（起動の順によらずシミュレーターが受信すること）が確認します。

`sil-kit-registry --listen-uri silkit://127.0.0.1:0 --generate-configuration <file>` は空きポートで待ち受け、実際の URI を含む設定ファイルを原子的に書き出します。統合テストはこれを使って、並列実行でもポートが衝突しないようにしています。

同じ名前の participant を拒否するとき、registry は「A participant with the same name ... already exists in the simulation」という文のエラーを返します。`create_participant()` は、エラーの文にこの「already exists in the simulation」があるときだけ、待ってやり直します（SIL Kit には、エラーの種類を見分ける公開の API がありません）。拒否のしかたと文は registry のバージョンによって違うので、ほかのバージョンの registry（ほかのツールが立てたもの、公式の配布物のもの）では文が合わないことがあります。そのときは、待たずに、SIL Kit のエラーのまますぐ失敗します（`name_in_use_wait` は役に立ちません。同じ名前ですぐに起動し直すときは、少し間を空けます）。拒否された後の作り直しは、registry が同じ名前を毎回拒否すること（4.0.55 以上）にも頼っています。それより古い registry は 2 回目の接続を拒否しないので、同じ名前の participant が黙って 2 つ入ります（変更履歴から。古いバージョンではテストしていません）。上流を更新するときは、この文が変わっていないかを確認します（テスト `core.participant_runtime_waits_while_the_registry_still_has_the_name`、`core.participant_runtime_says_how_long_the_name_was_refused`）。

`ReportError` で Error になった participant は、`Stop` か abort されるまで Error のままで、`StartLifecycle` の future も完了しません。`Stop` は Error から Shutdown に進めます。公開のヘッダーとドキュメントは、`Stop` を呼べるのは Running（か Paused）のときとしていて、Error からの `Stop` は保証の外です（Error については、終わらせるしかない、とだけ書いてあります）。`ParticipantRuntime::stop()` は Error でも `Stop` を呼ぶので、上流がこの前提を検査するようになると、Error の participant が Shutdown に進まなくなります。上流を更新するときに確認します（テスト `core.virtual_time_stop_during_a_failing_step_leaves_through_error`）。

workflow configuration は、送信した時点で接続している参加者だけが受信します。そのため `SimulationController` は、必要な参加者のステータスが見えてから送信します（`sil-kit-system-controller` と同じ方針）。

### 時刻同期

時刻同期した participant は、自分の次のステップ時刻がほかの全員の次のステップ時刻以下のときだけ進みます（`TimeConfiguration::OtherParticipantHasLowerTimepoint`）。「最も遅いピアを待つ」と `SimulationController` の stop_at の保証は、これに基づいています。

構成の `Experimental.TimeSynchronization.DynamicSimulationStep` は 3 つの値を取ります。`true` の participant は、自分のステップを全員の最小の幅に縮め、ほかの participant にもそうするよう求めます。キーがない participant は、`true` の participant が 1 つでもいれば、それに従って縮みます。`false` の participant は縮みません。vehicle-sil のステップは固定の幅を前提にしているので（ステップの格子、`TickScheduler`、時間ゲート）、既定の構成（`default_participant_configuration()`）はいつも `false` にします。自分の構成を渡した participant にも、キーがなければ `false` を追加し、`true` なら participant を作る前にエラーにします（`merged_participant_configuration()`）。それでも SIL Kit がステップを縮めたとき（rapidyaml のないビルドで、キーのない構成か `true` の構成を渡したとき）は、`ParticipantRuntime` が、渡されたステップの幅が自分の幅と違うことを見つけて、そのステップを失敗にします（[internals/core.md](internals/core.md) の「SIL Kit の設定の既定」）。experimental のキーなので、上流を更新するときは、名前と意味が変わっていないかを確認します。テスト `core.virtual_time_keeps_its_step_when_another_participant_asks_for_dynamic_steps` が、`true` の participant と一緒に動かして、既定の構成が有効であることを確認します（キーがなくなるか、意味が変わると落ちます）。`core.virtual_time_keeps_its_step_with_a_configuration_of_its_own` が、キーのない自分の構成でも同じであることを確認します。

時間ゲートは、非同期版のステップを完了させないことで仮想時間を止めます（[internals/virtual-time.md](internals/virtual-time.md)）。SIL Kit の HealthCheck（構成の `HealthCheck`）は、ステップのハンドラーの呼び出しにかかった時間だけを測るので、保留しているステップを時間切れにしません。公式のドキュメントが保証しているのは `Pause()` の間のことだけで、完了させていない非同期のステップのことは書いていません。変わると、HealthCheck を付けた構成では、一時停止が警告か Error になります。

時刻同期した participant が送信するメッセージのタイムスタンプは、送信時のステップの時刻です。

時刻同期していない participant が送信するメッセージのタイムスタンプは `std::chrono::nanoseconds::min()` です（`TimeProvider` の既定）。時刻同期している受信側の SIL Kit は、受信したメッセージのこの値を自分の今の時刻に置き換えます（`SilKitLink::DistributeRemoteSilKitMessage`）。最初のステップの前は今の時刻もこの値なので、そのまま残ります。`SilKitCanPort`（仮想時間）と `SilKitEthPort` は負の時刻を見分け、受信側の最新のステップの時刻（最初のステップの前なら 0）に寄せます（[internals/can.md](internals/can.md) の「SIL Kit のポート」、[internals/ethernet.md](internals/ethernet.md) の「時刻同期に参加していない送信側」）。テスト `j1939_silkit.j1939_on_virtual_time_receives_a_bam_from_a_participant_without_time_synchronisation*` が、始まる前と始まった後の両方を確認します。

### publish/subscribe（Agent と Hub の間）

Agent と Hub の間を publish/subscribe で運ぶ層（`vehicle_sil::control_silkit`）と始まる前の窓は、次の挙動を前提にしています。テスト `control_silkit.*` と `launch_control.*` が、まとめて確認します（どれかが変わると、`HubTransportStats::late_messages` が 0 でなくなるか、窓の中の書き込みが時刻 0 のステップに間に合わなくなります）。

`IDataPublisher::Publish` は、呼んだスレッドでデータをコピーし、送信を participant の I/O スレッドの待ち行列に積むだけです。どのスレッドから呼んでも安全で、1 つの相手には積んだ順に届きます。

ステップの完了（`CompleteSimulationStep`）も同じ待ち行列に積まれます。そのため、ステップの中で送信したメッセージは、そのステップの次の時刻の通知より前に相手に届きます。上の「時刻同期」の進む条件と合わせて、受信側のステップ P が始まる時点で、P より前に始まった送信側のステップのメッセージは全部届いています（ちょうど P のステップの分は決まりません）。ステップの外（別のスレッド、受信のハンドラー）から送信したメッセージには、この関係がありません。

受信のハンドラーは、participant の I/O スレッドで、届いた順に 1 つずつ呼ばれます。ステップのハンドラーも同じスレッドです。時間ゲートがステップを保留している間も、I/O スレッドは空いていて、メッセージは届きます。

`CreateDataPublisher` は、発行者の登録を participant の I/O スレッドに任せて、その完了を待ちます。そのため、そのスレッド（受信のハンドラーやステップのハンドラー）からは呼べません（自分を待って止まります）。`HubTransport` は、発行者を全部コンストラクターで作ります。

ラベル（文書にある規則）: 種類が Optional のラベルは、相手にそのキーがないときと、値が同じときに合い、値が違うと合いません。合わない発行者と購読者は通信しません。Hub のコマンドの、Agent のグループごとの発行者の正しさは、これを前提にしています。Agent は、自分のグループの発行者と、ラベルのない発行者から受け取ります（テスト `control_silkit.a_command_reaches_the_agents_of_its_group_only_and_in_the_order_written`、`control_silkit.a_last_word_is_answered_also_for_a_session_that_has_ended_or_was_never_connected`）。

送信（実装を読んで頼っていること。文書にはない）: 発行者はそれぞれ自分の link を持ち、メッセージはその発行者に合った購読者を持つ participant にだけ送られます。ほかの participant には何も届きません。グループの速さだけがこれに頼ります。SIL Kit が全 participant に送って受け手の側で絞るようになっても、コマンドはグループの前と同じく届きます。

履歴 1 の発行者が最後に送信したメッセージは、後から購読した相手にも届きます（発行者の participant が接続している間）。participant を始める前に送信した hello も届きます。

Coordinated の participant の CommunicationReady のハンドラーは、必須の participant が全員 ServicesCreated になり、全員から返事が届き、自分の publish/subscribe の購読が確認された後で呼ばれます（ほかの participant は、その時点でまだ CommunicationInitializing の途中のことがあります。コマンドは受け取れます）。相手は、購読の通知を受けると、履歴（hello）を送信してから確認を返すので、必須の ECU が始める前に送信した hello は、窓が開く前に届いています。非同期のハンドラーでは、`CompleteCommunicationReadyHandlerAsync` を呼ぶまで、誰も ReadyToRun に進まず、どのステップも始まりません。その間、publish/subscribe は届きます。`CompleteCommunicationReadyHandlerAsync` は、ハンドラーが呼ばれる前に呼ぶと無視されます（`ParticipantRuntime` が、呼ばれた後にだけ渡します）。

始めたライフサイクルを持つ participant が、Shutdown せずに接続を切ると、ほかの participant にはその状態が Error として届きます（`HubTransport` が、いなくなった Agent の接続を終えるのに使います）。

participant の構成の `EnableMessageAggregation` は、既定の Off のままであることを前提にしています（On にした participant は、データのメッセージを溜めてから送信するので、上の順序の前提が変わります）。

participant を壊すと、まだソケットに書いていない送信は捨てられます（同じ相手への前の送信を書いている途中なら、その後ろに積んだ送信は書かれません）。そのため、Agent の「最後の通知」は、Hub の返事を待ってから participant を壊します（`AgentTransport` のデストラクター。internals/control.md の「接続の終わり」）。participant が生きている間は、積んだ送信は順に書かれます。

### スレッドと順序の保証

スレッドと順序について、公式のドキュメントが保証していることと、実装を読んで頼っていることを分けて書きます。

**保証されていること**: 受信のハンドラーとステップのハンドラーは、participant の 1 つの通信スレッドで呼ばれ、ステップのハンドラーが動いている間はほかのハンドラーが呼ばれません。ブロッキング版のステップのハンドラーの中で送信したメッセージは、ハンドラーが終わったときに配信されます。メッセージは、どのスレッドやハンドラーから呼んでも、API を呼んだ順に送信されます（4.0.24 の変更履歴。別々のスレッドから並行に呼んだものどうしの順は保証されていません）。

**保証されていないこと（実装を読んで頼っている）**: 非同期版のステップ（`CompleteSimulationStep`）でも、ステップの中の送信が完了の通知より前に出ること（完了の通知が、送信と同じ待ち行列を通ること）。`Publish` と `SendFrame` をどのスレッドから呼んでもよいこと（順の保証は別のスレッドからの呼び出しに触れていて、公式の例にも別のスレッドからの送信がありますが、スレッド安全性そのものの記述はありません）。上の publish/subscribe の項目と、下の LIN の Master のフレームの項目は、これに頼っています。

「ステップ T が始まる時点で、T より前の時刻のメッセージは全部届いている」のは、送信側がステップのハンドラーの中で送信したときだけの保証です。ほかの SIL Kit のプログラムがステップの外（別のスレッド、受信のハンドラー）から送信したフレームは、受信側のどのステップに入るかが実行ごとに変わりえます（[internals/can.md](internals/can.md)、[internals/ethernet.md](internals/ethernet.md)）。

コントローラーの設定の操作（CAN の `SetBaudRate`、`Start`、Ethernet の `Activate` など）は、どのスレッドから呼んでも安全とは言えません（trivial と detailed の切り替えは通信スレッドで起きます。「SIL Kit へのパッチ」の 0002）。vehicle-sil は、通信スレッドの上（CommunicationReady のハンドラー）で呼びます。

### CAN

`RemoveFrameHandler` は、実行中のハンドラの終了を待ってから戻ります。ハンドラの呼び出しと削除が同じ `recursive_mutex` で直列化されているためです。`SilKitCanPort::close()` はこれを前提に、ハンドラを削除してから受信のバッファを捨てます。

trivial simulation の CAN は、`Start()` の前の `SendFrame` を、警告のログだけで捨てます。送信の確認は来ません（`SimBehaviorTrivial::SendMsg`）。`Start()` はその場で状態を変えます。受信は `Start()` と関係なく届きます。そのため `SilKitCanPort` は、`start()` の前の送信を SIL Kit に渡さずにエラーにし、ブリッジは、コントローラーを始める前に外から受信したフレームを SIL Kit に渡しません。trivial simulation の Ethernet は、`Activate()` の前の `SendFrame` に `ControllerInactive` の確認を返します。

CAN の `SendFrame` の userContext は、ポインタの値（64 ビット）のまま運ばれます。ネットワークシミュレーターの `CanFrameRequest` と送信確認には届き、他の受信者の RX のフレームでは `nullptr` になります（`CanController::ReceiveMsg`）。`SilKitCanPort` は送信時刻を入れ、簡易 CAN Network Simulator がそれを読みます。公式のドキュメントに書いてあるのは、userContext が送信側の送信確認とシミュレーターの要求に届くことまでで、プロセスをまたいで値を運ぶ使い方は保証されていません。SIL Kit の replay が流し直すフレームは userContext が空なので、送信時刻がありません（簡易 CAN Network Simulator は、時刻のない要求を、受け取った時点の自分の最後のステップの時刻に置き、警告を 1 度表示します。この位置は決定的ではありません）。

### Network Simulator

Network Simulator API（experimental）: あるネットワークの模擬を引き受ける participant が現れると、そのネットワークの CAN コントローラーは詳細シミュレーションに切り替わり、送信の要求、ボーレート、モードの変更をその participant に送信します。コントローラーの状態（`Started` など）は、シミュレーターが返す状態の変化で初めて変わります。`ControllerDescriptor` は見つけた順の通し番号で、決定的ではありません。

コントローラーがシミュレーターを見つける前に呼んだ `SetBaudRate()` と `Start()` は、trivial simulation で処理され、見つけた後もシミュレーターに送り直されません。すでに見つけているシミュレーターは、コントローラーを作った時点で同じスレッドから通知されます。vehicle-sil のポートは、どちらも CommunicationReady のハンドラーで呼ぶので、coordinated の participant の設定は、起動の順によらずシミュレーターに届きます。autonomous の participant（途中から入るもの、ブリッジ）と、コントローラーを作ってすぐ呼ぶほかのプログラムの設定は、届くとは限りません。そのため、簡易 CAN Network Simulator は、ビットレートが届かなければ `--default-bitrate` を使い、モードを一度も伝えてこないコントローラーを、始めたものとして扱います（送信を止めるのは、`Started` 以外のモードを伝えてきたコントローラーだけです）。シミュレーションが始まった後の送信は、すべてシミュレーターを通ります。Ethernet も同じ作りです（見つける前の `Activate()` は手元の状態だけを LinkUp にし、2 回目の `Activate()` は何もしません）。

### Ethernet

Ethernet は、Network Simulator がなければ、送信したフレームを同じネットワークのほかの全部のコントローラーに届けます（宛先のアドレスで絞らない）。送信の確認は `SendFrame()` の中で、送信した時刻で返ります。`Activate()` は、その中で状態を LinkUp にします。60 バイトより短いフレームは、受信側のコントローラーが 0 で埋めてからハンドラーに渡します（`EthController::ReceiveMsgInternal`）。`VirtualEthernetBus` はこの振る舞いに合わせてあり、テスト `eth_silkit.records_match_*` が同じ記録になることを確認します。

`EthernetBridge` は、次の挙動を前提にしています（テスト `ethernet_bridge.silkit_*` が、埋めることと、自分の送信が RX のハンドラーに来ないことを確認します）。

`SendFrame` は、呼び出しの中でフレームのバイト列をコピーします。ブリッジは、外のインターフェースのハンドラーの間だけ有効なバッファーをそのまま渡します。

trivial simulation では、送信したフレームはほかの participant に RX として、自分には TX として届きます。RX の向きだけで登録したハンドラーには、自分の送信は来ません。

trivial simulation では、`Activate()` でコントローラーはすぐ `LinkUp` になり、送信の確認は `SendFrame` の中で同期に呼ばれます。状態は、`LinkUp` なら `Transmitted`、`LinkDown` なら `LinkDown`、`Activate()` の前なら `ControllerInactive` です。

受信側は、60 バイトより短いフレームを 0 で埋めてからハンドラーに渡します（`EthController::ReceiveMsgInternal`）。送信側には最小長の検査がありません（`SilKitEthPort` は、送信する前に自分で 60 バイトまで埋めます。ブリッジは、外から受信したフレームをそのまま渡します）。最大長の検査は、送信側にも受信側にも見当たりません。

`RemoveFrameHandler` は、CAN と同じく、実行中のハンドラーの終了を待ってから戻ります（ハンドラーの呼び出しと削除が同じ `recursive_mutex` で直列化されている）。

コントローラーは、フレームのハンドラーを、ハンドラーの集まりの `recursive_mutex` を持ったまま呼びます（`SynchronizedHandlers::InvokeAll`）。trivial simulation の `SendFrame` は、自分への TX の配信で同じ集まりの `InvokeAll` を通るので、同じ錠を取ります。そのため、外のインターフェースの受信スレッドが呼ぶ `SendFrame` は、SIL Kit の受信スレッドが `EthernetBridge` のハンドラー（その中の `ExternalLink::transmit()`）を終えるまで待ちます。`ExternalLink` の仕様（`transmit()` はブロックせず、ハンドラーの間に `transmit()` の取る錠を持たない）は、これを前提にしています。

### LIN

LIN の trivial simulation では、Slave の応答はヘッダーを受信したハンドラの中で生成されます（`SimBehaviorTrivial::ProcessFrameHeaderRequest`、dynamic response では `LinFrameHeaderEvent`）。ヘッダーのタイムスタンプは Master の送信時刻です。応答のタイムスタンプは Slave の `Now()` なので、決定論的ではありません（`LinMaster` は使いません）。

応答する Slave が設定されておらず、dynamic response の Slave もいないヘッダーに対しては、Master 自身の `SendFrameHeader` の中から同期的に `LIN_RX_NO_RESPONSE` が通知されます。dynamic response の Slave（`LinSlave`）がいると、ヘッダーは常に送信され、応答がなければ状態は通知されません。`LinMaster` はその場合をスロットの終わりに NoResponse とします。

SIL Kit は、ある ID に最初に応答した Slave をその ID の送信元として記録し、取り消す方法がありません。別の Slave が同じ ID に応答すると、Master はヘッダーを送信せずに Error を報告し続けます。

LIN の `GoToSleep()`（Master のみ）は、trivial simulation ではヘッダーなしで go-to-sleep フレーム（ID 0x3C）を送信し、自分をすぐ Sleep にします。`Wakeup()` は、ほかの参加者に方向 RX のパルスを送信し、自分のハンドラを方向 TX で呼び、自分を Operational にします。受信側の状態は変わらないので、`GoToSleepInternal()` / `WakeupInternal()` を自分で呼びます。同じ状態に変えると警告のログが出ます。

Sleep の LIN コントローラは、受信したフレーム（`LinTransmission`）を、go-to-sleep フレームも含めて捨てます。dynamic response のヘッダーイベントと wakeup パルスは、状態によらず届きます。そのため `LinSlave` は、SIL Kit のコントローラを Sleep にしません。

dynamic response の Slave には、Rx の設定と関係なく、すべてのフレームが送信どおりの状態で届きます。`SilKitLinPort` は Ok のもの（go-to-sleep フレームを除く）を渡し、`LinSlave` は購読した ID だけを渡します。

`SendDynamicResponse` は、その ID の応答を `TxUnconditional` とする更新をほかのコントローラに配信します。v5.0.8 の Master は、`SendFrameHeader` でこれを応答するノードとして数えるので、2 つの dynamic response の Slave が同じ ID に一度ずつ応答すると、以後そのヘッダーを送信せずに `LIN_RX_ERROR` を通知していました。パッチ 0001 で、dynamic response のノードを数えないようにしています（「SIL Kit へのパッチ」）。

2 つのノードが同じヘッダーに応答すると、Master には両方の応答が `LIN_RX_OK`（Master が送るフレームなら `LIN_TX_OK`）で届きます。trivial simulation は衝突をエラーにしません（上流のテストにも既知の問題として書かれています）。`LinMaster` は、1 つのスロットに 2 つ目の応答が届いたら Error とします。

Rx の設定と応答のどちらかのチェックサムモデルが `LinChecksumModel::Unknown` だと、trivial simulation はチェックサムモデルを比べません。vehicle-sil の `LinChecksumModel` には Classic と Enhanced しかないので、`SilKitLinPort` が `Unknown` を SIL Kit に渡すことはありません。

Master が送るフレーム（`SendFrame` の `MasterResponse`）は、`SendFrame` の中では送信されません。SIL Kit がヘッダーを Master 自身に配送するとき（I/O スレッドのタスク）に作られ、そこからさらにタスクとして送信されます。Master 自身のフレームの状態（`LIN_TX_OK`）も、そのヘッダーの配送の中で通知されます。タイムスタンプは、そのときの Master の `Now()` です。

SIL Kit の `SendMsg` は、送信を I/O スレッドのタスクとして積むだけです。ブロッキング版のステップハンドラでは、ハンドラが戻るとすぐに次のステップの時刻を通知するタスクが積まれるので、上の 2 段目の送信はその通知より後になります。また、I/O スレッドは 1 回の読み取りで受信した複数のメッセージを続けて処理し（`VAsioPeer::DispatchBuffer`）、その中で次のステップを直接実行することがあります（`ReceiveNextSimTask`）。非同期版では、`CompleteSimulationStep` が完了の処理をタスクとして積み、ステップが完了するまで次のステップを実行しません。`ParticipantRuntime` はこれを使って、Master のフレームを次のステップの時刻の通知より前に送信させています。

## 混ぜてよい SIL Kit のバージョン

確認しているのは v5.0.8 だけです（利用側の `SilKit::SilKit`、ほかの PC の participant、外で立てた registry のどれについても）。SIL Kit は 4.0.5 から、バージョンの違う participant どうしが接続できることを保証していますが、vehicle-sil が頼っている挙動には、それより後のバージョンで入ったものがあります。下は SIL Kit の変更履歴から拾ったもので、古いバージョンと組んでテストしてはいません。

| バージョン | 入った挙動 | 古いバージョンと組むと |
|---|---|---|
| 4.0.24 | メッセージが API を呼んだ順に送信され、直接接続できない相手とは registry が中継する | Control の順序の前提が成り立たず、ファイアウォールの内側の participant と接続できない |
| 4.0.26 | CommunicationReady の前に、購読の確認を待つ | 始まる前の窓と、コントローラーの最初の設定が相手に届く保証がない |
| 4.0.31 | autonomous の participant が、進んだ仮想時間に途中から入れ、coordinated の participant が入ろうとすると abort する | 途中からの参加の動きが違う |
| 4.0.34 | Error になるのは `StartLifecycle` の後だけで、切断した相手が Error として届くのは、lifecycle を始めた相手だけ | いなくなった participant の見つけ方が変わる |
| 4.0.39 | 片方向にしか接続できないとき、反対側から接続し直す | 複数の PC で、片側だけが待ち受けられる構成が接続できない |
| 4.0.55 | registry が、同じ名前の participant を毎回拒否する | registry が古いと、同じ名前の 2 つ目が黙って入る（`create_participant()` の作り直しが、これに頼っています） |
| 5.0.7 | 切断した相手を購読の確認の待ちから除き、`Experimental.TimeSynchronization.DynamicSimulationStep` が入る | 切断した相手がいると、コントローラーの作成が戻らないことがあり、古いバージョンは既定の構成のこのキーを知らない（読み込みがどうなるかは確認していません） |
| 5.0.8 | 角括弧つきの IPv6 のアドレスに対応し、Stopping を通らない停止でも止まる | IPv6 の registry の URI が使えないことがある |

## 上流の既知の不具合

### SIL Kit: `ISystemMonitor::SetParticipantConnectedHandler` のデータ競合

**内容**: `SystemMonitor::SetParticipantConnectedHandler()` は、`_participantConnectedHandler` への代入をロックの外で行います。`SetParticipantDisconnectedHandler()` も同じです。一方、I/O スレッドの `OnParticipantConnected()` も、同じハンドラをロックの外で読んで呼び出します（`SilKit/source/services/orchestration/SystemMonitor.cpp`）。participant の作成後にこれらを呼ぶと、ほかの参加者の接続と同時に起きたときに未定義動作になります。過去の接続を再生した直後から代入するまでの間に接続した参加者は、通知されないこともあります。

**検出**: これらの API を使うと、ThreadSanitizer が data race を報告します。

**対応**: vehicle-sil はこの 2 つの API を使いません。他参加者の状態は `AddParticipantStatusHandler` だけで追跡します。SIL Kit の中では、こちらは同期されたハンドラリストで管理されています（ハンドラーの呼び出しと、登録・削除が重なってもよい）。ただし、公開の C++ API の包みは、登録したハンドラーを錠のない表に持つので、登録と削除を 2 つのスレッドから同時に呼べません。`ParticipantRuntime` は、状態を読み直すための登録と削除（`refresh_peer_statuses()`、`wait_for_peer()`）を 1 度に 1 つのスレッドに限ります（テスト `core.peer_statuses_can_be_re_read_from_several_threads_at_once`。ThreadSanitizer で確認します）。lifecycle を開始した参加者が正常に終了せずに切断した場合も、SIL Kit は状態 `Error` を発行するので検知できます。上流のコードは変更していないので、パッチは不要です。

**上流への報告**: 未報告です。

### SIL Kit: `AddParticipantStatusHandler` 登録中のステータスの取りこぼし

**内容**: `SystemMonitor::AddParticipantStatusHandler()` は、次の順で処理します（`SystemMonitor.cpp`）。

1. ロックを取り、接続済みの参加者の現在のステータスを、新しいハンドラに再生する。
2. ロックを外す。
3. ハンドラをリストに追加する。

2 と 3 の間に届いたステータスは、新しいハンドラには届きません。それが相手の最後のステータス（Running など）だと、ハンドラはその状態を観測できないままになります。

**影響**: まれに、相手がすでに Running なのに、それを観測できないまま待ち続けます。

**対応**: `ParticipantRuntime::wait_for_peer()` は、待っている間 200 ms ごとに再同期します（`resync_peers()`）。再同期では、一時的なハンドラを登録してすぐ削除します。登録時の再生はロックの下で正しく行われるので、取りこぼしたステータスを取り戻せます。

`peer_generation()` と `wait_for_peer_change()` は、runtime がすでに知っている状態の変化を監視し、再同期はしません。参加者の状態を確認する前に世代を読み、待機後は期限に達した場合も含めて条件を確認し直してください。

- 再生されたステータスが、すでに届いている新しいステータスを上書きしないように、`enterTime` を比べます。
- 通常の通知は常に反映し、切断時の `Error` はローカルの時計で付けられるため、相手の時計とは比べません。
- `ISystemMonitor::ParticipantStatus()` は、ロックの下で内部データへの参照を返し、呼び出し側がロックの外でそれを読むため、使いません。

**上流への報告**: 未報告です。

### SIL Kit（Windows）: スレッドを止められたプロセスとの local-domain ソケットを閉じると戻らないことがある

**見たこと**: SIL Kit は、同じ機械の participant どうしを、既定で local-domain ソケット（Windows では AF_UNIX）で接続します。Windows で、シミュレーションの実行中に 1 つの participant の全部のスレッドを `SuspendThread` で止め、そのまま controller の participant を破棄すると、まれに、止めた相手とのソケットを閉じる呼び出し（I/O スレッドの上の `closesocket()`）が戻りませんでした。SIL Kit は participant の破棄で I/O スレッドの終わりを待つので（`VAsioConnection::~VAsioConnection()`）、破棄が戻りません。このとき、プロセスに自分を終わらせる呼び出し（`TerminateProcess`）をしても、プロセスは残りました。同じ手順を、local-domain ソケットを使わない構成（`Middleware.EnableDomainSockets: false`、TCP だけ）で繰り返した範囲では、起きませんでした。Linux の同じテストでも起きていません。

**推測（確認していないこと）**: 止められたスレッドが、AF_UNIX のソケットのシステムの呼び出しの途中にいて、こちらの閉じる処理がそれを待つ。そのためにプロセスも終われない。デバッガーの一時停止もスレッドを止めるので、同じことが起きる。TCP の接続は、相手のプロセスの状態によらずに閉じられる。

**影響**: 推測のとおりなら、1 つの participant をデバッガーで止めたまま全体を止めると、まれに、ほかの participant やランチャーが終わらなくなります。

**対応**: vehicle-sil の participant（`ParticipantRuntime`）は、Windows では、構成を渡されなければ `Middleware.EnableDomainSockets: false` で作ります。vehicle-sil が立てる registry（`vehicle_sil_launch run`、`Simulation`）は、どの OS でもそうします（TCP のポートで待ち受けられないとき、SIL Kit の registry は local-domain ソケットだけで動き続け、同じ URI のほかの registry の local-domain のパスを奪うため。[simulation-run.md](simulation-run.md) の「待ち受けるアドレス」）。既定を作るのは 1 か所です（`default_participant_configuration()`、`default_participant_configuration_with()`、`merged_participant_configuration()`、registry は `default_registry_configuration()`。ほかに変える所は [internals/core.md](internals/core.md) の「SIL Kit の設定の既定」）。`vehicle_sil_can_bus_simulator` が自分で作る構成（ログの設定）も、これに追加して作ります。相手が local-domain のエンドポイントを持っていても、こちらが使わないので TCP で接続します。自分の SIL Kit の構成を渡したとき（`participant_configuration`、ecu_main の `--silkit-config`、`vehicle_sil_can_bus_simulator --participant-configuration`）も、Windows では `Middleware.EnableDomainSockets` を `false` にします（`true` と書いても `false` です）。vehicle-sil を使わない SIL Kit の participant どうしの接続には、この対応は適用されません。

**守り方**: `libs/`、`tools/`、`bridges/`、`tests/` で、文字列から SIL Kit の構成を作る所（`ParticipantConfigurationFromString(`）が上の関数を通っていることを、テスト `core.silkit_configuration_defaults`（`cmake/VehicleSilCheckSilKitConfiguration.cmake`）が確認します。

**安全網**: `vehicle_sil_launch run` は、自分の SIL Kit のオブジェクト（変数のエンドポイント、controller、registry）を専用のスレッドで破棄し、期限まで待ちます。戻らなければ、指摘 `connection-not-closed` を追加して結果を書き、静的なオブジェクトの破棄をせずにプロセスを終えます（`OutcomeClose::not_closed_ms`、[simulation-run.md](simulation-run.md) の「止め方」）。上の「見たこと」の場面では、結果は書けましたが、プロセスは残りました。

**上流への報告**: 未報告です。

### SIL Kit: 開始の途中で Error になっても lifecycle が先へ進む

**内容**: coordinated の participant の開始には、後から呼ばれるコールバックで終わる段階が 2 つあります。全 participant からの返事を待つ段階（`ServicesCreatedState::ServicesCreated()`）と、保留中の購読を待つ段階（`CommunicationInitializingState::CommunicationInitializing()`）です（どちらも `SilKit/source/services/orchestration/LifecycleStates.cpp`）。このコールバックは、現在の状態を見ずに次の状態を設定します。待っている間に participant が Error になると、コールバックが Error を CommunicationInitializing や CommunicationInitialized で上書きし、lifecycle はそこから先へ進みます。

required でない coordinated の participant が、この順になります。`LifecycleService::SetWorkflowConfiguration()` は、エラーを後回しの呼び出しで報告します（`ReportError()`）。同じメッセージ、またはソケットから一緒に読んだメッセージで system state が ServicesCreated になると、後回しの呼び出しが動く前に、返事を待つ段階が始まります。

**影響**: その後の動きは、どのメッセージが先に届くかで変わります。1 つは、SIL Kit が不正と報告した participant が Running まで進み、シミュレーションに加わる場合です。もう 1 つは、待っている system state がすでに過ぎていて、Running より前の状態にとどまり続ける場合です。時刻同期があると、ほかの participant がこの participant を待つので、この participant が抜けるまで仮想時間は 0 のままです。

**対応**: パッチ [0006](#0006-開始の待ちが終わったときに-lifecycle-が-error-を離れない) で直しています。2 つのコールバックは、participant が待ちを始めた状態のままのときだけ次へ進めます。Error になった participant は、止められるまで Error のままです。`ParticipantRuntime::start()` は例外を投げ、ecu_main の実行ファイルは、開始に失敗したときの終了コードで終わって、シミュレーションから抜けます。試験 `core.participant_start_fails_for_a_participant_that_is_not_required_and_it_stays_in_error` が、この順を作って確かめます。パッチがなければ落ちます。

開始がまだ返事を待っている間に停止が来る順は、パッチでは直りません。SIL Kit は participant を Shutdown にしますが、終わりの状態を設定するはずの返事の待ちを捨てます。この待ちを 1 つずつしか行わないためです（`ParticipantReplies::CallAfterAllParticipantsReplied()`）。そのため `ParticipantRuntime::stop()` は、このように開始に失敗した後は、`shutdown_timeout` を待たずに、lifecycle が Shutdown になったときに待ちを終えます。試験 `core.participant_stop_after_a_failed_start_returns_once_the_lifecycle_is_in_shutdown` が、返事を遅らせてこの順を作ります。

`ParticipantRuntime` には、パッチのない SIL Kit（利用側の `SilKit::SilKit`）への備えもあります。自分の participant が走り始める前に SIL Kit が出した Error を覚え、そのとき `start()` は、状態がその後どう変わっていても、Error で終わった開始と同じように例外を投げます。ステップのハンドラーも呼びません（[internals/core.md](internals/core.md#start-が戻るときの状態) の「`start()` が戻るときの状態」）。この備えは SIL Kit を Error に戻さないので、パッチがないと次の 2 つが残ります。

- 開始に失敗した participant の停止を、SIL Kit が受け付けないことがあります。その participant は `shutdown_timeout` が過ぎるまで残り、時刻同期があると、その間は全 participant の仮想時間が進みません。
- まれに、SIL Kit が Error を離れた後、Error のステータスが届く前に、`start()` が状態を読みます。`start()` は戻り、その participant は実行の最後まで 1 ステップも実行せず、何も報告しません。

サブモジュールを、この直しを含む上流のコミットに更新したら、パッチを外します。

**上流への報告**: 未報告です。

### SIL Kit: ほかの participant が抜ける間に参加すると、抜けた participant を待つ

**内容**: participant が終わっても、registry がそれを知るのは、I/O スレッドがその接続の終わりを処理したときです。それより前に参加した participant は、終わった participant を既知の participant の一覧で受け取ります（`VAsioRegistry::SendKnownParticipants()`）。参加する側は、次の 3 つの方法を順に試します（`ConnectKnownParticipants.cpp`）。

1. 直接つなぐ。すぐ失敗します。
2. registry が中継する依頼で、相手からつないでもらう。registry は、そのときには宛先を消していて、依頼を返事なしに捨てます（`VAsioConnection::ReceiveRemoteParticipantConnectRequest_Registry()`、「Ignoring invalid RemoteParticipantConnectRequest」）。依頼した側は `Middleware.ConnectTimeoutSeconds` の間待ちます。
3. registry を proxy にする。registry はこのメッセージも届けられず（「Unable to deliver proxy message」）、それを知らせません。依頼した側は、返事をさらに `ConnectTimeoutSeconds` の間待ちます。

相手がもういないことは、参加する側に伝わりません。

**影響**: participant が終わった直後に別の participant が参加すると（すぐ起動し直す ECU、1 つの registry で続けて行う実行）、まれに `CreateParticipant()` が、`ConnectTimeoutSeconds` の 2 倍（既定では 10 秒）の後に `Timeout while waiting for replies from known participants: <名前> (...) is waiting for reply` で失敗します。もう一度試すと成功します。

**対応**: `create_participant()`（したがって `ParticipantRuntime`）は、この時間切れの後に participant を 1 回だけ作り直し、そのことを標準エラーに 1 行出します。この不具合に当たった participant は、失敗せずに、約 10 秒遅れて参加します。2 回目も時間切れなら、やり直したことを書き足した SIL Kit のエラーを投げます。エラーは文で見分けます。上流を更新するときは、テスト `core.silkit_still_writes_the_texts_create_participant_matches` が、SIL Kit のソースにまだこの文があることを確かめます。パッチ 0005 が防ぐのは、registry が終わった participant のソケットをすでに閉じている間だけです。返事は来ないので、`ConnectTimeoutSeconds` を伸ばしても直りません。

**上流への報告**: 未報告です。

### MinGW GCC（`winpthreads`）: `std::recursive_mutex` の破棄で Win32 の `Event` ハンドルがリークする

**内容**: `posix` スレッドモデル（`winpthreads`、MSYS2 UCRT64 など）の MinGW-w64 GCC では、`libstdc++` の `os_defines.h` が `_GTHREAD_USE_MUTEX_INIT_FUNC` と `_GTHREAD_USE_COND_INIT_FUNC` を定義して `std::mutex` と `std::condition_variable` の `winpthreads` 資源を破棄する一方、`_GTHREAD_USE_RECURSIVE_MUTEX_INIT_FUNC` を定義していません。そのため `std::recursive_mutex` は `PTHREAD_RECURSIVE_MUTEX_INITIALIZER` かつ自明なデストラクターになり、`pthread_mutex_destroy()` が呼ばれません。`std::recursive_mutex` をロックすると `winpthreads` が Win32 の `Event` ハンドルを遅延確保し、破棄時にリークします。

**影響**: SIL Kit は内部のサービスやハンドラー一覧で `std::recursive_mutex` を使います。Run ごとに SIL Kit のオブジェクトを作り直す常駐ランチャー（`vehicle_sil_launch serve`）では、回避策がないと 1 Run ごとに約 15〜17 個の Win32 `Event` ハンドルがリークします。

**対応**: `cmake/VehicleSilDependencies.cmake` で、MinGW ビルドのとき SIL Kit の内部インターフェースターゲット（`I_SilKit`）に `_GTHREAD_USE_RECURSIVE_MUTEX_INIT_FUNC=1` を与えます。このマクロを参照するのは `gthr-posix.h` だけなので `winpthreads` 以外のツールチェーンでは影響がなく、SIL Kit の公開 ABI は C API のためバイナリ境界もまたぎません。この回避策で、ハンドルの増加は 1 Run あたり +1 まで減りました。残りの 1 つはミューテックスではありませんでした。C++ API が lifecycle の終わりを待つスレッドを detach しており、`winpthreads` は、detach されたスレッドが終わるたびに `Event` ハンドルを 1 つ残します。パッチ [0007](#0007-c-api-の-startlifecycle-が始めるスレッドを-join-する) がそのスレッドを join するので、`serve` のハンドル数は Run を重ねても変わりません。試験 `serve.windows` は、1 Run あたり 0.5 以上増えると落ちます。

**上流への報告**: 未報告です。

## SIL Kit へのパッチ

`patches/sil-kit/` のパッチは、上流のタグ `v5.0.8`（固定コミット）に `git format-patch` で作ったもので、上流のスタイル（clang-format）に合わせています。0001 は上流の統合テストを、0006 と 0007 は上流の単体テストを含みます。

| パッチ | 直すもの |
|---|---|
| [0001](#0001-lin-の-master-が-dynamic-response-のノードを数えない) | LIN の Master が、dynamic response のノードを数えない |
| [0002](#0002-can-コントローラーの振る舞いの切り替えを-atomic-にする) | CAN コントローラーの振る舞いの切り替えを atomic にする |
| [0003](#0003-registry-がほかの-peer-の名前の登録を消さない) | registry が、ほかの peer の名前の登録を消さない |
| [0004](#0004-非同期の購読が済んだときのハンドラーを失わない) | 非同期の購読が済んだときのハンドラーを失わない |
| [0005](#0005-registry-が接続の閉じた-participant-を通知しない) | registry が、接続の閉じた participant を通知しない |
| [0006](#0006-開始の待ちが終わったときに-lifecycle-が-error-を離れない) | 開始の待ちが終わったときに、lifecycle が Error を離れない |
| [0007](#0007-c-api-の-startlifecycle-が始めるスレッドを-join-する) | C++ API の `StartLifecycle()` が始めるスレッドを join する |

**上流への報告**: いずれも未報告です。0001、0002、0004、0005 は上流の `main` でも同じコードです（0003、0006、0007 は上流の `main` を確認していません）。

### 0001: LIN の Master が dynamic response のノードを数えない

**内容**: trivial simulation の Master が、`SendFrameHeader` で応答するノードを数えるときに、dynamic response のノードを数えない。dynamic response のノードはヘッダーごとに応答するかを決めるので、以前の `SendDynamicResponse` で常に応答するノードにはならない。

**vehicle-sil が必要とする理由**: `LinSlave` は dynamic response を使う。修正がないと、同じ ID に 2 つの `LinSlave` が一度ずつ応答したあと（LIN 診断の 0x3D は全 Slave が共有する）、その ID のヘッダーが送られなくなる。

**上流の再現テスト**: `ITest_LinDynamicResponse.normal_master_dynamic_slaves_take_turns_on_one_id`

**検証**: 上流の LIN のテスト（単体と統合）で確認しています。

### 0002: CAN コントローラーの振る舞いの切り替えを atomic にする

**内容**: CAN コントローラーが trivial と detailed の振る舞いを切り替えるポインタ（`SimBehavior::_currentBehavior`）を atomic にする。切り替えは I/O スレッドのサービス発見で起き、同時にコントローラーのスレッドが `Start()` などでそのポインタを読むことがある。

**vehicle-sil が必要とする理由**: SIL Kit のスレッドの外からコントローラーを呼ぶ所が残っている: `SilKitCanPort::close()` の `Stop()`（止める側のスレッド。シミュレーターが先に抜けて trivial に戻る切り替えと重なりうる）、実時間のポートの送信（ドライバーのスレッド）、Vector XL CAN Bridge の送信（XL の受信スレッド）。`SetBaudRate()` と `Start()` は CommunicationReady のハンドラー（SIL Kit のスレッド）から呼ぶので、当たらない。もとは、生成時に `Start()` を呼んでいたときに、簡易 CAN Network Simulator を別プロセスで動かすテストで ThreadSanitizer が data race を報告した。

**上流の再現テスト**: なし（vehicle-sil の ThreadSanitizer のテストで確認する）。シミュレーターが抜けて再び現れたときの 2 回目の切り替えで、切り替え前の振る舞いを使っている呼び出しと、シミュレーターのリンクの書き換えが重なる場合は直していない。

**検証**: vehicle-sil の簡易 CAN Network Simulator のテスト（`can_netsim.*`）を ThreadSanitizer で実行して確認しています（この競合はまれにしか重ならず、パッチを外してもテストで必ず見つかるわけではありません。テストの結果では外せるかを判断できません。上の「vehicle-sil が必要とする理由」に挙げた、SIL Kit のスレッドの外からコントローラーを呼ぶ所が、コードにまだ残っているかで判断します）。

### 0003: registry が、ほかの peer の名前の登録を消さない

**内容**: registry が、切断した peer の名前の登録（`_participantNameToPeer`）を消すときに、その名前の登録がその peer のものであるときだけ消す。registry は、名前が使用中かを確認する前に、来た peer にその名前を付けるので、同じ名前で拒否した peer が切断すると、先にいた participant の登録が消えていた。

**vehicle-sil が必要とする理由**: 同じ名前で拒否された participant（`--name` の間違い、すぐに起動し直したプロセス。`create_participant()` は拒否される間に作り直す）の後では、先にいた participant に直接接続できない participant（ファイアウォールの内側、local-domain ソケットだけで待つ）が、registry の中継（`RemoteParticipantConnectRequest`）で接続できず、参加の待ちの時間切れになる。

**上流の再現テスト**: なし（vehicle-sil の `core.a_refused_name_leaves_the_registry_able_to_relay_to_the_participant_that_has_it` で確認する）

**検証**: local-domain ソケットだけで待つ participant に、TCP だけの participant が中継で接続するテストで確認しています（パッチがなければ、同じ名前で拒否された participant の後で時間切れになる）。

### 0004: 非同期の購読が済んだときのハンドラーを失わない

**内容**: 非同期の購読（ネットワークシミュレーターの登録など）が済んだときに呼ぶハンドラーの登録（`VAsioConnection::AddAsyncSubscriptionsCompletionHandler()`）と、済んだ側の処理（`AsyncSubscriptionsCompleted()`）を、1 つの mutex で守る。登録は呼び出したスレッドで「済んでいないか」を見てから一覧に追加し、済んだ側は I/O スレッドで一覧を呼んで消してから印を戻すので、最後の確認応答がその間に届くと、追加したハンドラーは呼ばれないままになっていた。

**vehicle-sil が必要とする理由**: SIL Kit は、シミュレートするネットワークの告知（コントローラーを detailed simulation に切り替える Link のサービス）を、このハンドラーから出す（`SimulatedNetworkRouter` のコンストラクター、利用者のスレッド）。失われると、そのネットワークは最後まで告知されない: コントローラーは trivial simulation のまま動き、`SetBaudRate()` とモードはシミュレーターに届かず（ビットレートは既定値のまま）、フレームは直接にも届き、シミュレーターを通しても届く。エラーも警告も出ない。簡易 CAN Network Simulator を起動するたびに、まれに起きうる。

**上流の再現テスト**: なし（重なるのがまれで、テストで必ずは見つからない。vehicle-sil の `j1939_silkit.*_on_the_simulator_*` と `can_netsim.*` が、シミュレーターのビットレートの統計で気づく）

**検証**: 登録の「見てから追加する」の間を仮に遅らせて確認しています（パッチがなければ、シミュレーターを使うテストが、ビットレートが既定値のままで落ちる。パッチがあれば通る）。遅らせなければまれにしか重ならないので、パッチを外してもテストで必ず見つかるわけではありません。

### 0005: registry が、接続の閉じた participant を通知しない

**内容**: registry が、参加してきた participant に通知する一覧（known participants）から、接続がすでに閉じている participant を除く。接続が切れると、ソケットはすぐ閉じるが、registry が一覧から消すのは、その後に I/O スレッドで終わりの処理が実行されてからになる。その間に別の participant の参加を扱うと、閉じたソケットの相手のアドレスを読もうとして例外（`remote_endpoint: Bad file descriptor`）になり、参加してきた側に返事が届かなかった。

**vehicle-sil が必要とする理由**: participant が終わった直後に別の participant が参加する使い方（続けて起動し直す ECU、ランチャーの作り直し、テスト）で、参加する側の `CreateParticipant()` が、`Middleware.ConnectTimeoutSeconds`（既定 5 秒）の後に「Timeout during connection handshake with the SIL Kit Registry」（名前の重複を疑う文が付くが、重複ではない）で失敗する。registry のログには「SilKit-IOWorker: Something went wrong」が出る。もう一度作れば参加できる。

**上流の再現テスト**: なし（重なる時機を外から決められない。vehicle-sil にも、必ず落ちるテストはない）

**検証**: registry が終わりの処理を実行するのを仮に遅らせると、パッチなしでは participant を続けて作るテストが必ずこの時間切れで落ち、パッチありでは通ることで確認しています（仮の遅れは入れたままにしていません）。

### 0006: 開始の待ちが終わったときに lifecycle が Error を離れない

**内容**: 開始の待ちを終える 2 つのコールバック（`ServicesCreatedState::ServicesCreated()` の、全 participant からの返事の待ち。`CommunicationInitializingState::CommunicationInitializing()` の、保留中の購読の待ち）が、participant が待ちを始めた状態のままのときだけ、次の状態を設定する。これまでは、状態が何であっても設定していた。待っている間に Error になった participant や abort された participant が、開始の続きに戻され、そこから先へ進んでいた（「[SIL Kit: 開始の途中で Error になっても lifecycle が先へ進む](#sil-kit-開始の途中で-error-になっても-lifecycle-が先へ進む)」）。

**vehicle-sil が必要とする理由**: 実行にない名前で手で起動した participant は、required でない coordinated の participant として参加する。SIL Kit は、シミュレーションの開始のときに、この participant を Error にする。パッチがないと、Error から先へ進むことがあった。実行に加わるか、Running の前にとどまって全 participant の仮想時間を 0 のままにし、実行が「virtual time has not advanced」で終わる。`ParticipantRuntime` だけで手当てすると、その不具合の節に挙げた 2 つが残る。

**上流の再現テスト**: `Test_LifecycleService.error_while_waiting_for_replies_is_kept`、`Test_LifecycleService.error_while_waiting_for_subscriptions_is_kept`（パッチに含む）

**検証**: 上流の lifecycle service の単体テストと、vehicle-sil の `core.participant_start_fails_for_a_participant_that_is_not_required_and_it_stays_in_error` で確認しています。どちらも、パッチがなければ落ちます。待ちの間の abort は同じ確認で直りますが、専用のテストはありません。

### 0007: C++ API の StartLifecycle が始めるスレッドを join する

**内容**: ヘッダーだけの C++ API の `LifecycleService::StartLifecycle()` は、lifecycle の終わりを待ち（`SilKit_LifecycleService_WaitForLifecycleToComplete()`）、返した future に結果を入れるスレッドを始める。これまではそのスレッドを detach していた。パッチの後は持っておき、デストラクターで join する。`Participant::~Participant()` は、サービスより先に C の participant を破棄し、それで待ちが終わる（lifecycle の future が broken になる）ので、join は止まらない。

**vehicle-sil が必要とする理由**: MinGW-w64 では、`winpthreads` が、detach されたスレッドが終わるたびに Win32 の `Event` ハンドルを 1 つ残す。`vehicle_sil_launch serve` は Run ごとに lifecycle を持つコントローラーの participant を作って破棄するので、Run のたびにハンドルが 1 つ増えていた（「[MinGW GCC（`winpthreads`）: `std::recursive_mutex` の破棄で Win32 の `Event` ハンドルがリークする](#mingw-gccwinpthreads-stdrecursive_mutex-の破棄で-win32-の-event-ハンドルがリークする)」）。また、detach されたスレッドは participant がなくなった後も動いていることがあった。

**上流の再現テスト**: `Test_HourglassOrchestration.SilKit_LifecycleService_Destructor_JoinsTheWaiterOfStartLifecycle`（パッチに含む）

**検証**: 上流のテストと、vehicle-sil の `serve.windows`（Windows、MinGW）で確認しています。`serve` のハンドル数は 30 Run でも 100 Run でも変わりません。パッチがなければ 1 Run ごとに 1 つ増え、試験は落ちます。上流のテストは、パッチがなければどのプラットフォームでも落ちます。

## 上流を更新する手順

1. `git -C external/<lib> fetch --tags` のあと、`git -C external/<lib> checkout <コミット>` を実行し、`git add external/<lib>` でステージする。
2. `git submodule update --init --recursive` を実行し、ネストしたサブモジュールを合わせる。
3. このドキュメントの「実装が前提にしている上流の挙動」を見直す。
4. パッチがある場合は、パッチを適用できるかを確認する（configure が失敗しないこと）。
5. 適用できなければ、新しいコミットの上でパッチを作り直す（`patches/README.md`）。
6. 上流に取り込まれたパッチは削除する。
7. Linux の全プリセットでビルドとテストを実行し、FetchContent テストも実行する。
8. 固定コミットの表を更新してコミットする。

## 関連するドキュメント

- 全体像とターゲットの一覧 → [architecture.md](architecture.md)
- 変更する場所の内部の仕組み → [architecture.md](architecture.md#内部の仕組み) の「内部の仕組み」
- ビルドの手順と必要なもの → [build.md](build.md)
- パッチの作り方 → [patches/README.md](../../patches/README.md)
