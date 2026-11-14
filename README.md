# vehicle-sil

vehicle-sil は、車の ECU（電子制御ユニット）をソフトウェアで模擬し、複数の ECU をつないで PC の上で動かし、試験するための道具一式です。ECU は C++20 で書きます。ECU の間の通信と時刻の同期には [Vector SIL Kit](https://github.com/vectorgrp/sil-kit) を使います。Linux と Windows（MinGW）で動きます。

> [!NOTE]
> 最初の版（0.1.0）を公開する前です。公開 API は、互換性を保たずに変わることがあります。

## できること

| やりたいこと | 使うもの | 読む文書 |
|---|---|---|
| 自分の ECU を作る | C++ のクラス 1 つと `ecu_main()` の 1 行で、ECU が実行ファイルになります。 | [最初の ECU を作る](docs/howto/first-ecu.md) |
| ECU に通信させる | J1939（CAN）、LIN、生のイーサネットのフレームを送受信できます。 | [j1939-guide.md](docs/j1939-guide.md)、[lin-guide.md](docs/lin-guide.md)、[examples/ethernet](examples/ethernet) |
| 複数の ECU をつないで 1 つのシミュレーションとして動かす | `vehicle_sil_launch` が、YAML の構成ファイルと各 ECU の食い違いを調べ（`check`）、全部を起動して動かします（`run`）。 | [ECU をまとめて動かす](docs/howto/integrate-ecus.md) |
| 試験を書いて自動で回す | YAML のシナリオに、値を入れる、待つ、確かめる手順を書きます。 | [シナリオで試験を書く](docs/howto/scenario-tests.md) |
| 動いているシミュレーションの値を Python から読み書きする | Control API（gRPC）と、その Python のクライアントを使います。 | [信号を外から操作する](docs/howto/operate-signals.md) |
| ブラウザで値を見て触る | `vehicle-sil-gui` が、変数をスライダーやグラフで出します。 | [gui.md](docs/gui.md) |
| 実機の CAN やネットワークにつなぐ | Vector XL の CAN とのブリッジと、イーサネットのブリッジがあります。 | [bridges.md](docs/bridges.md)、[ethernet-bridge.md](docs/ethernet-bridge.md) |
| ほかの PC の ECU を参加させる | 構成ファイルに、SIL Kit の registry の場所と、相手が自分で起動する ECU を書きます。 | [複数の PC で動かす](docs/howto/multi-pc.md) |

```mermaid
flowchart LR
    a["ECU（A さんが作る）"] --- silkit(["SIL Kit"])
    b["ECU（B さんが作る）"] --- silkit
    launch["vehicle_sil_launch<br>check / run / シナリオ"] --- silkit
    bridge["ブリッジ"] --- silkit
    bridge --- real["実機の CAN、<br>外のネットワーク"]
    client["Python、ブラウザ"] -- "値を読む、書く" --> launch
```

ECU は 1 つずつ別の実行ファイルです。何人かが別々に作った ECU を、中身を知らない人が構成ファイルに並べて動かせます。全体の形は [docs/architecture.md](docs/architecture.md) に、やりたいこと別のサンプルは [examples/README.md](examples/README.md) にあります。

## 動かすとこう見える

例は 2 つの ECU です。エンジンの ECU は、スロットルの値に合わせて回転数を変え、J1939 で送ります。表示の ECU は、それを受けて表示します。次の構成ファイルが、2 つを 1 つのシミュレーションにします（[examples/ecu_main/simulation.yaml](examples/ecu_main/simulation.yaml)。コメントは省いています）。

```yaml
vehicle_sil_simulation: 1
stop_at_ms: 1000
networks:
  - name: J1939_CAN
    kind: can
participants:
  - name: Engine
    command: [engine/vehicle_sil_engine_ecu]
    step_ms: 10
  - name: Dashboard
    command: [dashboard/vehicle_sil_dashboard_ecu]
    step_ms: 5
```

`participants` の 1 つが、1 つの ECU の実行ファイルです。`vehicle_sil_launch check simulation.yaml` は、各 ECU に自分の構成を報告させて、構成ファイルと突き合わせます。何も動かしません。

```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

passed: 0 errors, 0 warnings, 0 notes
```

`vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml` は、2 つの ECU を起動し、シナリオの手順どおりに値を入れて確かめます。スロットルを 50 % にして、回転数が上がり、表示に届くのを待つシナリオです。全部通れば、終了コードは 0 です（出力の終わりの部分）。

```text
scenario scenarios/throttle_step.yaml (throttle step): passed, 6 of 6 steps passed

line  step                                                              result  at
8     set Engine.engine.throttle = 50                                   ok      0 ms
10    wait_until Engine.engine.speed ge 1700 (timeout 500 ms)           ok      330 ms
12    expect Engine.engine.speed near 1800 (tolerance 10) (for 100 ms)  ok      490 ms
15    expect Dashboard.dashboard.engine_speed ge 1700 (within 200 ms)   ok      490 ms
17    ramp Engine.engine.throttle to 0 over 200 ms                      ok      700 ms
18    expect Engine.engine.speed le 1000 (within 100 ms)                ok      700 ms

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

passed: 0 errors, 0 warnings, 0 notes
```

食い違いは、動かす前に `check` が場所と直し方を付けて挙げます。構成ファイルのネットワークの名前を `Vehicle_CAN` と書き間違えた写しを作って、確かめてみます。

```sh
sed 's/name: J1939_CAN/name: Vehicle_CAN/' simulation.yaml > mistake.yaml
vehicle_sil_launch check mistake.yaml
```

終了コードは 1 で、所見は 3 件です。次は、そのうちの 1 件と最後の行です。行の番号は、コメントを含むファイルのものです。

```text
error: mistake.yaml:11: participant 'Engine': the executable uses the network 'J1939_CAN' (j1939), which is not in networks: add it to networks with kind: can, or correct the network's name in the ECU
failed: 2 errors, 1 warning, 0 notes
```

ブラウザの GUI では、同じ例を止めるまで動かして、値を見ながら触れます。スロットルのスライダーを動かすと、回転数が変わります。

![ブラウザの GUI の「変数」の画面。スロットルのスライダー、回転数のグラフ、表示の ECU が受けた回転数が並んでいる](docs/images/gui-variables.png)

## 始める

### リポジトリから動かす

版を公開するまでは、リポジトリをビルドして、上の例を動かします。コマンドは Linux のものです。

```sh
git clone --recursive https://github.com/rahanahu/vehicle-sil.git
cd vehicle-sil
scripts/doctor.py                          # 要るものがそろっているかを 1 行ずつ出す
uv sync                                    # ビルドと GUI が使う Python のパッケージを入れる
cmake --preset linux-gcc-control           # Control API のサーバーつき（Python と GUI から値を読み書きするのに要る）
cmake --build --preset linux-gcc-control
```

「動かすとこう見える」の出力は、ビルドした例のディレクトリで出せます。

```sh
export PATH="$PWD/build/linux-gcc-control/tools/launch:$PATH"
cd build/linux-gcc-control/examples/ecu_main
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml
```

ブラウザの GUI は、リポジトリの直下に戻って、画面をビルドしてから起動します。Ctrl+C で止まります。

```sh
cd ../../../..                            # リポジトリの直下へ
npm --prefix gui/web ci
npm --prefix gui/web run build
scripts/run-gui.py                         # 足りないものがあれば、作るコマンドを表示する
```

- 要るものは、C++20 のコンパイラ、CMake 3.21 以上、Ninja、Git、[uv](https://docs.astral.sh/uv/)、画面のビルドに Node.js 24.15 以上と npm 11.12 以上です。
- `scripts/doctor.py` は、足りないものと入れ方を表示します。uv、Node.js、npm は「absent」と出ても終了コードが 0 のままなので、行を見て確かめます。
- 最初のビルドは、SIL Kit をコンパイルするので時間がかかります。PC に gRPC がなければ、gRPC も取ってきてコンパイルします（このときは CMake 3.28 以上が要ります）。
- Windows（MSYS2 UCRT64）の手順、ほかのプリセット、試験の回し方は [docs/build.md](docs/build.md) にあります。

### ひな形とビルド済みの道具一式から（版を公開した後）

版を公開した後は、リポジトリをビルドしなくて済みます。[リリースのページ](https://github.com/rahanahu/vehicle-sil/releases)に 2 つを置きます。まだ公開していません。

- ひな形: 上の例と同じ 2 つの ECU、構成ファイル、シナリオが入った、ECU のプロジェクトの出発点です（[template/](template/) と同じもの）。
- ビルド済みの道具一式: `vehicle_sil_launch`、ブラウザの GUI、Python のクライアントを、OS ごとに 1 つの圧縮ファイルにしたものです。

ひな形を展開したディレクトリで、次の順に実行します。

```sh
python3 get_tools.py             # 道具一式を取ってきて、PATH に入れるコマンドを表示する
cmake --preset linux             # Windows（MSYS2 UCRT64）: cmake --preset windows
cmake --build --preset linux     # Windows: cmake --build --preset windows
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml
vehicle-sil-gui simulation.live.yaml
```

要るものと各段の説明は、ひな形の [README](template/README.md) にあります。版ごとの変更は [CHANGELOG.md](CHANGELOG.md) にあります。

## ECU はこう書く

ECU は、`vehicle_sil::Ecu&` を受け取る普通のクラスです。上の例のうち、エンジンの ECU のクラスと `main()` をそのまま載せます（[examples/ecu_main/engine/main.cpp](examples/ecu_main/engine/main.cpp)。`#include` と名前空間の宣言は省いています）。

```cpp
class Engine
{
public:
    explicit Engine(vehicle_sil::Ecu& ecu) :
      node_(ecu.j1939("J1939_CAN").add_node(
          {.address = 0x00, .name = j1939::Name{}.set_identity_number(1).set_manufacturer_code(0x7F0)}))
    {
        ecu.input("engine.throttle", &throttle_,
                  {.unit = "%", .min = 0.0, .max = 100.0, .description = "accelerator pedal position"});
        ecu.output("engine.speed", &rpm_, {.unit = "rpm"});
        // The model: the speed moves towards 800 rpm plus 20 rpm per percent of throttle.
        ecu.every(10ms, [this](vehicle_sil::SimTime) {
            const int target = 800 + static_cast<int>(throttle_ * 20.0);
            rpm_ += rpm_ < target ? 50 : rpm_ > target ? -50 : 0;
        });
        ecu.every(100ms, [this](vehicle_sil::SimTime) {
            static_cast<void>(node_.send(ecu_main_example::kEngineSpeed, ecu_main_example::encode_speed(rpm_)));
        });
    }

private:
    j1939::Node& node_;
    double throttle_ = 20.0;
    int rpm_ = 0;
};
```

```cpp
int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Engine>(argc, argv, {.name = "Engine", .step = 10ms});
}
```

- `ecu.input()` と `ecu.output()` で宣言した変数が、シナリオ、Python、GUI から読み書きする値です。
- `ecu.j1939("J1939_CAN")` の名前は、構成ファイルの `networks` の名前と合わせます。合わなければ、上の `check` の誤りになります。
- できた実行ファイルは、どの ECU も同じ引数を持ちます。`--describe` は自分の構成を JSON で出し、`--local --run-for-ms 1000` は 1 つだけで動きます（[ecu-executable.md](docs/ecu-executable.md)）。
- 送っているメッセージの定義は、隣の [engine_speed.hpp](examples/ecu_main/engine_speed.hpp) にあります。信号の定義を YAML に書いて型を生成することもできます（[j1939-codec.md](docs/j1939-codec.md)）。
- 複数の ECU を 1 つのプロセスで動かす書き方もあります。SIL Kit なしで動くので、ECU のロジックを 1 人で試すときに向いています（[examples/getting_started](examples/getting_started)）。

## 既知の制限

- CAN は、クラシック CAN（8 バイト以下）だけです。RTR、CAN FD、CAN XL のフレームは、受信時に捨てて件数を数えます。
- J1939 のスタックに ETP（1785 バイトを超えるメッセージ）はありません。ほかの制限と、規格との一致を確かめていない点は [docs/internals/j1939.md](docs/internals/j1939.md) の「J1939 の既知の制限」と各節にあります。
- LIN は仮想時間だけで動きます（実時間の `Peer` では `std::logic_error`）。LIN の dynamic response など、SIL Kit の experimental API を使っています。
- SIL Kit で動かす LIN の Slave は、応答するフレームのスロット長より短いステップ幅で動かす必要があります。
- LIN の Slave が受信できるのは、Master が送るフレームだけです。ほかの Slave の応答は、仮想 LIN バスでは渡されません。SIL Kit では購読した ID なら渡されますが、受け取るステップと時刻は決定論的ではありません。
- イーサネットは生のフレームだけで、TCP/IP のスタックはありません。
- シミュレーションを始めからやり直す操作（リセット）はありません。止めてから `run` をやり直します。
- 仮想時間の一時停止とステップ実行は、C++ の API（`SimulationController` の時間ゲート）、`vehicle_sil_launch run` の標準入力の命令、Control API（`--control`）からできます。`vehicle_sil_sim_controller` からはできません。
- CAN と LIN のチャネルやデバイスは、Control API、構成ファイル、GUI からは選べません（Vector XL CAN Bridge のチャネルは、起動の引数で選びます）。
- 既存 ECU の DLL が内部で実時間（スリープ、時刻の取得）を使っている場合、vehicle-sil はそれを仮想時間に置き換えられません。
- 試験キットに LIN の故障注入と記録の道具はありません（記録は仮想 LIN バスの観測、ECU ごとの通信断は `ecu.connected` でできます）。
- `LocalBench`（`vehicle_sil::local`）と `vehicle_sil::testkit` の API は固まっていません。
- `install()` とパッケージ化（`find_package`）には未対応です。取り込み方は FetchContent と `add_subdirectory` だけです。
- Windows 10 は未確認です（Windows 11 の MSYS2 UCRT64 で確認しています）。
- Vector XL CAN Bridge が扱うのは、クラシック CAN だけです。Vector Virtual CAN と VN5620 の物理ループバックで確認していて、ほかの機器は未確認です。

## 開発に参加する

vehicle-sil 自体を直すときは、先に [AGENTS.md](AGENTS.md) を読みます。文書の管理、コミットメッセージ、検証のスクリプトの決まりがあります。ソースのどこに何があるかは [docs/code-map.md](docs/code-map.md) にあります。

## ライセンス

vehicle-sil は MIT License で公開しています（[LICENSE](LICENSE)）。ひな形 `template/` のファイルだけは MIT-0（[template/LICENSE](template/LICENSE)）で、写して作ったプロジェクトに表示を残す必要はありません。依存ライブラリのライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) にあります。

vehicle-sil は、Vector Informatik や SAE International とは関係のない、独立したプロジェクトです。文中の製品名と規格名は、それぞれの権利者のものです。
