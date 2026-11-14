# vehicle-sil

[Vector SIL Kit](https://github.com/vectorgrp/sil-kit) の上で、車載通信のシミュレーションピアを作るための C++20 ライブラリです。J1939 over CAN と LIN に対応しています。ピアは Windows と Linux の独立したプロセスとして動きます。

> [!NOTE]
> vehicle-sil は最初のリリースの前です。公開 API は、互換性を保たずに変わることがあります。わかっている制限は「[既知の制限](#既知の制限)」に、全体の形と設計の決まりは [docs/architecture.md](docs/architecture.md) に、契約と制約の詳細は [docs/internals/](docs/architecture.md#中の仕組み) の各文書にあります。

使えるのは、vehicle-sil 自身の J1939 スタック（単一フレーム、Request と Acknowledgement、Address Claim、トランスポートプロトコル、診断、ECU アプリ向けの部品）、SIL Kit と Vector XL Driver をつなぐ CAN ブリッジ（Vector Virtual CAN と VN5620 の物理 CAN ループバックで確認済み）、LIN の Master/Slave（スケジュールテーブル、フレームの送受信、診断フレーム 0x3C/0x3D、応答の衝突の検出、Sleep/Wakeup）と、それらを同期させる共通仮想時間です。

## どれを読むか

役割ごとに、読む順と、読まなくてよい文書です。docs の各文書の冒頭にも、対象と前後に読むものを書いています。

| 役割 | 読む順 | 読まなくてよいもの |
|---|---|---|
| ECU を書く人（C/C++ で 1 つの ECU を作る） | 「[取得とビルド](#取得とビルド)」の「ECU を作って動かす」（ひな形とビルド済みの道具）→ 「[最初の ECU を動かす](#最初の-ecu-を動かす)」→ [examples/getting_started](examples/getting_started) → [docs/architecture.md](docs/architecture.md)（全体の形）→ 手引き「[最初の ECU を作る](docs/howto/first-ecu.md)」（実行ファイルにして、構成ファイルで動かし、Python から変数を読み書きするまで）→ [docs/j1939-guide.md](docs/j1939-guide.md) → [docs/j1939-codec.md](docs/j1939-codec.md) → [docs/ecu-executable.md](docs/ecu-executable.md)。LIN を使うときは [docs/lin-guide.md](docs/lin-guide.md) も | docs/simulation-*.md、docs/dependencies.md、docs/code-map.md。docs/internals/ は、契約や制限を確かめるときに該当する節だけ |
| まとめる人（各自の ECU の実行ファイルを 1 つのシミュレーションにする） | 「[取得とビルド](#取得とビルド)」の「ECU を作って動かす」（ビルド済みの道具）→ [docs/architecture.md](docs/architecture.md)（全体の形）→ 手引き「[ECU をまとめて動かす](docs/howto/integrate-ecus.md)」（ほかの人の PC から ECU を参加させるなら「[複数の PC で動かす](docs/howto/multi-pc.md)」）→ [docs/ecu-executable.md](docs/ecu-executable.md) → [docs/simulation-config.md](docs/simulation-config.md)（構成ファイル）→ [docs/simulation-check.md](docs/simulation-check.md)（照合）→ [docs/simulation-run.md](docs/simulation-run.md)（実行）→ 決まった手順で値を入れて確かめるなら手引き「[シナリオで試験を書く](docs/howto/scenario-tests.md)」 | docs/j1939-guide.md、docs/j1939-codec.md、docs/dependencies.md、docs/code-map.md、docs/internals/ |
| 試験する人（決まった手順で値を入れて確かめる、CI で回す） | 手引き「[シナリオで試験を書く](docs/howto/scenario-tests.md)」（シナリオを書き、照合して回し、終了コードと結果を読むまで）→ [docs/scenario.md](docs/scenario.md) → [docs/simulation-config.md](docs/simulation-config.md) の「引数と終了コード」 | docs/j1939-guide.md、docs/j1939-codec.md、docs/control-api.md（シナリオは gRPC を使わない）、docs/dependencies.md、docs/code-map.md |
| 外から操作する人（Python、CLI、試験） | 手引き「[信号を外から操作する](docs/howto/operate-signals.md)」（どの口を使うか、読む・書く・待つ、時間の操作）→ [docs/control-api.md](docs/control-api.md)（`vehicle_sil_launch run --control` で、ランチャーの Control Server につなぐ）→ [examples/launch_control](examples/launch_control) → [docs/simulation-events.md](docs/simulation-events.md)（事象と命令）。変数の名前は、`ecu_main` で作った ECU の実行ファイルなら `--describe` の `variables` で調べる（[docs/ecu-executable.md](docs/ecu-executable.md) の「JSON の形式」） | docs/j1939-guide.md、docs/j1939-codec.md、docs/dependencies.md、docs/code-map.md |
| ブラウザで見る人、自分の画面を作る人（GUI を、好きな枠組みで） | [docs/gui.md](docs/gui.md)（起動、画面、変数）→ 自分の画面を作るなら [docs/gui.md](docs/gui.md) の「自分の画面を作る」→ ひな形 [examples/gui_starter](examples/gui_starter) → [gui/client](gui/client/README.md)（クライアントのライブラリ）→ [gui/protocol](gui/protocol/README.md)（サーバーの口） | docs/gui.md の「サンプルの画面」（Angular の部品を写して使うときだけ）、docs/j1939-guide.md、docs/j1939-codec.md |
| 実機の CAN につなぐ人 | [docs/bridges.md](docs/bridges.md)（Vector XL CAN Bridge の使い方、ビルド、契約） | docs/j1939-guide.md、docs/j1939-codec.md、docs/simulation-*.md、docs/code-map.md |
| `Peer` や SIL Kit の participant を自分で組み立てる人 | [docs/j1939-guide.md](docs/j1939-guide.md) → [docs/peers.md](docs/peers.md)（ピアを別々のプロセスで動かす、仮想時間、CAN バスの模擬）→ [docs/internals/](docs/architecture.md#中の仕組み) の該当する文書 | docs/simulation-*.md、docs/gui.md |
| vehicle-sil 自体を直す人 | [AGENTS.md](AGENTS.md) → [docs/code-map.md](docs/code-map.md) → [docs/architecture.md](docs/architecture.md) → [docs/build.md](docs/build.md) → [docs/dependencies.md](docs/dependencies.md) → 変える場所の [docs/internals/](docs/architecture.md#中の仕組み) の文書 | なし（利用者向けの文書は、変える機能のものを読む） |

ライブラリの CMake ターゲットとディレクトリの一覧は、[docs/architecture.md](docs/architecture.md) の「構成」にあります。やりたいこと別のサンプルの索引は [examples/README.md](examples/README.md) です。

## 取得とビルド

### ECU を作って動かす（ひな形とビルド済みの道具）

ECU を書く人とまとめる人は、vehicle-sil のリポジトリをビルドしなくて構いません。手順は 3 つです。Node.js は要りません。ひな形と道具は、[リリースのページ](https://github.com/rahanahu/vehicle-sil/releases)に公開した版にだけあります。公開した版がまだないときは、下の「vehicle-sil 自体をビルドする」の手順で使います。

1. リリースのページから `vehicle-sil-template-<版>.zip`（ECU のプロジェクトのひな形）を取って展開し、自分のプロジェクトにします。中身は 2 つの ECU、構成ファイル、シナリオ、GUI のダッシュボードで、vehicle-sil を FetchContent で取り込みます（[template/](template/) と同じもの）。
2. ひな形の中で `python3 get_tools.py`（Windows の UCRT64 シェルでは `python get_tools.py`）を実行します。リリースから自分の OS のビルド済みの道具（`vehicle_sil_launch` とブラウザの GUI）を取り、ひな形に書いてある SHA-256 で確かめて、`tools/` に展開します。表示されたコマンドで、その `bin` を `PATH` に入れます。
3. ECU をビルドして（`cmake --preset linux`、`cmake --build --preset linux`。Windows は `windows`）、`vehicle_sil_launch check simulation.yaml`、`vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml`、`vehicle-sil-gui simulation.yaml` で動かします。

- 必要なものと各段の詳しい説明は、ひな形の [README](template/README.md) にあります。続きは手引き「[最初の ECU を作る](docs/howto/first-ecu.md)」です。
- 待ち時間は、最初のビルドで SIL Kit をコンパイルする数分だけです（ECU がリンクするライブラリは、ソースからビルドします）。
- ECU と道具は同じ版でそろえます（ひな形の `GIT_TAG` と `get_tools.py` の `VERSION`）。版ごとの変更と、互換を壊す変更の直し方は [CHANGELOG.md](CHANGELOG.md) にあります。
- 道具の圧縮ファイルの中身と作り方は [docs/build.md](docs/build.md) の「ビルド済みの道具」にあります。

### vehicle-sil 自体をビルドする

vehicle-sil を直す人、試験を回す人、ビルド済みの道具がない環境で使う人の手順です。

```sh
git clone --recursive https://github.com/rahanahu/vehicle-sil.git
cd vehicle-sil
# --recursive を付けずにクローンした場合
git submodule update --init --recursive
```

```sh
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
ctest --preset linux-gcc-release
```

上は Linux（GCC）の例です。要るものの確かめ方（`scripts/doctor.py`）、必要なもの、ほかのプリセット、Windows（MSYS2 UCRT64）の手順は [docs/build.md](docs/build.md) に、試験のラベルと回し方は [docs/code-map.md](docs/code-map.md) の「試験のラベル」にあります。

## 最初の ECU を動かす

次のコマンドは、エンジンの ECU（回転数を周期送信する）と表示の ECU（受けて表示する）を動かします。動かす場所は 2 つあり、ECU のコードはどちらでも同じです。

- 引数なし（既定）: 1 つのプロセスの中の仮想バスの上で動かします。SIL Kit を使わないので、registry を起動する必要はありません。ECU のロジックを作って試すときに向いています。
- `--silkit`: 同じ ECU を SIL Kit の participant として動かします（registry はプロセス内に立てます）。ほかのプロセスや道具とつなぐ前の確認に使います。すでに動いている registry につなぐときは `--registry-uri <uri>` を渡します。

```sh
B=build/linux-gcc-release
$B/examples/getting_started/vehicle_sil_getting_started
$B/examples/getting_started/vehicle_sil_getting_started --silkit
```

ECU は、`vehicle_sil::Ecu&` を受け取る普通のクラスです。どこで動かすかは、`Simulation` のコンストラクターだけで決まります。

```cpp
class Engine {
public:
    explicit Engine(vehicle_sil::Ecu& ecu) {
        auto& node = ecu.j1939("J1939_CAN").add_node({.address = 0x00});
        codec::attach_sender<EngineStatus>(node, 20ms, [this](EngineStatus& m) { m.speed.set(rpm_); });
        ecu.every(10ms, [this](vehicle_sil::SimTime) { rpm_ = rpm_ < 1500 ? rpm_ + 25 : rpm_; });   // モデルを進める
    }
private:
    int rpm_ = 0;
};   // Display も同じ形: codec::on_message<EngineStatus>(node, ...) で受ける

int main() {
    vehicle_sil::Simulation sim;                                // プロセス内の仮想バス
 // vehicle_sil::Simulation sim(vehicle_sil::SilKitOptions{});  // SIL Kit（プロセス内の registry）。変えるのはこの 1 行
    sim.add<Engine>("Engine", 10ms);                            // ステップ幅 10 ms の ECU。Engine は ECU が持つ
    auto& display = sim.add<Display>("Display", 5ms);
    sim.run_for(2s);                                            // 仮想時間で 2 秒。ステップの失敗は SimulationError
    // 戻った後は、ステップが書いた値（display の記録など）を読んでよい
}
```

- メッセージ（`EngineStatus`）は YAML から生成した型です（[docs/j1939-codec.md](docs/j1939-codec.md)）。完全なコードは [examples/getting_started](examples/getting_started) にあります。
- 動かす場所を実行時に選ぶときは、`std::optional<vehicle_sil::SilKitOptions>` を渡します。空ならプロセス内の仮想バス、値があれば SIL Kit です（`Simulation sim(silkit);`）。
- ECU は、外から読み書きする変数も宣言できます（`ecu.input("engine.accelerating", &accelerating_, {.description = "Accelerates while true"})`、`ecu.output("engine.speed", &speed_, {.unit = "km/h"})`）。同じプロセスからは `sim.ecu("Engine").agent()` の `submit()` で時刻を指定して書き、`latest()` で読みます。Control API のクライアント（Python、CLI）からも操作できます。仮想バスでも SIL Kit でも、書き込みは同じステップで反映されます。例は [examples/vehicle](examples/vehicle)、使い方は [docs/control-api.md](docs/control-api.md) にあります。
- やりたいこと別のサンプル（型付きメッセージ、独自プロトコル、故障注入、複数の ECU と LIN、生のイーサネットのフレームなど）は [examples/README.md](examples/README.md) にまとめています。
- 作り方、試験の書き方、`Peer` を直接使う場合は [docs/j1939-guide.md](docs/j1939-guide.md) にあります。
- `Peer` を別プロセスで動かす構成（ピアを手動で動かす、仮想時間で動かす、CAN バスの調停と送信時間を模擬する）は [docs/peers.md](docs/peers.md) に、LIN の使い方は [docs/lin-guide.md](docs/lin-guide.md) に、vehicle-sil を自分の CMake プロジェクトに取り込む方法は [docs/build.md](docs/build.md) の「外部プロジェクトから使う（FetchContent）」にあります。

## 既知の制限

- 仮想時間の一時停止とステップ実行は、C++ の API（`SimulationController` の時間ゲート）、`vehicle_sil_launch run` の標準入力の命令と Control API（`--control`）からできます。`vehicle_sil_sim_controller` からはできません。
- シミュレーションを始めからやり直す操作（リセット）はありません。止めてから `run` をやり直します。CAN と LIN のチャネルやデバイスは、Control API、構成ファイル、GUI からは選べません（Vector XL CAN Bridge のチャネルは、起動の引数で選びます）。
- SIL Kit で動かすとき、LIN の Slave は、応答するフレームのスロット長より短いステップ幅で動かす必要があります。LIN の dynamic response など、SIL Kit の experimental API を使っています。
- LIN の Slave が受信できるのは、Master が送るフレームだけです。ほかの Slave の応答（slave-to-slave）は、仮想 LIN バスでは渡されず、SIL Kit では購読した ID なら渡されますが、受け取るステップと時刻は決定論的ではありません。
- 既存 ECU の DLL が内部で実時間（スリープ、時刻の取得）を使っている場合、vehicle-sil はそれを仮想時間に置き換えられません。
- J1939 スタックに ETP（1785 バイトを超えるメッセージ）はありません。制限と規格との一致を確かめていない点は [docs/internals/j1939.md](docs/internals/j1939.md) の「J1939 の既知の制限」と各節にあります。
- 試験キットに LIN の故障注入と記録の道具はありません（記録は仮想 LIN バスの観測、ECU ごとの通信断は `ecu.connected` でできます）。LIN は仮想時間だけで動きます（実時間の `Peer` では `std::logic_error`）。
- `LocalBench`（`vehicle_sil::local`）と `vehicle_sil::testkit` の API は固まっていません。
- 対応するのはクラシック CAN（8 バイト以下）だけです。RTR、CAN FD、CAN XL のフレームは、受信時に捨てて件数を数えます。
- `install()` とパッケージ化（`find_package`）には未対応です。使えるのは FetchContent と `add_subdirectory` だけです。
- Windows 10 は未確認です（Windows 11 の MSYS2 UCRT64 ビルドと試験は確認済み）。
- Vector XL CAN Bridge は Vector Virtual CAN と VN5620 の物理ループバックで確認済みです。他の物理 CAN 構成や機器での動作は未確認です。
- Vector XL CAN Bridge が扱うのはクラシック CAN だけです。CAN FD には対応していません。

## 開発者向け

ドキュメントの管理ルールは [AGENTS.md](AGENTS.md) にあります。

## ライセンス

vehicle-sil は MIT License で公開しています（[LICENSE](LICENSE)）。ECU のプロジェクトのひな形 `template/` のファイルだけは MIT-0（MIT No Attribution、[template/LICENSE](template/LICENSE)）で、写して作ったプロジェクトに表示を残す必要はありません。

依存ライブラリのライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。

vehicle-sil は、Vector Informatik や SAE International とは関係のない、独立したプロジェクトです。文中の製品名と規格名は、それぞれの権利者のものです。
