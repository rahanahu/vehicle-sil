# 依存ライブラリ

vehicle-sil 自体を直す人（SIL Kit の固定、パッチ、上流の更新）と、gRPC や XL Driver Library を用意する人向けです。直す人は、先に [architecture.md](architecture.md) を読みます。この後に読む文書はありません。

vehicle-sil は上流ライブラリの SIL Kit を Git サブモジュールとして取り込み、特定のコミットに固定しています。SIL Kit にはパッチ（「SIL Kit へのパッチ」）を当ててビルドします。サブモジュールの作業ツリーは変更しません。J1939 のスタックは vehicle-sil 自身のもの（`vehicle_sil::j1939`）で、外部のライブラリを使いません。

| ライブラリ | パス | 固定コミット | 由来 | ライセンス |
|---|---|---|---|---|
| Vector SIL Kit | `external/sil-kit` | `52ad61d5399483c395585d5feee89d2e89fc74b1` | タグ `v5.0.8` | MIT |

SIL Kit は自身のサブモジュール（`ThirdParty/asio`、`fmt`、`spdlog`、`googletest`）を持つため、`git submodule update --init --recursive` が必要です。FetchContent はこれを自動で行います。

### gRPC と Protocol Buffers（Control Server、任意）

- `VEHICLE_SIL_BUILD_CONTROL_SERVER=ON`（既定 OFF）のときだけ使います。`vehicle_sil::control_grpc`、`proto/` の生成コード、`vehicle_sil_control_server`、`vehicle_sil_launch`（`run --control`）、`examples/control` が対象で、ほかのライブラリ（`vehicle_sil::launch` を含む）と ECU の実行ファイルは使いません。
- `find_package(gRPC CONFIG)` で見つかればそれを使います（MSYS2 UCRT64 は `pacman -S mingw-w64-ucrt-x86_64-grpc`）。見つからなければ FetchContent で gRPC を取り込みます（GitHub の `grpc/grpc`、タグ `v1.76.0`、`VEHICLE_SIL_GRPC_VERSION` で変えられる。サブモジュールは abseil、protobuf、re2、c-ares、BoringSSL、zlib だけ）。取り込んだ gRPC はインストールのルールを作らず（abseil の export 用の設定だけ有効）、警告の対象外（`SYSTEM`）にします。FetchContent で取り込むには CMake 3.28 以上が要ります（それより古い CMake では、gRPC を入れて `find_package` で見つけさせます）。取得済みのソースは `-DFETCHCONTENT_SOURCE_DIR_GRPC=<path>` で再利用できます。
- ライセンス: gRPC は Apache-2.0、Protocol Buffers は BSD-3-Clause、abseil は Apache-2.0、re2 は BSD-3-Clause、c-ares は MIT、BoringSSL は OpenSSL/ISC 系、zlib は zlib ライセンス。vehicle-sil のリポジトリには含めません。
- Python のクライアント（`python/vehicle_sil_control`）は `grpcio` と `protobuf` を、スタブの生成には `grpcio-tools` を使います（`pyproject.toml` の `control` グループ。uv がなければ pip、MSYS2 は `mingw-w64-ucrt-x86_64-python-grpcio` と `-python-grpcio-tools`）。試験はこれらが入った Python 3.11 以上（`VEHICLE_SIL_CONTROL_PYTHON`。指定がなければ、下の「Python の探し方」の順）があるときだけ登録します。

### Python の探し方

- 版と依存は、リポジトリ直下の `pyproject.toml`（`requires-python` と `[dependency-groups]`。`codegen` は PyYAML、`control` は grpcio、grpcio-tools、protobuf、`gui` は GUI サーバー（`gui/server`、workspace のメンバー）と pytest、`dev` はすべてで既定）、`.python-version`（開発で使う版）、`uv.lock`（解決した版の固定）で決めています。版の下限は、試験を実際に回している最も古い版です（`scripts/verify-python.py` が、uv の lowest-direct の解決で下限そのものを Python 3.11 に入れて確かめます）。上限は付けていません。
- vehicle-sil が最上位のプロジェクトのとき、configure はリポジトリの `.venv/bin/python`（`uv sync` が作る）が Python 3.11 以上で必要なモジュールを import できれば、それを使います。そうでなければ `find_package(Python3 3.11)` で探します（有効にした仮想環境が先。`-DPython3_EXECUTABLE=` で指定できる）。処理は `cmake/VehicleSilPython.cmake` にあります。
- 生成器の Python は configure のたびに探し直します。`uv sync` で `.venv` を作ったり、`.venv` にパッケージを足したりした後は、普通に configure し直せば使います（`--fresh` は要りません）。
- FetchContent や `add_subdirectory` で取り込んだ利用側のビルドは、vehicle-sil の `.venv` を使いません。利用側の Python（有効にした仮想環境、`Python3_EXECUTABLE`、`PATH`）を使います。
- Windows では `.venv` を候補にしません（uv の Python を MinGW のビルドから使うことは確かめていないため）。MSYS2 UCRT64 の pacman のパッケージの Python を、今までどおり探して使います。

### Python 3 と PyYAML（J1939 の型付きメッセージの生成）

- `vehicle_sil_add_j1939_codec()` が、ビルドのときに `tools/j1939_codegen/j1939_codegen.py` を動かします。Python 3.11 以上と PyYAML（`import yaml`）が要ります。configure のときに上の順で探し、PyYAML を import できるかを確かめます。なければ、この関数を呼んだ時点で configure が失敗し、試した Python とそれぞれを使わなかった理由（`No module named 'yaml'`、3.11 より古い版など）と、入れ方（uv、pip、OS と MSYS2 のパッケージ）を示します。Control API の試験を見送るときの configure の表示も、同じように試した Python と理由を示します。
- vehicle-sil の試験と例（`VEHICLE_SIL_BUILD_TESTS`、`VEHICLE_SIL_BUILD_EXAMPLES`。トップレベルのビルドでは既定で ON）はこの関数を使うので、そのビルドには Python と PyYAML が要ります。ライブラリだけを使う利用側（FetchContent など）は、関数を呼ばなければ要りません。
- PyYAML は YAML 1.1 で読みます。生成器は `on`、`off`、`yes`、`no` を真偽値にせず文字列として読み、整数は 10 進（先頭の 0 なし）、16 進、2 進だけを受け付け（8 進と、整数と小数の 60 進を誤りにする）、同じキーの重複を誤りにします（マージキーで取り込んだキーの上書きは可）。
- Python や PyYAML が見つからなかった configure の結果はキャッシュに残りません。入れたあとで configure し直せば見つけます。

### aiohttp（ブラウザの GUI のサーバー）

- GUI サーバー（`gui/server`、配布名 `vehicle-sil-gui`、[gui.md](gui.md)）は aiohttp だけに依存します。HTTP のサーバー、WebSocket、試験で使うクライアントを 1 つで持ち、asyncio の Windows の既定のイベントループ（子プロセスを動かせる ProactorEventLoop）で動き、Windows（3.11 以降）の wheel があります。C++ のビルドは使いません。
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
| 設定 | `xlCanSetChannelBitrate`（init 権限が要る、15000〜1000000 bit/s）、`xlCanSetChannelMode(tx=0, txrq=0)`（init 権限が要る。権限があるときだけ呼ぶ） |
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
  - パッチが当たらなければ configure は失敗します。`git apply` を使うので Git が必要です。親の Git リポジトリや、継承した `GIT_DIR` などの環境変数の影響は受けません。
  - `--ignore-whitespace` で当てるので、CRLF でチェックアウトしたサブモジュール（Windows の `core.autocrlf`）にも当たります。パッチファイル自体は `.gitattributes` で改行を変換しません。
- 利用側がすでに `SilKit::SilKit` を定義している場合は追加しません。利用側の SIL Kit に「SIL Kit へのパッチ」がないと、同じ LIN ID に複数の `LinSlave` が交代で応答する構成（診断の 0x3D など）が動きません。パッチが効くのはヘッダーを送る Master のプロセスの SIL Kit なので、パッチのない SIL Kit を使う外部の Master（CANoe など）と組み合わせた場合も同じです。
- 上流の公開インクルードディレクトリを SYSTEM 扱いにします。上流ヘッダーの警告で、vehicle-sil のターゲットが `-Werror` 付きのビルドに失敗しないようにするためです。
- 上流のオプションは関数スコープの通常変数で上書きします（CMP0077）。利用側のキャッシュは変えません。

| ライブラリ | 設定 | 理由 |
|---|---|---|
| SIL Kit | `SILKIT_BUILD_DEMOS/TESTS/DOCS/DASHBOARD=OFF`、`SILKIT_BUILD_STATIC=OFF`、`SILKIT_BUILD_UTILITIES=ON` | 不要なものを作らない。統合テストに `sil-kit-registry` が要る |

## 使っているターゲット

| ターゲット | 中身 |
|---|---|
| `SilKit::SilKit` | SIL Kit 本体。共有ライブラリで、Debug では `libSilKitd.so` / `SilKitd.dll` になる。出力先は `${CMAKE_BINARY_DIR}/$<CONFIG>` |
| `sil-kit-registry` | レジストリの実行ファイル。SIL Kit を静的にリンクしているので、共有ライブラリは不要 |
| `rapidyaml` | SIL Kit が `ThirdParty/rapidyaml` からビルドする YAML/JSON パーサー（MIT。静的ライブラリ）。`vehicle_sil::can_netsim` と `vehicle_sil::launch` が設定ファイルの読み込みに使う。SIL Kit をサブモジュールから追加しないとき（利用側が `SilKit::SilKit` を定義済み）はこのターゲットがないので、`vehicle_sil::can_netsim`、`vehicle_sil_can_bus_simulator`、`vehicle_sil::launch`、`vehicle_sil_launch` を作らない |

Windows では `SilKit.dll` が実行ファイルと別のディレクトリに出力されます。`vehicle_sil_copy_runtime_dlls(<target>)` を呼ぶと、ビルド後に DLL が実行ファイルの隣へコピーされます。

SIL Kit 本体は C++17 でビルドされます。C++ API はヘッダーのみで C API を包む構成なので、利用側を C++20 でコンパイルしても問題ありません。MinGW-w64 は上流が対応しています（`-Wa,-mbig-obj` など）。

## 使っている API

| ライブラリ | 用途 | API |
|---|---|---|
| SIL Kit | 参加 | `Config::ParticipantConfigurationFromString`、`CreateParticipant(config, name, registryUri)` |
| SIL Kit | lifecycle | `CreateLifecycleService({OperationMode::Autonomous / Coordinated})`、`StartLifecycle`、`Stop`、`State`、`ReportError`、`SetCommunicationReadyHandler`、`SetCommunicationReadyHandlerAsync` と `CompleteCommunicationReadyHandlerAsync`（始まる前の窓。`SimulationController::set_start_window()`） |
| SIL Kit | 時刻同期 | `ILifecycleService::CreateTimeSyncService`、`ITimeSyncService::SetSimulationStepHandlerAsync` と `CompleteSimulationStep`（ハンドラの最後で完了させる） |
| SIL Kit | system controller（experimental） | `Experimental::Participant::CreateSystemController`、`SetWorkflowConfiguration`、`AbortSimulation` |
| SIL Kit | LIN（`vehicle_sil::lin_silkit`） | `CreateLinController`、`ILinController::Init`、`SetFrameResponse`、`SendFrameHeader`、`AddFrameStatusHandler` |
| SIL Kit | LIN dynamic response（experimental） | `Experimental::Services::Lin::InitDynamic`、`AddFrameHeaderHandler`、`SendDynamicResponse` |
| SIL Kit | publish/subscribe（`vehicle_sil::control_silkit`、examples/virtual_time） | `CreateDataPublisher`（履歴 0。hello だけ履歴 1）、`CreateDataSubscriber`、`IDataPublisher::Publish`、`PubSubSpec`（トピックとメディアタイプ。ラベルは使わない） |
| SIL Kit | 他参加者の監視 | `CreateSystemMonitor`、`AddParticipantStatusHandler`（`SetParticipantConnectedHandler` と `SetParticipantDisconnectedHandler` は使わない。「上流の既知の不具合」を参照） |
| SIL Kit | CAN | `CreateCanController`、`ICanController::Start/Stop/SendFrame/SetBaudRate`、`AddFrameHandler`、`AddFrameTransmitHandler`。`SendFrame` の userContext に送信時刻を入れる（簡易 CAN Network Simulator との取り決め） |
| SIL Kit | Network Simulator（experimental、`vehicle_sil::can_netsim`） | `Experimental::Participant::CreateNetworkSimulator`、`INetworkSimulator::SimulateNetwork/Start`、`ISimulatedNetwork`、`Can::ISimulatedCanController`、`Can::ICanEventProducer::Produce`（フレーム、送信確認、状態の変化） |
| SIL Kit | プロセス内レジストリ（FetchContent の例） | `Vendor::Vector::CreateSilKitRegistry`、`StartListening` |

## 実装が前提にしている上流の挙動

上流を更新するときは、次の挙動が変わっていないかを確認してください。

**SIL Kit**
- `RemoveFrameHandler` は、実行中のハンドラの終了を待ってから戻ります。ハンドラの呼び出しと削除が同じ `recursive_mutex` で直列化されているためです。`SilKitCanPort::close()` はこれを前提に、ハンドラを削除してから受け箱を捨てます。
- `ParticipantStatus` は履歴長 1 で配信されます。後から参加した participant も、既存の参加者の最新状態を受け取れます。`ParticipantRuntime::wait_for_peer` はこれを使います。
- 購読が確立する前に送ったフレームは届かないことがあります。そのため、コントローラは `ParticipantRuntime::start()` の前に作ります。
- `sil-kit-registry --listen-uri silkit://127.0.0.1:0 --generate-configuration <file>` は空きポートで待ち受け、実際の URI を含む設定ファイルを原子的に書き出します。統合テストはこれを使って、並列実行でもポートが衝突しないようにしています。
- 時刻同期した participant は、自分の次のステップ時刻がほかの全員の次のステップ時刻以下のときだけ進みます（`TimeConfiguration::OtherParticipantHasLowerTimepoint`）。「最も遅いピアを待つ」と `SimulationController` の stop_at の保証は、これに基づいています。
- Agent と Hub の間を publish/subscribe で運ぶ層（`vehicle_sil::control_silkit`）と始まる前の窓は、次の挙動を前提にしています。試験 `control_silkit.*` と `launch_control.*` が、まとめて確かめます（どれかが変わると、`HubTransportStats::late_messages` が 0 でなくなるか、窓の中の書き込みが時刻 0 のステップに間に合わなくなります）。
  - `IDataPublisher::Publish` は、呼んだスレッドでデータを写し、送信を participant の I/O スレッドの待ち行列に積むだけです。どのスレッドから呼んでも安全で、1 つの相手には積んだ順に届きます。
  - ステップの完了（`CompleteSimulationStep`）も同じ待ち行列に積まれます。そのため、ステップの中で送ったメッセージは、そのステップの次の時刻の知らせより前に相手に届きます。上の「進む条件」と合わせて、受け手のステップ P が始まる時点で、P より前に始まった送り手のステップのメッセージは全部届いています（ちょうど P のステップの分は決まりません）。ステップの外（別のスレッド、受信のハンドラー）から送ったメッセージには、この関係がありません。
  - 受信のハンドラーは、participant の I/O スレッドで、届いた順に 1 つずつ呼ばれます。ステップのハンドラーも同じスレッドです。時間ゲートがステップを保留している間も、I/O スレッドは空いていて、メッセージは届きます。
  - 履歴 1 の発行者が最後に送ったメッセージは、後から購読した相手にも届きます（発行者の participant がつながっている間）。participant を始める前に送った hello も届きます。
  - Coordinated の participant の CommunicationReady のハンドラーは、必須の participant が全員 ServicesCreated になり、全員から返事が届き、自分の publish/subscribe の購読が確認された後で呼ばれます（ほかの participant は、その時点でまだ CommunicationInitializing の途中のことがあります。コマンドは受け取れます）。相手は、購読の知らせを受けると、履歴（hello）を送ってから確認を返すので、必須の ECU が始める前に送った hello は、窓が開く前に届いています。非同期のハンドラーでは、`CompleteCommunicationReadyHandlerAsync` を呼ぶまで、誰も ReadyToRun に進まず、どのステップも始まりません。その間、publish/subscribe は届きます。`CompleteCommunicationReadyHandlerAsync` は、ハンドラーが呼ばれる前に呼ぶと無視されます（`ParticipantRuntime` が、呼ばれた後にだけ渡します）。
  - 始めたライフサイクルを持つ participant が、Shutdown せずに接続を切ると、ほかの participant にはその状態が Error として届きます（`HubTransport` が、いなくなった Agent の接続を終えるのに使います）。
  - participant の構成の `EnableMessageAggregation` は既定の Off のままであること（On にした participant は、データのメッセージを溜めてから送るので、上の順序の前提が変わります）。
  - participant を壊すとき、すでに I/O スレッドの待ち行列に積んだ送信は、接続を閉じる処理より前に実行されます（Agent の「最後の知らせ」が届くのは、これによります。届かなくても、接続は `HubTransport` を壊すときに終わります）。
- 時刻同期した participant が送るメッセージのタイムスタンプは、送信時のステップの時刻です。
- CAN の `SendFrame` の userContext は、ポインタの値（64 ビット）のまま運ばれます。ネットワークシミュレーターの `CanFrameRequest` と送信確認には届き、他の受信者の RX のフレームでは `nullptr` になります（`CanController::ReceiveMsg`）。`SilKitCanPort` は送信時刻を入れ、簡易 CAN Network Simulator がそれを読みます。
- Network Simulator API（experimental）: あるネットワークの模擬を引き受ける participant が現れると、そのネットワークの CAN コントローラーは詳細シミュレーションに切り替わり、送信の要求、ボーレート、モードの変更をその participant に送ります。コントローラーの状態（`Started` など）は、シミュレーターが返す状態の変化で初めて変わります。`ControllerDescriptor` は見つけた順の通し番号で、決定的ではありません。
- コントローラーがシミュレーターを見つける前に呼んだ `SetBaudRate()` と `Start()` は、trivial simulation で処理され、見つけた後もシミュレーターに送り直されません。すでに見つけているシミュレーターは、コントローラーを作った時点で同じスレッドから知らされます。そのため、シミュレーターを先に起動しても、ピアのコントローラーの設定が届くとは限りません。簡易 CAN Network Simulator は、ビットレートが届かなければ `--default-bitrate` を使い、フレームの配送をモードで止めません。シミュレーションが始まった後の送信は、すべてシミュレーターを通ります。
- `ReportError` で Error になった participant は、`Stop` か abort されるまで Error のままで、`StartLifecycle` の future も完了しません。`Stop` は Error から Shutdown に進めます。
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

## 上流の既知の不具合

### SIL Kit: `ISystemMonitor::SetParticipantConnectedHandler` のデータ競合

- **内容**: `SystemMonitor::SetParticipantConnectedHandler()` は、`_participantConnectedHandler` への代入をロックの外で行います。`SetParticipantDisconnectedHandler()` も同じです。一方、I/O スレッドの `OnParticipantConnected()` も、同じハンドラをロックの外で読んで呼び出します（`SilKit/source/services/orchestration/SystemMonitor.cpp`）。participant の作成後にこれらを呼ぶと、ほかの参加者の接続と同時に起きたときに未定義動作になります。過去の接続を再生した直後から代入するまでの間に接続した参加者は、通知されないこともあります。
- **検出**: これらの API を使うと、ThreadSanitizer が data race を報告します。
- **対応**: vehicle-sil はこの 2 つの API を使いません。他参加者の状態は `AddParticipantStatusHandler` だけで追跡します。こちらは同期されたハンドラリストで管理されています。lifecycle を開始した参加者が正常に終了せずに切断した場合も、SIL Kit は状態 `Error` を発行するので検知できます。上流のコードは変更していないので、パッチは不要です。
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

## SIL Kit へのパッチ

`patches/sil-kit/` のパッチは、上流のタグ `v5.0.8`（固定コミット）に `git format-patch` で作ったもので、上流のスタイル（clang-format）に合わせています。0001 は上流の統合テストを含みます。

| パッチ | 内容 | vehicle-sil が必要とする理由 | 上流の再現テスト |
|---|---|---|---|
| 0002 | CAN コントローラーが trivial と detailed の振る舞いを切り替えるポインタ（`SimBehavior::_currentBehavior`）を atomic にする。切り替えは I/O スレッドのサービス発見で起き、同時にコントローラーのスレッドが `Start()` などでそのポインタを読むことがある | ネットワークシミュレーターより先に作った CAN コントローラー（`SilKitCanPort` は生成時に `Start()` を呼ぶ）で、ThreadSanitizer が data race を報告する。簡易 CAN Network Simulator を別プロセスで動かす試験で起きた | なし（vehicle-sil の ThreadSanitizer の試験で確かめる）。シミュレーターが抜けて再び現れたときの 2 回目の切り替えで、切り替え前の振る舞いを使っている呼び出しと、シミュレーターのリンクの書き換えが重なる場合は直していない |
| 0001 | trivial simulation の Master が、`SendFrameHeader` で応答するノードを数えるときに、dynamic response のノードを数えない。dynamic response のノードはヘッダーごとに応答するかを決めるので、以前の `SendDynamicResponse` で常に応答するノードにはならない | `LinSlave` は dynamic response を使う。修正がないと、同じ ID に 2 つの `LinSlave` が一度ずつ応答したあと（LIN 診断の 0x3D は全 Slave が共有する）、その ID のヘッダーが送られなくなる | `ITest_LinDynamicResponse.normal_master_dynamic_slaves_take_turns_on_one_id` |

- **上流への報告**: どちらも未報告です。上流の `main` でも同じコードです。
- **検証**: 0001 は上流の LIN のテスト（単体と統合）で確かめています。0002 は vehicle-sil の簡易 CAN Network Simulator のテスト（`can_netsim.*`）を ThreadSanitizer で回して確かめています。

## 上流を更新する手順

1. `git -C external/<lib> fetch --tags` のあと、`git -C external/<lib> checkout <コミット>` を実行し、`git add external/<lib>` でステージする。
2. `git submodule update --init --recursive` を実行し、ネストしたサブモジュールを合わせる。
3. この文書の「実装が前提にしている上流の挙動」を見直す。
4. パッチがある場合は、パッチが当たるかを確かめる（configure が失敗しないこと）。当たらなければ、新しいコミットの上でパッチを作り直す（`patches/README.md`）。上流に取り込まれたパッチは削除する。
5. Linux の全プリセットでビルドとテストを実行し、FetchContent テストも実行する。
6. 固定コミットの表を更新してコミットする。
