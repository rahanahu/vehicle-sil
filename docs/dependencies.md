# 依存ライブラリ

vehicle-sil 自体を直す人（SIL Kit の固定、パッチ、上流の更新）と、gRPC や XL Driver Library を用意する人向けです。直す人は、先に [architecture.md](architecture.md) と、変える場所の [internals/](architecture.md#中の仕組み) の文書を読みます。この後に読む文書はありません。

vehicle-sil は上流ライブラリの SIL Kit を Git サブモジュールとして取り込み、特定のコミットに固定しています。SIL Kit にはパッチ（「SIL Kit へのパッチ」）を当ててビルドします。サブモジュールの作業ツリーは変更しません。J1939 のスタックは vehicle-sil 自身のもの（`vehicle_sil::j1939`）で、外部のライブラリを使いません。

| ライブラリ | パス | 固定コミット | 由来 | ライセンス |
|---|---|---|---|---|
| Vector SIL Kit | `external/sil-kit` | `52ad61d5399483c395585d5feee89d2e89fc74b1` | タグ `v5.0.8` | MIT |

SIL Kit は自身のサブモジュール（`ThirdParty/asio`、`fmt`、`spdlog`、`googletest`）を持つため、`git submodule update --init --recursive` が必要です。FetchContent はこれを自動で行います。

### gRPC と Protocol Buffers（Control Server、任意）

- `VEHICLE_SIL_BUILD_CONTROL_SERVER=ON`（既定 OFF）のときだけ使います。`vehicle_sil::control_grpc`、`proto/` の生成コード、`vehicle_sil_launch`（`run --control`）が対象で、ほかのライブラリ（`vehicle_sil::launch` を含む）と ECU の実行ファイルは使いません。
- `find_package(gRPC CONFIG)` で見つかればそれを使います（MSYS2 UCRT64 は `pacman -S mingw-w64-ucrt-x86_64-grpc`）。見つからなければ FetchContent で gRPC を取り込みます（GitHub の `grpc/grpc`、タグ `v1.76.0`、`VEHICLE_SIL_GRPC_VERSION` で変えられる。サブモジュールは abseil、protobuf、re2、c-ares、BoringSSL、zlib だけ）。取り込んだ gRPC はインストールのルールを作らず（abseil の export 用の設定だけ有効）、警告の対象外（`SYSTEM`）にします。FetchContent で取り込むには CMake 3.28 以上が要ります（それより古い CMake では、gRPC を入れて `find_package` で見つけさせます）。取得済みのソースは `-DFETCHCONTENT_SOURCE_DIR_GRPC=<path>` で再利用できます。
- ライセンス: gRPC は Apache-2.0、Protocol Buffers は BSD-3-Clause、abseil は Apache-2.0、re2 は BSD-3-Clause、c-ares は MIT、BoringSSL は Apache-2.0（一部に古い OpenSSL と ISC の条項が残る）、zlib は zlib ライセンス。vehicle-sil のリポジトリには含めません。
- Python のクライアント（`python/vehicle_sil_control`）は `grpcio` と `protobuf` を、スタブの生成には `grpcio-tools` を使います（`pyproject.toml` の `control` グループ。uv がなければ pip、MSYS2 は `mingw-w64-ucrt-x86_64-python-grpcio` と `-python-grpcio-tools`）。試験はこれらが入った Python 3.11 以上（`VEHICLE_SIL_CONTROL_PYTHON`。指定がなければ、下の「Python の探し方」の順）があるときだけ登録します。

### Python の探し方

- 版と依存は、リポジトリ直下の `pyproject.toml`（`requires-python` と `[dependency-groups]`。`codegen` は PyYAML、`control` は grpcio、grpcio-tools、protobuf、`gui` は GUI サーバー（`gui/server`、workspace のメンバー。変数の読み書き用の grpcio と protobuf つき）と pytest、`dev` はすべてで既定）、`.python-version`（開発で使う版）、`uv.lock`（解決した版の固定）で決めています。版の下限は、試験を実際に回している最も古い版です（`scripts/verify-python.py` が、uv の lowest-direct の解決で下限そのものを Python 3.11 に入れて確かめます）。上限は付けていません。
- vehicle-sil が最上位のプロジェクトのとき、configure はリポジトリの `.venv/bin/python`（`uv sync` が作る）が Python 3.11 以上で必要なモジュールを import できれば、それを使います。そうでなければ `find_package(Python3 3.11)` で探します（有効にした仮想環境が先。`-DPython3_EXECUTABLE=` で指定できる）。見つかった Python にモジュールがなければ、`PATH` の `python3` と `python` を、`PATH` の順に試します（`-DPython3_EXECUTABLE=` で指定したときは試しません）。Windows の `find_package(Python3)` は、レジストリに登録された Python（Windows 用に入れたもの）を `PATH` より先に見つけるので、MSYS2 の Python（`mingw-w64-ucrt-x86_64-python-yaml` などを入れたもの）は、この順で見つかります。処理は `cmake/VehicleSilPython.cmake` にあります。
- 生成器の Python は configure のたびに探し直します。`uv sync` で `.venv` を作ったり、`.venv` にパッケージを足したりした後は、普通に configure し直せば使います（`--fresh` は要りません）。
- FetchContent や `add_subdirectory` で取り込んだ利用側のビルドは、vehicle-sil の `.venv` を使いません。利用側の Python（有効にした仮想環境、`Python3_EXECUTABLE`、`PATH`）を使います。
- Windows では `.venv` を候補にしません（uv の Python を MinGW のビルドから使うことは確かめていないため）。MSYS2 UCRT64 の pacman のパッケージの Python を、今までどおり探して使います。

### Python 3 と PyYAML（J1939 の型付きメッセージの生成）

- `vehicle_sil_add_j1939_codec()` が、ビルドのときに `tools/j1939_codegen/j1939_codegen.py` を動かします。Python 3.11 以上と PyYAML（`import yaml`）が要ります。configure のときに上の順で探し、PyYAML を import できるかを確かめます。なければ、この関数を呼んだ時点で configure が失敗し、試した Python とそれぞれを使わなかった理由（`No module named 'yaml'`、3.11 より古い版など）と、入れ方（uv、pip、OS と MSYS2 のパッケージ）を示します。Control API の試験を見送るときの configure の表示も、同じように試した Python と理由を示します。
- vehicle-sil の試験と例（`VEHICLE_SIL_BUILD_TESTS`、`VEHICLE_SIL_BUILD_EXAMPLES`。トップレベルのビルドでは既定で ON）はこの関数を使うので、そのビルドには Python と PyYAML が要ります。ライブラリだけを使う利用側（FetchContent など）は、関数を呼ばなければ要りません。
- PyYAML は YAML 1.1 で読みます。生成器は `on`、`off`、`yes`、`no` を真偽値にせず文字列として読み、整数は 10 進（先頭の 0 なし）、16 進、2 進だけを受け付け（8 進と、整数と小数の 60 進を誤りにする）、同じキーの重複を誤りにします（マージキーで取り込んだキーの上書きは可）。
- Python や PyYAML が見つからなかった configure の結果はキャッシュに残りません。入れたあとで configure し直せば見つけます。

### aiohttp と PyYAML（ブラウザの GUI のサーバー）

- GUI サーバー（`gui/server`、配布名 `vehicle-sil-gui`、[gui.md](gui.md)）は aiohttp と PyYAML（ダッシュボードのファイルの読み込み）に依存します。HTTP のサーバー、WebSocket、試験で使うクライアントを 1 つで持ち、asyncio の Windows の既定のイベントループ（子プロセスを動かせる ProactorEventLoop）で動き、Windows（3.11 以降）の wheel があります。C++ のビルドは使いません。ECU の変数を読み書きするときだけ、ランチャーの Control Server のクライアントとして grpcio と protobuf を使います（任意の依存 `vehicle-sil-gui[variables]`。`gui` のグループが入れます）。なくても GUI サーバーは動き、変数だけが使えないと理由を出します。この任意の依存だけでは足りず、スタブが要ります（リポジトリの中で `uv sync` して動かすか、スタブを同梱したビルド済みの道具を使うのが前提。スタブは Python のパッケージには入っていません）。gRPC のスタブは、起動のたびに `proto/` から grpcio-tools（`control` のグループ）で作るか、`python -m vehicle_sil_control.generate` が作ったもの、`VEHICLE_SIL_CONTROL_STUBS` のディレクトリのものを使います。
- ライセンス: aiohttp は Apache-2.0（同梱の llhttp は MIT）。依存する multidict、yarl、frozenlist、aiosignal、propcache は Apache-2.0、aiohappyeyeballs は PSF-2.0、attrs は MIT、idna は BSD-3-Clause。vehicle-sil のリポジトリには含めず、uv が入れます。

### Vector XL Driver Library（Vector XL CAN Bridge、Windows のみ）

- XL Driver Library は vehicle-sil に含めず、サブモジュールにもしていません。利用者が Vector から入手し、`VEHICLE_SIL_VECTOR_XL_DIR` で場所を指定します。
  - 使うファイルは `vxlapi.h`、`vxlapi64.lib`（32 bit なら `vxlapi.lib`）、実行時の `vxlapi64.dll` です。
- 見つからなければ、ブリッジ（`vehicle_sil::vector_xl` と `vehicle_sil_vector_xl_bridge`）はビルドされません。
- 使う API はクラシック CAN 用です（`XL_INTERFACE_VERSION`、`XL_BUS_TYPE_CAN`）。

| 用途 | API |
|---|---|
| ドライバ | `xlOpenDriver`、`xlCloseDriver`、`xlGetDriverConfig`、`xlGetErrorString` |
| ポート | `xlOpenPort`、`xlClosePort`、`xlActivateChannel`（`XL_ACTIVATE_NONE`）、`xlDeactivateChannel` |
| 設定 | `xlCanSetChannelBitrate`（init 権限が要る、15000〜1000000 bit/s）、`xlCanSetChannelMode(tx=1, txrq=0)`（送信レシートを有効にし、送信要求の通知を無効にする。init 権限が要る。権限があるときだけ呼ぶ） |
| 送受信 | `xlCanTransmit`（`XL_TRANSMIT_MSG`、DLC 0〜8）、`xlReceive`（`XL_RECEIVE_MSG`、1 回に 1 件）、`xlSetNotification` と `WaitForSingleObject` |

- 上の API を呼ぶのは `bridges/vector-xl/src/xl_backend_vector.cpp` だけです。このファイルは、vehicle-sil 独自の内部インターフェース（`src/xl_backend.hpp`。名前、型、値は vehicle-sil のもの）を XL Driver Library の呼び出しで実装します。行うのは型と値の変換だけで、判断も状態も持ちません。Windows で XL Driver Library が見つかったときだけビルドされます。
- ブリッジの判断と状態（イベントの分類、オーバーランの扱い、受信スレッド、開閉の順序と後始末、エラーの組み立て）は `src/xl_driver.cpp` にあります。内部インターフェースだけに依存するので、どのプラットフォームでもビルドされます。
- テストは、内部インターフェースを実装したテスト専用の偽物（`tests/vector_xl/fake_xl/`）と `src/xl_driver.cpp` をリンクして実行します。Vector のヘッダーもライブラリも使いません。リポジトリには、Vector のヘッダーもその写しも含めていません。

## 取り込み方（`cmake/VehicleSilDependencies.cmake`）

- `add_subdirectory(... EXCLUDE_FROM_ALL)` で追加します。リンクされたターゲットだけがビルドされます。
- SIL Kit は、サブモジュール `external/sil-kit` をビルドツリーの `external/sil-kit-patched/` にコピーし（ネストしたサブモジュールも含めて `.git` を除く）、`patches/sil-kit/*.patch` をファイル名の順に `git apply` してから追加します。パッチがなければ、コピーせずにサブモジュールを直接追加します。
  - コピーとパッチ適用は、パッチかサブモジュールのファイル（`.git` を除く）の内容が前回の configure と違うときだけやり直します。判定には、全ファイルの SHA-256 を並べたスタンプ（`sil-kit-patched.stamp`）を使います。Git の状態は見ないので、サブモジュールが Git のチェックアウトでなくても動きます。やり直すときはコピーしたファイルをすべて touch するので、そのライブラリは全体が再ビルドされます。
  - SIL Kit のバージョン情報の Git ハッシュ（`SilKit::Version::GitHash()`）は `UNKNOWN` になります。SIL Kit は自分の `.git/HEAD` を探しますが、サブモジュールの `.git` はファイルなので、コピーしなくても同じです。
  - パッチの追加、変更、削除と、サブモジュールのファイルの変更（チェックアウトの切り替えや手元での編集）では、CMake が自動で configure をやり直します。
  - パッチが当たらなければ configure は失敗します。`git apply` を使うので Git が必要です。親の Git リポジトリや、継承した `GIT_DIR` などの環境変数の影響は受けません。ビルドツリーがリポジトリの中にあっても、`GIT_CEILING_DIRECTORIES` で、コピーの上のリポジトリを Git に見せません。この値は、Git 自身が出すパスの形で渡します（MSYS2 の `git` は `/d/a/...` の形しか読まないため）。
  - 失敗の文には、使った Git のパスと版、`GIT_CEILING_DIRECTORIES` の値、Git がコピーの周りに見つけたリポジトリ（あれば、パッチのパスをそのリポジトリからの相対と取り違えて飛ばしたということです）、パッチが変えるファイルの改行（LF か CRLF か）が出ます。
  - `--ignore-whitespace` で当てるので、CRLF でチェックアウトしたサブモジュール（Windows の `core.autocrlf`）にも当たります。パッチファイル自体は `.gitattributes` で改行を変換しません。
- 利用側がすでに `SilKit::SilKit` を定義している場合は追加しません。確かめているのは v5.0.8 だけです。それより古い SIL Kit には、vehicle-sil が頼っている挙動の一部がありません（「[混ぜてよい SIL Kit の版](#混ぜてよい-sil-kit-の版)」）。利用側の SIL Kit に「SIL Kit へのパッチ」がないと、同じ LIN ID に複数の `LinSlave` が交代で応答する構成（診断の 0x3D など）が動きません。パッチが効くのはヘッダーを送る Master のプロセスの SIL Kit なので、パッチのない SIL Kit を使う外部の Master（CANoe など）と組み合わせた場合も同じです。
- 上流の公開インクルードディレクトリを SYSTEM 扱いにします。上流ヘッダーの警告で、vehicle-sil のターゲットが `-Werror` 付きのビルドに失敗しないようにするためです。
- 上流のオプションは関数スコープの通常変数で上書きします（CMP0077）。利用側のキャッシュは変えません。

| ライブラリ | 設定 | 理由 |
|---|---|---|
| SIL Kit | `SILKIT_BUILD_DEMOS/TESTS/DOCS/DASHBOARD=OFF`、`SILKIT_BUILD_STATIC` は `VEHICLE_SIL_STATIC` と同じ（既定 OFF）、`SILKIT_BUILD_UTILITIES=ON` | 不要なものを作らない。統合テストに `sil-kit-registry` が要る。`VEHICLE_SIL_STATIC=ON` では、SIL Kit を静的なライブラリにし、`SilKit::SilKit` の `INTERFACE_LINK_OPTIONS` に C++ の実行時ライブラリの静的なリンク（Linux: `-static-libstdc++ -static-libgcc`、MinGW: `-static`）を付ける。SIL Kit をリンクする実行ファイル（利用側の ECU も）に効く。glibc は共有のまま（[build.md](build.md) の「1 つの実行ファイルにする（`VEHICLE_SIL_STATIC`）」） |

## 使っているターゲット

| ターゲット | 中身 |
|---|---|
| `SilKit::SilKit` | SIL Kit 本体。既定では共有ライブラリ（`VEHICLE_SIL_STATIC=ON` では静的）で、Debug では `libSilKitd.so` / `SilKitd.dll` になる。出力先は `${CMAKE_BINARY_DIR}/$<CONFIG>` |
| `sil-kit-registry` | レジストリの実行ファイル。SIL Kit を静的にリンクしているので、共有ライブラリは不要 |
| `rapidyaml` | SIL Kit が `ThirdParty/rapidyaml` からビルドする YAML/JSON パーサー（MIT。静的ライブラリ）。`vehicle_sil::can_netsim` と `vehicle_sil::launch` が設定ファイルの読み込みに使い、`vehicle_sil::core` が、渡された SIL Kit の構成に vehicle-sil の既定を足すのに使う（`merged_participant_configuration()`）。SIL Kit をサブモジュールから追加しないとき（利用側が `SilKit::SilKit` を定義済み）はこのターゲットがないので、`vehicle_sil::can_netsim`、`vehicle_sil_can_bus_simulator`、`vehicle_sil::launch`、`vehicle_sil_launch` を作らない。`vehicle_sil::core` は作るが、渡された SIL Kit の構成に既定を足さない（[internals/core.md](internals/core.md) の「SIL Kit の構成の既定」） |

Windows では `SilKit.dll` が実行ファイルと別のディレクトリに出力されます。`vehicle_sil_copy_runtime_dlls(<target>)` を呼ぶと、ビルド後に DLL が実行ファイルの隣へコピーされます。

### Linux のカーネルの口（イーサネットのブリッジの `tap` と `packet`、Linux のみ）

- 外のライブラリは使いません。使うのはカーネルと libc のヘッダーだけで、ビルドにも実行にも、入れるものはありません。
- 呼ぶのは `libs/eth_link/src/linux` だけです（`vehicle_sil::eth_link` の中。Linux でだけビルドします）。

| 用途 | 口 |
|---|---|
| `tap` | `/dev/net/tun` を開き、`ioctl(TUNSETIFF)`（`IFF_TAP`、`IFF_NO_PI`。`--tap-create` では `IFF_TUN_EXCL` も）。`read` と `write` が 1 回に 1 フレーム |
| `packet` | `socket(AF_PACKET, SOCK_RAW)` をインターフェースに `bind`（`ETH_P_ALL`）。`recvmsg` と `send`。`PACKET_AUXDATA`（外された VLAN タグ）、`PACKET_ADD_MEMBERSHIP` の `PACKET_MR_PROMISC`、`PACKET_STATISTICS`（捨てた数） |
| インターフェース | `if_nameindex`、`if_nametoindex`、`ioctl(SIOCGIFFLAGS)`、`ioctl(SIOCGIFHWADDR)`（リンクの種類） |
| 受信スレッド | `poll`、`eventfd`（止める合図） |

- 実装が前提にしているカーネルの挙動（試験 `ethernet_bridge.netns.*` が確かめます）: TAP デバイスに書いたフレームは、書いた側の読み取りに戻らない。packet ソケットは、自分が送ったフレームを受けず、同じインターフェースのほかのソケットが送ったフレームは受ける。受けたフレームの外側の VLAN タグは、フレームのバイト列から外され、`PACKET_AUXDATA` で渡される。
- 試験していない前提: グループのアドレス宛てに送ったフレームを、カーネルが同じ PC の受け手のために渡し直した写し（`PACKET_LOOPBACK`）は、送ったときの写しと重なるので、`packet` は捨てます。

SIL Kit 本体は C++17 でビルドされます。C++ API はヘッダーのみで C API を包む構成なので、利用側を C++20 でコンパイルしても問題ありません。MinGW-w64 は上流が対応しています（`-Wa,-mbig-obj` など）。

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
| SIL Kit | CAN | `CreateCanController`、`ICanController::Start/Stop/SendFrame/SetBaudRate`（`SetBaudRate` と `Start` は CommunicationReady のハンドラーから）、`AddFrameHandler`、`AddFrameTransmitHandler`、`AddErrorStateChangeHandler`（ネットワークシミュレーターがいるときだけ知らせが来る）。`SendFrame` の userContext に送信時刻を入れる（簡易 CAN Network Simulator との取り決め） |
| SIL Kit | Ethernet（`vehicle_sil::eth_silkit`） | `CreateEthernetController`、`IEthernetController::Activate/Deactivate/SendFrame`（`Activate` は CommunicationReady のハンドラーから）、`AddFrameHandler`（RX だけ）、`AddFrameTransmitHandler`、`AddStateChangeHandler` |
| SIL Kit | Ethernet（`vehicle_sil::ethernet_bridge_core`） | `CreateEthernetController`、`IEthernetController::Activate/Deactivate/SendFrame`（`Activate` は CommunicationReady のハンドラーから）、`AddFrameHandler`（RX の向きだけ）、`AddFrameTransmitHandler`（`Transmitted` 以外の確認だけ） |
| SIL Kit | Network Simulator（experimental、`vehicle_sil::can_netsim`） | `Experimental::Participant::CreateNetworkSimulator`、`INetworkSimulator::SimulateNetwork/Start`、`ISimulatedNetwork`、`Can::ISimulatedCanController`、`Can::ICanEventProducer::Produce`（フレーム、送信確認、状態の変化） |
| SIL Kit | プロセス内レジストリ（`Simulation`、`vehicle_sil_launch run`） | `Vendor::Vector::CreateSilKitRegistry`、`StartListening` |

## 実装が前提にしている上流の挙動

上流を更新するときは、次の挙動が変わっていないかを確認してください。

**SIL Kit**
- `RemoveFrameHandler` は、実行中のハンドラの終了を待ってから戻ります。ハンドラの呼び出しと削除が同じ `recursive_mutex` で直列化されているためです。`SilKitCanPort::close()` はこれを前提に、ハンドラを削除してから受け箱を捨てます。
- `ParticipantStatus` は履歴長 1 で配信されます。後から参加した participant も、既存の参加者の最新状態を受け取れます。`ParticipantRuntime::wait_for_peer` はこれを使います。
- 購読が確立する前に送ったフレームは届かないことがあります。そのため、コントローラは `ParticipantRuntime::start()` の前に作ります。
- コントローラーの最初の設定（CAN の `SetBaudRate` と `Start`、LIN の `Init`、Ethernet の `Activate`）は、CommunicationReady のハンドラーで呼ぶよう、SIL Kit の文書が勧めています。vehicle-sil のポートとブリッジは、そこで呼びます（`ParticipantRuntime::add_communication_ready_handler()`）。このハンドラーは、coordinated でも autonomous でも、participant の通信スレッドで、Running（最初のステップ）より前に呼ばれます（autonomous の開始も、`StartLifecycle()` が通信スレッドに積みます）。coordinated の participant では、必須の participant が全員サービスを作り終えた後なので、ネットワークシミュレーターが模擬するネットワークも見つかっています。autonomous の participant には、その保証がありません。試験 `core.communication_ready_handlers_that_were_added_run_in_order_before_the_one_that_was_set_and_the_first_step`（スレッドと順）と `can_netsim.simulator_learns_the_bit_rate_and_the_start_whichever_joins_first`（起動の順によらず届くこと）が確かめます。
- trivial simulation の CAN は、`Start()` の前の `SendFrame` を、警告のログだけで捨てます。送信の確認は来ません（`SimBehaviorTrivial::SendMsg`）。`Start()` はその場で状態を変えます。受信は `Start()` と関係なく届きます。そのため `SilKitCanPort` は、`start()` の前の送信を SIL Kit に渡さずに誤りにし、ブリッジは、コントローラーを始める前に外から届いたフレームを SIL Kit に渡しません。trivial simulation の Ethernet は、`Activate()` の前の `SendFrame` に `ControllerInactive` の確認を返します。
- `sil-kit-registry --listen-uri silkit://127.0.0.1:0 --generate-configuration <file>` は空きポートで待ち受け、実際の URI を含む設定ファイルを原子的に書き出します。統合テストはこれを使って、並列実行でもポートが衝突しないようにしています。
- 時刻同期した participant は、自分の次のステップ時刻がほかの全員の次のステップ時刻以下のときだけ進みます（`TimeConfiguration::OtherParticipantHasLowerTimepoint`）。「最も遅いピアを待つ」と `SimulationController` の stop_at の保証は、これに基づいています。
- 構成の `Experimental.TimeSynchronization.DynamicSimulationStep` は 3 つの値を取ります。`true` の participant は、自分のステップを全員の最小の幅に縮め、ほかの participant にもそうするよう求めます。キーがない participant は、`true` の participant が 1 つでもいれば、それに従って縮みます。`false` の participant は縮みません。vehicle-sil のステップは固定の幅を前提にしているので（ステップの格子、`TickScheduler`、時間ゲート）、既定の構成（`default_participant_configuration()`）はいつも `false` にします。自分の構成を渡した participant にも、キーがなければ `false` を足し、`true` なら participant を作る前に誤りにします（`merged_participant_configuration()`）。それでも SIL Kit がステップを縮めたとき（rapidyaml のないビルドで、キーのない構成を渡したとき）は、`ParticipantRuntime` が、渡されたステップの幅が自分の幅と違うことを見つけて、そのステップを失敗にします（[internals/core.md](internals/core.md) の「SIL Kit の構成の既定」）。experimental のキーなので、上流を更新するときは、名前と意味が変わっていないかを確かめます。試験 `core.virtual_time_keeps_its_step_when_another_participant_asks_for_dynamic_steps` が、`true` の participant と一緒に動かして、既定の構成が効いていることを確かめます（キーがなくなるか、意味が変わると落ちます）。`core.virtual_time_keeps_its_step_with_a_configuration_of_its_own` が、キーのない自分の構成でも同じであることを確かめます。
- 同じ名前の participant を断るとき、registry は「A participant with the same name ... already exists in the simulation」という文の誤りを返します。`create_participant()` は、誤りの文にこの「already exists in the simulation」があるときだけ、待ってやり直します（SIL Kit には、誤りの種類を見分ける公開の口がありません）。断り方と文は registry の版によって違うので、ほかの版の registry（ほかの道具が立てたもの、公式の配布物のもの）では文が合わないことがあります。そのときは、待たずに、SIL Kit の誤りのまますぐ失敗します（`name_in_use_wait` が効きません。同じ名前ですぐに起動し直すときは、少し間を空けます）。断られた後の作り直しは、registry が同じ名前を毎回断ること（4.0.55 以上）にも頼っています。それより古い registry は 2 回目の接続を断らないので、同じ名前の participant が黙って 2 つ入ります（変更履歴から。古い版では試していません）。上流を更新するときは、この文が変わっていないかを確かめます（試験 `core.participant_runtime_waits_while_the_registry_still_has_the_name`、`core.participant_runtime_says_how_long_the_name_was_refused`）。
- Agent と Hub の間を publish/subscribe で運ぶ層（`vehicle_sil::control_silkit`）と始まる前の窓は、次の挙動を前提にしています。試験 `control_silkit.*` と `launch_control.*` が、まとめて確かめます（どれかが変わると、`HubTransportStats::late_messages` が 0 でなくなるか、窓の中の書き込みが時刻 0 のステップに間に合わなくなります）。
  - `IDataPublisher::Publish` は、呼んだスレッドでデータを写し、送信を participant の I/O スレッドの待ち行列に積むだけです。どのスレッドから呼んでも安全で、1 つの相手には積んだ順に届きます。
  - ステップの完了（`CompleteSimulationStep`）も同じ待ち行列に積まれます。そのため、ステップの中で送ったメッセージは、そのステップの次の時刻の知らせより前に相手に届きます。上の「進む条件」と合わせて、受け手のステップ P が始まる時点で、P より前に始まった送り手のステップのメッセージは全部届いています（ちょうど P のステップの分は決まりません）。ステップの外（別のスレッド、受信のハンドラー）から送ったメッセージには、この関係がありません。
  - 受信のハンドラーは、participant の I/O スレッドで、届いた順に 1 つずつ呼ばれます。ステップのハンドラーも同じスレッドです。時間ゲートがステップを保留している間も、I/O スレッドは空いていて、メッセージは届きます。
  - 履歴 1 の発行者が最後に送ったメッセージは、後から購読した相手にも届きます（発行者の participant がつながっている間）。participant を始める前に送った hello も届きます。
  - Coordinated の participant の CommunicationReady のハンドラーは、必須の participant が全員 ServicesCreated になり、全員から返事が届き、自分の publish/subscribe の購読が確認された後で呼ばれます（ほかの participant は、その時点でまだ CommunicationInitializing の途中のことがあります。コマンドは受け取れます）。相手は、購読の知らせを受けると、履歴（hello）を送ってから確認を返すので、必須の ECU が始める前に送った hello は、窓が開く前に届いています。非同期のハンドラーでは、`CompleteCommunicationReadyHandlerAsync` を呼ぶまで、誰も ReadyToRun に進まず、どのステップも始まりません。その間、publish/subscribe は届きます。`CompleteCommunicationReadyHandlerAsync` は、ハンドラーが呼ばれる前に呼ぶと無視されます（`ParticipantRuntime` が、呼ばれた後にだけ渡します）。
  - 始めたライフサイクルを持つ participant が、Shutdown せずに接続を切ると、ほかの participant にはその状態が Error として届きます（`HubTransport` が、いなくなった Agent の接続を終えるのに使います）。
  - participant の構成の `EnableMessageAggregation` は既定の Off のままであること（On にした participant は、データのメッセージを溜めてから送るので、上の順序の前提が変わります）。
  - participant を壊すと、まだソケットに書いていない送信は捨てられます（同じ相手への前の送信を書いている途中なら、その後ろに積んだ送信は書かれません）。そのため、Agent の「最後の知らせ」は、Hub の返事を待ってから participant を壊します（`AgentTransport` のデストラクター。internals/control.md の「接続の終わり」）。participant が生きている間は、積んだ送信は順に書かれます。
- スレッドと順序について、公式の文書が約束していることと、実装を読んで頼っていることを分けて書きます。
  - 約束されていること: 受信のハンドラーとステップのハンドラーは、participant の 1 つの通信スレッドで呼ばれ、ステップのハンドラーが動いている間はほかのハンドラーが呼ばれません。ブロッキング版のステップのハンドラーの中で送ったメッセージは、ハンドラーが終わったときに配られます。メッセージは、どのスレッドやハンドラーから呼んでも、API を呼んだ順に送られます（4.0.24 の変更履歴。別々のスレッドから並行に呼んだものどうしの順は約束されていません）。
  - 約束されていないこと（実装を読んで頼っている）: 非同期版のステップ（`CompleteSimulationStep`）でも、ステップの中の送信が完了の知らせより前に出ること（完了の知らせが、送信と同じ待ち行列を通ること）。`Publish` と `SendFrame` をどのスレッドから呼んでもよいこと（順の約束は別のスレッドからの呼び出しに触れていて、公式の例にも別のスレッドからの送信がありますが、スレッド安全性そのものの記述はありません）。上の publish/subscribe の項目と、下の LIN の Master のフレームの項目は、これに頼っています。
  - 「ステップ T が始まる時点で、T より前の時刻のメッセージは全部届いている」のは、送り手がステップのハンドラーの中で送ったときだけの約束です。ほかの SIL Kit のプログラムがステップの外（別のスレッド、受信のハンドラー）から送ったフレームは、受け手のどのステップに入るかが実行ごとに変わりえます（[internals/can.md](internals/can.md)、[internals/ethernet.md](internals/ethernet.md)）。
  - コントローラーの設定の操作（CAN の `SetBaudRate`、`Start`、Ethernet の `Activate` など）は、どのスレッドから呼んでも安全とは言えません（trivial と detailed の切り替えは通信スレッドで起きます。「SIL Kit へのパッチ」の 0002）。vehicle-sil は、通信スレッドの上（CommunicationReady のハンドラー）で呼びます。
- 時間ゲートは、非同期版のステップを完了させないことで仮想時間を止めます（[internals/virtual-time.md](internals/virtual-time.md)）。SIL Kit の HealthCheck（構成の `HealthCheck`）は、ステップのハンドラーの呼び出しにかかった時間だけを測るので、保留しているステップを時間切れにしません。公式の文書が約束しているのは `Pause()` の間のことだけで、完了させていない非同期のステップのことは書いていません。変わると、HealthCheck を付けた構成では、一時停止が警告か Error になります。
- 時刻同期した participant が送るメッセージのタイムスタンプは、送信時のステップの時刻です。
- 時刻同期していない participant が送るメッセージのタイムスタンプは `std::chrono::nanoseconds::min()` です（`TimeProvider` の既定）。時刻同期している受け手の SIL Kit は、届いたメッセージのこの値を自分の今の時刻に置き換えます（`SilKitLink::DistributeRemoteSilKitMessage`）。最初のステップの前は今の時刻もこの値なので、そのまま残ります。`SilKitCanPort`（仮想時間）と `SilKitEthPort` は負の時刻を見分け、受け手の最新のステップの時刻（最初のステップの前なら 0）に寄せます（[internals/can.md](internals/can.md) の「SIL Kit のポート」、[internals/ethernet.md](internals/ethernet.md) の「時刻同期に参加していない送り手」）。試験 `j1939_silkit.j1939_on_virtual_time_receives_a_bam_from_a_participant_without_time_synchronisation*` が、始まる前と始まった後の両方を確かめます。
- Ethernet は、Network Simulator がなければ、送ったフレームを同じネットワークのほかの全部のコントローラーに届けます（宛先のアドレスで絞らない）。送信の確認は `SendFrame()` の中で、送った時刻で返ります。`Activate()` は、その中で状態を LinkUp にします。60 バイトより短いフレームは、受け手のコントローラーが 0 で埋めてからハンドラーに渡します（`EthController::ReceiveMsgInternal`）。`VirtualEthernetBus` はこの振る舞いに合わせてあり、試験 `eth_silkit.records_match_*` が同じ記録になることを確かめます。
- Ethernet（`EthernetBridge` が前提にしています。試験 `ethernet_bridge.silkit_*` が、埋めることと、自分の送信が RX のハンドラーに来ないことを確かめます）:
  - `SendFrame` は、呼び出しの中でフレームのバイト列を写します。ブリッジは、外の口のハンドラーの間だけ有効なバッファーをそのまま渡します。
  - trivial simulation では、送ったフレームはほかの participant に RX として、自分には TX として届きます。RX の向きだけで登録したハンドラーには、自分の送信は来ません。
  - trivial simulation では、`Activate()` でコントローラーはすぐ `LinkUp` になり、送信の確認は `SendFrame` の中で同期に呼ばれます。状態は、`LinkUp` なら `Transmitted`、`LinkDown` なら `LinkDown`、`Activate()` の前なら `ControllerInactive` です。
  - 受け手は、60 バイトより短いフレームを 0 で埋めてからハンドラーに渡します（`EthController::ReceiveMsgInternal`）。送る側には最小長の検査がありません（`SilKitEthPort` は、送る前に自分で 60 バイトまで埋めます。ブリッジは、外から受けたフレームをそのまま渡します）。最大長の検査は、送る側にも受ける側にも見当たりません。
  - `RemoveFrameHandler` は、CAN と同じく、実行中のハンドラーの終了を待ってから戻ります（ハンドラーの呼び出しと削除が同じ `recursive_mutex` で直列化されている）。
  - コントローラーは、フレームのハンドラーを、ハンドラーの集まりの `recursive_mutex` を持ったまま呼びます（`SynchronizedHandlers::InvokeAll`）。trivial simulation の `SendFrame` は、自分への TX の配達で同じ集まりの `InvokeAll` を通るので、同じ錠を取ります。そのため、外の口の受信スレッドが呼ぶ `SendFrame` は、SIL Kit の受信スレッドが `EthernetBridge` のハンドラー（その中の `ExternalLink::transmit()`）を終えるまで待ちます。`ExternalLink` の契約（`transmit()` はブロックせず、ハンドラーの間に `transmit()` の取る錠を持たない）は、これを前提にしています。
- CAN の `SendFrame` の userContext は、ポインタの値（64 ビット）のまま運ばれます。ネットワークシミュレーターの `CanFrameRequest` と送信確認には届き、他の受信者の RX のフレームでは `nullptr` になります（`CanController::ReceiveMsg`）。`SilKitCanPort` は送信時刻を入れ、簡易 CAN Network Simulator がそれを読みます。公式の文書に書いてあるのは、userContext が送り手の送信確認とシミュレーターの要求に届くことまでで、プロセスをまたいで値を運ぶ使い方は約束されていません。SIL Kit の replay が流し直すフレームは userContext が空なので、送信時刻がありません（簡易 CAN Network Simulator は、時刻のない要求を、受け取った時点の自分の最後のステップの時刻に置き、警告を 1 度出します。この位置は決定的ではありません）。
- Network Simulator API（experimental）: あるネットワークの模擬を引き受ける participant が現れると、そのネットワークの CAN コントローラーは詳細シミュレーションに切り替わり、送信の要求、ボーレート、モードの変更をその participant に送ります。コントローラーの状態（`Started` など）は、シミュレーターが返す状態の変化で初めて変わります。`ControllerDescriptor` は見つけた順の通し番号で、決定的ではありません。
- コントローラーがシミュレーターを見つける前に呼んだ `SetBaudRate()` と `Start()` は、trivial simulation で処理され、見つけた後もシミュレーターに送り直されません。すでに見つけているシミュレーターは、コントローラーを作った時点で同じスレッドから知らされます。vehicle-sil のポートは、どちらも CommunicationReady のハンドラーで呼ぶので、coordinated の participant の設定は、起動の順によらずシミュレーターに届きます。autonomous の participant（途中から入るもの、ブリッジ）と、コントローラーを作ってすぐ呼ぶほかのプログラムの設定は、届くとは限りません。そのため、簡易 CAN Network Simulator は、ビットレートが届かなければ `--default-bitrate` を使い、フレームの配送をモードで止めません。シミュレーションが始まった後の送信は、すべてシミュレーターを通ります。Ethernet も同じ作りです（見つける前の `Activate()` は手元の状態だけを LinkUp にし、2 回目の `Activate()` は何もしません）。
- `ReportError` で Error になった participant は、`Stop` か abort されるまで Error のままで、`StartLifecycle` の future も完了しません。`Stop` は Error から Shutdown に進めます。公開のヘッダーと文書は、`Stop` を呼べるのは Running（か Paused）のときとしていて、Error からの `Stop` は約束の外です（Error については、終わらせるしかない、とだけ書いてあります）。`ParticipantRuntime::stop()` は Error でも `Stop` を呼ぶので、上流がこの前提を検査するようになると、Error の participant が Shutdown に進まなくなります。上流を更新するときに確かめます（試験 `core.virtual_time_stop_during_a_failing_step_leaves_through_error`）。
- workflow configuration は、送った時点で接続している参加者にだけ届きます。そのため `SimulationController` は、必要な参加者のステータスが見えてから送ります（`sil-kit-system-controller` と同じ方針）。
- LIN の trivial simulation では、Slave の応答はヘッダーを受信したハンドラの中で生成されます（`SimBehaviorTrivial::ProcessFrameHeaderRequest`、dynamic response では `LinFrameHeaderEvent`）。ヘッダーのタイムスタンプは Master の送信時刻です。応答のタイムスタンプは Slave の `Now()` なので、決定論的ではありません（`LinMaster` は使いません）。
- 応答する Slave が設定されておらず、dynamic response の Slave もいないヘッダーに対しては、Master 自身の `SendFrameHeader` の中から同期的に `LIN_RX_NO_RESPONSE` が通知されます。dynamic response の Slave（`LinSlave`）がいると、ヘッダーは常に送られ、応答がなければ状態は通知されません。`LinMaster` はその場合をスロットの終わりに NoResponse とします。
- SIL Kit は、ある ID に最初に応答した Slave をその ID の送り手として記録し、取り消す方法がありません。別の Slave が同じ ID に応答すると、Master はヘッダーを送らずに Error を報告し続けます。
- LIN の `GoToSleep()`（Master のみ）は、trivial simulation ではヘッダーなしで go-to-sleep フレーム（ID 0x3C）を送り、自分をすぐ Sleep にします。`Wakeup()` は、ほかの参加者に方向 RX のパルスを送り、自分のハンドラを方向 TX で呼び、自分を Operational にします。受信側の状態は変わらないので、`GoToSleepInternal()` / `WakeupInternal()` を自分で呼びます。同じ状態に変えると警告のログが出ます。
- Sleep の LIN コントローラは、届いたフレーム（`LinTransmission`）を、go-to-sleep フレームも含めて捨てます。dynamic response のヘッダーイベントと wakeup パルスは、状態によらず届きます。そのため `LinSlave` は、SIL Kit のコントローラを Sleep にしません。
- dynamic response の Slave には、Rx の設定と関係なく、すべてのフレームが送信どおりの状態で届きます。`SilKitLinPort` は Ok のもの（go-to-sleep フレームを除く）を渡し、`LinSlave` は購読した ID だけを渡します。
- `SendDynamicResponse` は、その ID の応答を `TxUnconditional` とする更新をほかのコントローラに配ります。v5.0.8 の Master は、`SendFrameHeader` でこれを応答するノードとして数えるので、2 つの dynamic response の Slave が同じ ID に一度ずつ応答すると、以後そのヘッダーを送らずに `LIN_RX_ERROR` を通知していました。パッチ 0001 で、dynamic response のノードを数えないようにしています（「SIL Kit へのパッチ」）。
- 2 つのノードが同じヘッダーに応答すると、Master には両方の応答が `LIN_RX_OK`（Master が送るフレームなら `LIN_TX_OK`）で届きます。trivial simulation は衝突をエラーにしません（上流のテストにも既知の問題として書かれています）。`LinMaster` は、1 つのスロットに 2 つ目の応答が届いたら Error とします。
- Rx の設定と応答のどちらかのチェックサムモデルが `LinChecksumModel::Unknown` だと、trivial simulation はチェックサムモデルを比べません。vehicle-sil の `LinChecksumModel` には Classic と Enhanced しかないので、`SilKitLinPort` が `Unknown` を SIL Kit に渡すことはありません。
- Master が送るフレーム（`SendFrame` の `MasterResponse`）は、`SendFrame` の中では送られません。SIL Kit がヘッダーを Master 自身に配送するとき（I/O スレッドのタスク）に作られ、そこからさらにタスクとして送られます。Master 自身のフレームの状態（`LIN_TX_OK`）も、そのヘッダーの配送の中で通知されます。タイムスタンプは、そのときの Master の `Now()` です。
- SIL Kit の `SendMsg` は、送信を I/O スレッドのタスクとして積むだけです。ブロッキング版のステップハンドラでは、ハンドラが戻るとすぐに次のステップの時刻を通知するタスクが積まれるので、上の 2 段目の送信はその通知より後になります。また、I/O スレッドは 1 回の読み取りで届いた複数のメッセージを続けて処理し（`VAsioPeer::DispatchBuffer`）、その中で次のステップを直接実行することがあります（`ReceiveNextSimTask`）。非同期版では、`CompleteSimulationStep` が完了の処理をタスクとして積み、ステップが完了するまで次のステップを実行しません。`ParticipantRuntime` はこれを使って、Master のフレームを次のステップの時刻の通知より前に送らせています。

## 混ぜてよい SIL Kit の版

確かめているのは v5.0.8 だけです（利用側の `SilKit::SilKit`、ほかの PC の participant、外で立てた registry のどれについても）。SIL Kit は 4.0.5 から、版の違う participant どうしがつながることを約束していますが、vehicle-sil が頼っている挙動には、それより後の版で入ったものがあります。下は SIL Kit の変更履歴から拾ったもので、古い版と組んで試してはいません。

| 版 | 入った挙動 | 古い版と組むと |
|---|---|---|
| 4.0.24 | メッセージは API を呼んだ順に送られる。直接つなげない相手とは registry が中継する | Control の順序の前提が成り立たない。ファイアウォールの内側の participant とつながらない |
| 4.0.26 | CommunicationReady の前に、購読の確認を待つ | 始まる前の窓と、コントローラーの最初の設定が相手に届く保証がない |
| 4.0.31 | autonomous の participant が、進んだ仮想時間に途中から入れる。coordinated の participant が入ろうとすると abort | 途中からの参加の動きが違う |
| 4.0.34 | Error になるのは `StartLifecycle` の後だけ。切れた相手が Error として届くのは、lifecycle を始めた相手だけ | いなくなった participant の見つけ方が変わる |
| 4.0.39 | 片方向にしかつなげないとき、反対側からつなぎ直す | 複数の PC で、片側だけが待ち受けられる構成がつながらない |
| 4.0.55 | registry が、同じ名前の participant を毎回断る | registry が古いと、同じ名前の 2 つ目が黙って入る（`create_participant()` の作り直しが、これに頼っています） |
| 5.0.7 | 切れた相手を、購読の確認の待ちから外す。`Experimental.TimeSynchronization.DynamicSimulationStep` | 切れた相手がいると、コントローラーの作成が戻らないことがある。古い版は既定の構成のこのキーを知らない（読み込みがどうなるかは確かめていません） |
| 5.0.8 | 角括弧つきの IPv6 のアドレス。Stopping を通らない停止でも止まる | IPv6 の registry の URI が使えないことがある |

## 上流の既知の不具合

### SIL Kit: `ISystemMonitor::SetParticipantConnectedHandler` のデータ競合

- **内容**: `SystemMonitor::SetParticipantConnectedHandler()` は、`_participantConnectedHandler` への代入をロックの外で行います。`SetParticipantDisconnectedHandler()` も同じです。一方、I/O スレッドの `OnParticipantConnected()` も、同じハンドラをロックの外で読んで呼び出します（`SilKit/source/services/orchestration/SystemMonitor.cpp`）。participant の作成後にこれらを呼ぶと、ほかの参加者の接続と同時に起きたときに未定義動作になります。過去の接続を再生した直後から代入するまでの間に接続した参加者は、通知されないこともあります。
- **検出**: これらの API を使うと、ThreadSanitizer が data race を報告します。
- **対応**: vehicle-sil はこの 2 つの API を使いません。他参加者の状態は `AddParticipantStatusHandler` だけで追跡します。SIL Kit の中では、こちらは同期されたハンドラリストで管理されています（ハンドラーの呼び出しと、登録・削除が重なってもよい）。ただし、公開の C++ API の包みは、登録したハンドラーを錠のない表に持つので、登録と削除を 2 つのスレッドから同時に呼べません。`ParticipantRuntime` は、状態を読み直すための登録と削除（`refresh_peer_statuses()`、`wait_for_peer()`）を 1 度に 1 つのスレッドに限ります（試験 `core.peer_statuses_can_be_re_read_from_several_threads_at_once`。ThreadSanitizer で確かめます）。lifecycle を開始した参加者が正常に終了せずに切断した場合も、SIL Kit は状態 `Error` を発行するので検知できます。上流のコードは変更していないので、パッチは不要です。
- **上流への報告**: 未報告です。

### SIL Kit: `AddParticipantStatusHandler` 登録中のステータスの取りこぼし

- **内容**: `SystemMonitor::AddParticipantStatusHandler()` は、次の順で処理します（`SystemMonitor.cpp`）。
  1. ロックを取り、接続済みの参加者の現在のステータスを、新しいハンドラに再生する。
  2. ロックを外す。
  3. ハンドラをリストに追加する。

  2 と 3 の間に届いたステータスは、新しいハンドラには届きません。それが相手の最後のステータス（Running など）だと、ハンドラはその状態を観測できないままになります。
- **影響**: まれに、相手がすでに Running なのに、それを観測できないまま待ち続けます。
- **対応**: `ParticipantRuntime::wait_for_peer()` は、待っている間 200 ms ごとに再同期します（`resync_peers()`）。再同期では、一時的なハンドラを登録してすぐ外します。登録時の再生はロックの下で正しく行われるので、取りこぼしたステータスを取り戻せます。
  - 再生されたステータスが、すでに届いている新しいステータスを上書きしないように、`enterTime` を比べます。
  - 通常の通知は常に反映します。切断時の `Error` はローカルの時計で付けられるため、相手の時計とは比べません。
  - `ISystemMonitor::ParticipantStatus()` は使いません。ロックの下で内部データへの参照を返し、呼び出し側がロックの外でそれを読むためです。
- **上流への報告**: 未報告です。

### SIL Kit（Windows）: スレッドを止められたプロセスとの local-domain ソケットを閉じると戻らないことがある

- **見たこと**: SIL Kit は、同じ機械の participant どうしを、既定で local-domain ソケット（Windows では AF_UNIX）でつなぎます。Windows で、シミュレーションの実行中に 1 つの participant の全部のスレッドを `SuspendThread` で止め、そのまま controller の participant を破棄すると、まれに、止めた相手とのソケットを閉じる呼び出し（I/O スレッドの上の `closesocket()`）が戻りませんでした。SIL Kit は participant の破棄で I/O スレッドの終わりを待つので（`VAsioConnection::~VAsioConnection()`）、破棄が戻りません。このとき、プロセスに自分を終わらせる呼び出し（`TerminateProcess`）をしても、プロセスは残りました。同じ手順を、local-domain ソケットを使わない構成（`Middleware.EnableDomainSockets: false`、TCP だけ）で繰り返した範囲では、起きませんでした。Linux の同じ試験でも起きていません。
- **推測（確かめていないこと）**: 止められたスレッドが、AF_UNIX のソケットのシステムの呼び出しの途中にいて、こちらの閉じる処理がそれを待つ。そのためにプロセスも終われない。デバッガーの一時停止もスレッドを止めるので、同じことが起きる。TCP の接続は、相手のプロセスの状態によらずに閉じられる。
- **影響**: 推測のとおりなら、1 つの participant をデバッガーで止めたまま全体を止めると、まれに、ほかの participant やランチャーが終わらなくなります。
- **対応**: vehicle-sil の participant（`ParticipantRuntime`）は、Windows では、構成を渡されなければ `Middleware.EnableDomainSockets: false` で作ります。vehicle-sil が立てる registry（`vehicle_sil_launch run`、`Simulation`）は、どの OS でもそうします（TCP のポートで待ち受けられないとき、SIL Kit の registry は local-domain ソケットだけで動き続け、同じ URI のほかの registry の local-domain のパスを奪うため。[simulation-run.md](simulation-run.md) の「待ち受けるアドレス」）。既定を作るのは 1 か所です（`default_participant_configuration()`、`default_participant_configuration_with()`、`merged_participant_configuration()`、registry は `default_registry_configuration()`。ほかに変える所は [internals/core.md](internals/core.md) の「SIL Kit の構成の既定」）。`vehicle_sil_can_bus_simulator` が自分で作る構成（ログの設定）も、これに足して作ります。相手が local-domain の口を持っていても、こちらが使わないので TCP でつながります。自分の SIL Kit の構成を渡したとき（`participant_configuration`、ecu_main の `--silkit-config`、`vehicle_sil_can_bus_simulator --participant-configuration`）も、Windows では `Middleware.EnableDomainSockets` を `false` にします（`true` と書いても `false` です）。vehicle-sil を使わない SIL Kit の participant どうしの接続には、この対応は効きません。
- **守り方**: `libs/`、`tools/`、`bridges/`、`tests/` で、文字列から SIL Kit の構成を作る所（`ParticipantConfigurationFromString(`）が上の関数を通っていることを、試験 `core.silkit_configuration_defaults`（`cmake/VehicleSilCheckSilKitConfiguration.cmake`）が確かめます。
- **安全網**: `vehicle_sil_launch run` は、自分の SIL Kit の物（変数の口、controller、registry）を専用のスレッドで破棄し、期限まで待ちます。戻らなければ、所見 `connection-not-closed` を足して結果を書き、静的なオブジェクトの破棄をせずにプロセスを終えます（`RunOutcome::connection_not_closed_ms`、[simulation-run.md](simulation-run.md) の「止め方」）。上の「見たこと」の場面では、結果は書けましたが、プロセスは残りました。
- **上流への報告**: 未報告です。

## SIL Kit へのパッチ

`patches/sil-kit/` のパッチは、上流のタグ `v5.0.8`（固定コミット）に `git format-patch` で作ったもので、上流のスタイル（clang-format）に合わせています。0001 は上流の統合テストを含みます。

| パッチ | 内容 | vehicle-sil が必要とする理由 | 上流の再現テスト |
|---|---|---|---|
| 0003 | registry が、切れた peer の名前の登録（`_participantNameToPeer`）を消すときに、その名前の登録がその peer のものであるときだけ消す。registry は、名前が使用中かを確かめる前に、来た peer にその名前を付けるので、同じ名前で断った peer が切れると、先にいた participant の登録が消えていた | 同じ名前で断られた participant（`--name` の誤り、すぐに起動し直したプロセス。`create_participant()` は断られる間に作り直す）の後では、先にいた participant に直接つなげない participant（ファイアウォールの内側、local-domain ソケットだけで待つ）が、registry の中継（`RemoteParticipantConnectRequest`）でつなげず、参加の待ちの時間切れになる | なし（vehicle-sil の `core.a_refused_name_leaves_the_registry_able_to_relay_to_the_participant_that_has_it` で確かめる） |
| 0002 | CAN コントローラーが trivial と detailed の振る舞いを切り替えるポインタ（`SimBehavior::_currentBehavior`）を atomic にする。切り替えは I/O スレッドのサービス発見で起き、同時にコントローラーのスレッドが `Start()` などでそのポインタを読むことがある | SIL Kit のスレッドの外からコントローラーを呼ぶ所が残っている: `SilKitCanPort::close()` の `Stop()`（止める側のスレッド。シミュレーターが先に抜けて trivial に戻る切り替えと重なりうる）、実時間のポートの送信（ドライバーのスレッド）、Vector XL CAN Bridge の送信（XL の受信スレッド）。`SetBaudRate()` と `Start()` は CommunicationReady のハンドラー（SIL Kit のスレッド）から呼ぶので、当たらない。もとは、生成時に `Start()` を呼んでいたときに、簡易 CAN Network Simulator を別プロセスで動かす試験で ThreadSanitizer が data race を報告した | なし（vehicle-sil の ThreadSanitizer の試験で確かめる）。シミュレーターが抜けて再び現れたときの 2 回目の切り替えで、切り替え前の振る舞いを使っている呼び出しと、シミュレーターのリンクの書き換えが重なる場合は直していない |
| 0001 | trivial simulation の Master が、`SendFrameHeader` で応答するノードを数えるときに、dynamic response のノードを数えない。dynamic response のノードはヘッダーごとに応答するかを決めるので、以前の `SendDynamicResponse` で常に応答するノードにはならない | `LinSlave` は dynamic response を使う。修正がないと、同じ ID に 2 つの `LinSlave` が一度ずつ応答したあと（LIN 診断の 0x3D は全 Slave が共有する）、その ID のヘッダーが送られなくなる | `ITest_LinDynamicResponse.normal_master_dynamic_slaves_take_turns_on_one_id` |

- **上流への報告**: いずれも未報告です。0001 と 0002 は上流の `main` でも同じコードです（0003 は上流の `main` を確かめていません）。
- **検証**: 0001 は上流の LIN のテスト（単体と統合）で確かめています。0002 は vehicle-sil の簡易 CAN Network Simulator のテスト（`can_netsim.*`）を ThreadSanitizer で回して確かめています（この競合はまれにしか重ならず、パッチを外しても試験で必ず見つかるわけではありません。外すかどうかは、上の表の「残っている所」で判断します）。0003 は、local-domain ソケットだけで待つ participant に、TCP だけの participant が中継でつなぐ試験で確かめています（パッチがなければ、同じ名前で断られた participant の後で時間切れになる）。

## 上流を更新する手順

1. `git -C external/<lib> fetch --tags` のあと、`git -C external/<lib> checkout <コミット>` を実行し、`git add external/<lib>` でステージする。
2. `git submodule update --init --recursive` を実行し、ネストしたサブモジュールを合わせる。
3. この文書の「実装が前提にしている上流の挙動」を見直す。
4. パッチがある場合は、パッチが当たるかを確かめる（configure が失敗しないこと）。当たらなければ、新しいコミットの上でパッチを作り直す（`patches/README.md`）。上流に取り込まれたパッチは削除する。
5. Linux の全プリセットでビルドとテストを実行し、FetchContent テストも実行する。
6. 固定コミットの表を更新してコミットする。
