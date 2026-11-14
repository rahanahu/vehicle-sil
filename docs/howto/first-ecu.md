# 最初の ECU を作る

ECU を書く人向けの手引きです。前に README の「[最初の ECU を動かす](../../README.md#最初の-ecu-を動かす)」を読んでおくと、ECU のクラスの形がわかります。次は [j1939-guide.md](../j1939-guide.md)（J1939 の送受信と部品）と、ほかの人の ECU とまとめるときは [ECU をまとめて動かす](integrate-ecus.md) です。

ひな形を写して ECU のクラスを書き、`ecu_main` で実行ファイルにし、構成ファイルで動かして、Python から変数を読み書きするまでの順番です。手引きに書くのは手順とつまずく所だけです。引数、形式、規則の正本は、各段で示すリファレンスです。

例には、ECU のプロジェクトのひな形 [template/](../../template) のエンジンの ECU（`engine_ecu`）を使います。CTest がひな形をビルドして、照合と実行をしています。自分の ECU は、ひな形を写して書き換えます。ひな形のエンジンと表示の ECU は、[examples/ecu_main](../../examples/ecu_main) の 2 つと同じ ECU です（ほかの手引きは、そちらを使います）。

```mermaid
flowchart LR
    copy["ひな形を写す"] --> write["ECU のクラス"]
    write --> main["ecu_main"]
    main --> describe["--describe"]
    describe --> config["構成ファイル"]
    config --> check["check"]
    check --> run["run"]
    run --> python["Python"]
```

## ひな形を写して、道具を取ってくる

README の「[取得とビルド](../../README.md#取得とビルド)」の「ECU を作って動かす」の手順です。

```sh
# リリースのページの vehicle-sil-template-<版>.zip を展開したディレクトリで
python3 get_tools.py                 # Windows（UCRT64）: python get_tools.py。ビルド済みの道具を tools/ に取ってくる。表示されたコマンドで bin を PATH に入れる
cmake --preset linux                 # Windows（MSYS2 UCRT64 シェル）: cmake --preset windows
cmake --build --preset linux         # Windows: cmake --build --preset windows
```

- `get_tools.py` は、ひな形に書いてある版（`VERSION`）の道具を、ひな形に書いてある SHA-256 で確かめてから展開します。取れないときの手順と、リポジトリの `template/` を写したときの `--sha256` は、ひな形の [README](../../template/README.md) にあります。
- 最初の構成で vehicle-sil と SIL Kit を取ってきて、最初のビルドで SIL Kit をコンパイルします（数分、最初の 1 回だけ）。実行ファイルは `bin/` にできます。
- 以降のコマンドは、ひな形のディレクトリ（`simulation.yaml` のある所）で実行します。

## ECU のクラスを書く

ECU は `vehicle_sil::Ecu&` を受け取るクラスです。コンストラクターで、つながるネットワーク、J1939 のノード、外から読み書きする変数、ステップごとの処理を宣言します。

[template/engine/main.cpp](../../template/engine/main.cpp) の 22〜47 行です。

```cpp
class Engine
{
public:
    explicit Engine(vehicle_sil::Ecu& ecu) :
      // The network's name is the one in simulation.yaml. 0x7F0 is a placeholder Manufacturer Code.
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
            static_cast<void>(node_.send(my_ecus::kEngineSpeed, my_ecus::encode_speed(rpm_)));
        });
    }

private:
    j1939::Node& node_;
    double throttle_ = 20.0;
    int rpm_ = 0;
};
```

- `ecu.j1939("J1939_CAN")` のネットワークの名前は、後で構成ファイルの `networks` に同じ名前で書きます。ほかの ECU と同じバスに載せるときは、名前をまとめる人と合わせます。
- J1939 のノードのアドレスと NAME は、同じネットワークのほかのノードと重ならないようにします（重なりは、後の照合が見つけます）。Manufacturer Code の 0x7F0 は例の仮の値です。
- `input()` は外から書く変数、`output()` は外から読む変数です。宣言の形、型、反映の時刻は [control-api.md](../control-api.md) の「ECU の変数」にあります。
- ステップごとの処理（`every()`）と変数の読み書きは、同じステップのスレッドで動きます。モデルにロックは要りません。
- 2 つの ECU が取り決めるメッセージ（PGN とデータの形）は、共有のヘッダー [template/engine_speed.hpp](../../template/engine_speed.hpp) にあります。信号を YAML で定義するときは [j1939-codec.md](../j1939-codec.md) です。

## `ecu_main` で実行ファイルにする

`main()` は `ecu_main()` の 1 行です（同じファイルの 51〜54 行）。

```cpp
int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Engine>(argc, argv, {.name = "Engine", .step = 10ms});
}
```

`name` は SIL Kit の participant 名、`step` はステップ幅です。これで、どの ECU とも同じ引数（`--describe`、`--registry-uri`、`--local`）を持つ実行ファイルになります。`ecu_main()` は `main()` の最後で呼びます（理由は [ecu-executable.md](../ecu-executable.md) の「引数」の後の段落）。

CMake は [template/CMakeLists.txt](../../template/CMakeLists.txt) の 32〜36 行です。

```cmake
add_executable(engine_ecu engine/main.cpp)
target_include_directories(engine_ecu PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(engine_ecu PRIVATE vehicle_sil::simulation)
# Windows only: copies SilKit.dll next to the executable.
vehicle_sil_copy_runtime_dlls(engine_ecu)
```

- リンクするのは `vehicle_sil::simulation` です。J1939 の部品や型付きメッセージを使うときは、そのターゲットも足します（[architecture.md](../architecture.md) の「構成」）。
- `vehicle_sil_copy_runtime_dlls()` は、Windows で `SilKit.dll` を実行ファイルの隣に写します。
- ECU を足すときは、この 5 行を写して名前を変え、構成ファイルにも足します。

vehicle-sil は、同じファイルの 15〜23 行で FetchContent で取り込んでいます。`GIT_TAG` が vehicle-sil の版で、道具の版（`get_tools.py` の `VERSION`）とそろえます。

```cmake
  include(FetchContent)
  FetchContent_Declare(
    vehicle_sil
    GIT_REPOSITORY https://github.com/rahanahu/vehicle-sil.git
    # The vehicle-sil release your ECUs are built with. Keep it the same as the version of the
    # tools (vehicle_sil_launch version): change both together.
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(vehicle_sil)
```

## 自己申告を見る（`--describe`）、単体で動かす（`--local`）

```sh
bin/engine_ecu --describe
bin/engine_ecu --local --run-for-ms 1000
```

- `--describe` は、何も動かさず、どこにも接続せずに、ECU の構成を JSON で標準出力に書きます。このエンジンの出力の全文は、[ecu-executable.md](../ecu-executable.md) の「JSON の形式」の例と同じです（例は `examples/ecu_main` の同じ ECU の出力で、試験が照合しています）。確かめるのは、`name`、`step_ns`、`networks`（ネットワークの名前、J1939 のアドレスと NAME）、`variables`（変数の名前、型、範囲）、`control` です。
- まとめる人は、この自己申告で照合します。ネットワーク、アドレス、変数を変えたら、`--describe` で確かめてから渡します。
- `--local --run-for-ms 1000` は、この ECU だけを、プロセス内の仮想バスの上で、仮想時間で 1000 ms 動かします。相手がいないので何も受信しません。ステップで例外が出たら終了コード 1 です。
- 引数と終了コードの表は [ecu-executable.md](../ecu-executable.md) の「引数」にあります（引数は `--help` でも出ます）。

つまずく所:

- 引数なしで起動すると、何も始めずに、使い方を標準エラーに書いて終了コード 4 です。
- 変数の名前は `peer.` で始められません。宣言のときに例外になるので、`--describe` が終了コード 3 で失敗し、標準エラーに `error: <ECU 名>: <理由>` を書きます。
- `--describe` の間に ECU が標準出力に書いたもの（`printf` など）は、標準エラーに回ります。JSON は壊れません。

## 構成ファイルに入れる

シミュレーションに入れる ECU の実行ファイルとネットワークを、構成ファイルに書きます。ひな形の [simulation.yaml](../../template/simulation.yaml) です（エンジンと表示の 2 つの ECU）。

<!-- template/simulation.yaml -->
```yaml
# yaml-language-server: $schema=schema/simulation.schema.json
# The two ECUs of this project in one simulation. Build them first (README.md), then, here:
#   vehicle_sil_launch check simulation.yaml
#   vehicle_sil_launch run simulation.yaml [--scenario scenarios/throttle_step.yaml]
# The paths in command are relative to this file's directory.
vehicle_sil_simulation: 1
stop_at_ms: 1000
networks:
  - name: J1939_CAN
    kind: can
participants:
  - name: Engine
    command: [bin/engine_ecu]
    step_ms: 10
  - name: Dashboard
    command: [bin/dashboard_ecu]
    step_ms: 5
```

- `command` の相対パスは、構成ファイルのあるディレクトリから数えます（コマンドを実行したディレクトリからではありません）。
- ECU が使うネットワークは、すべて `networks` に書きます。
- `step_ms` を書くと、ECU の申告と照合します。
- `command` には `ecu_main` の引数（`--name`、`--registry-uri` など）を書きません。ランチャーが足します。
- キーの一覧は [simulation-config.md](../simulation-config.md) の「構成ファイル」にあります。

## 照合する（`check`）

```sh
vehicle_sil_launch check simulation.yaml
```

照合は、各 ECU を `--describe` で起動し、構成ファイルと自己申告を突き合わせます。シミュレーションは始めません。

<!-- vehicle_sil_launch check simulation.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

passed: 0 errors, 0 warnings, 0 notes
```

- 最後の行が `passed:` なら終了コード 0 です。食い違いがあれば、`error:`、`warning:`、`note:` の行に、ファイルの行番号、何が違うか、どう直すかと、行の終わりに所見の名前（`[single-participant]` など）が出ます。読み方は [ECU をまとめて動かす](integrate-ecus.md) の「照合で食い違いを見つける」にあります。
- 自分の ECU だけで試すときは、相手を外します（`check simulation.yaml --disable Dashboard`）。ネットワークに 1 つしかいないので、警告 `single-participant` が出ます。

## 動かす（`run`）

```sh
vehicle_sil_launch run simulation.yaml
```

`run` は同じ照合をしてから、registry、controller、各 ECU を起動し、終わりまで見て、結果を標準出力に書きます。

<!-- vehicle_sil_launch run simulation.yaml -->
```text
simulation.yaml: 2 participants, 1 network

participant  step   networks
Engine       10 ms  J1939_CAN (j1939 0x00)
Dashboard    5 ms   J1939_CAN (j1939 0x28)

network    kind  participants
J1939_CAN  can   Engine, Dashboard

run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0

passed: 0 errors, 0 warnings, 0 notes
```

- 標準エラーには、ランチャーの経過（`[vehicle_sil_launch] ` で始まる行）と、ECU が書いた行（`[<participant 名>] ` を付けたもの）が流れます。表示の ECU が 500 ms ごとに書く行は `[Dashboard] ` 付きで出ます。
- `stop_at_ms` の 1000 ms で止まり、各 ECU は自分で終わります。途中で止めるときは Ctrl+C です（終了コード 130）。
- 出力の形と終了コードは [simulation-run.md](../simulation-run.md) の「シミュレーションの実行」にあります。

## デバッガーから動かす

自分の ECU をデバッガーで止めながら動かすときは、その ECU だけをランチャーに起動させず、自分で起動します。構成ファイルは書き換えずに、`--manual` に participant 名を付けます。

```sh
vehicle_sil_launch run simulation.yaml --manual Engine
```

ランチャーはほかの ECU を起動し、Engine を起動するコマンドを標準エラーに書いて、Engine が参加するのを待ちます（`<dir>` はひな形のディレクトリです。形は [simulation-config.md](../simulation-config.md) の「participant を自分で起動する」の例と同じで、実行ファイルのパスだけが違います）。

```text
[vehicle_sil_launch] not starting Engine: start it yourself (manual: true, or --manual); the simulation waits for it to join. In the directory <dir>, run:
[vehicle_sil_launch]   <dir>/bin/engine_ecu --name Engine --registry-uri silkit://127.0.0.1:8500 --start-timeout-ms 1000000000000
```

- 2 行目の `[vehicle_sil_launch]` の後が、Engine を起動するコマンドです。デバッガーには、実行ファイル、その後の引数、作業ディレクトリ（1 行目の `In the directory` の後）を設定して起動します。端末で試すときは、そのディレクトリでコマンドを貼って実行します。
- 構成ファイルに `registry_uri` がなければ、registry のポートは 8500 に決まります。コマンドが毎回同じなので、デバッガーの設定は 1 度作れば使い回せます。8500 を別のプログラムが使っているときは、`registry_uri` に別のポートを書きます。
- Engine が参加するまで、時間の制限なく待ちます。参加した後にブレークポイントで止めている間は、シミュレーション全体の仮想時間が止まります。
- 参加した後、シミュレーションが始まる前に Engine を終わらせて起動し直すと、ランチャーは開始の失敗で終わります。やり直すときは、ランチャーも起動し直します。
- 端末に貼るときは、Linux では sh（bash、zsh）、Windows ではコマンドプロンプトに貼る形です。PowerShell では、`"` で囲んだ実行ファイルの前に `&` を足します。
- Engine の出力は、起動した側（デバッガー）に出ます。ランチャーの標準エラーには出ません。
- シミュレーションが終われば、Engine は自分で終わります。ランチャーは Engine のプロセスを止めません。結果の表では、Engine の `ended` が `started by you (manual)` になります。
- 待ち方と止め方の規則は、[simulation-config.md](../simulation-config.md) の「participant を自分で起動する」にあります。

## Python から変数を読み書きする

ECU の側で足す作業はありません。`ecu_main` で作った ECU は、変数を宣言していれば、`run` のときにランチャーとつながります。クライアントは、ランチャーの Control Server（`vehicle_sil_launch run --control`）に gRPC でつなぎます。

準備は、ビルド済みの道具に入っています。

- 道具の `vehicle_sil_launch` は Control Server を持っています（自分でビルドするときは `-DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON`。[control-api.md](../control-api.md) の「ビルド」）。
- 道具の `python/` に、Python のクライアント（`vehicle_sil_control`）と生成済みのスタブがあります。要るパッケージ（grpcio、protobuf）は `python/requirements.txt` に版とハッシュを固定してあり、uv があれば `uv run --with-requirements` が入れます。uv がなければ、Python 3.11 以上の仮想環境に `pip install --require-hashes -r` で入れます。

例 [examples/launch_control/write_before_start.py](../../examples/launch_control/write_before_start.py) を、vehicle-sil のリポジトリからひな形のディレクトリに写して動かします（Linux の例。`T` は `get_tools.py` が展開したディレクトリ）。

```sh
T=tools/vehicle-sil-tools-<版>-linux-x86_64
PYTHONPATH=$T/python uv run --no-project --no-build --python '>=3.11,<3.15' --with-requirements $T/python/requirements.txt \
    python write_before_start.py --launch $T/bin/vehicle_sil_launch simulation.yaml
```

例は、上の構成ファイルの 2 つの ECU を、最初のステップの前で止めて起動し、エンジンのスロットルを書いて 500 ms 進め、回転数を読み返します。期待どおりなら終了コード 0 です。中心は 40〜63 行です。

```python
    with Launch(args.launch, args.config, start_paused=True) as launch:
        print(f"control server at {launch.address}")
        with Control(launch.address) as c:
            stands = c.sim.wait_paused()
            expect(stands.now == 0 and not stands.running, "it stands still at 0 before the first step")

            # Before the first step: the write takes effect in the engine's step at 0. Wait until the
            # engine has queued it before letting the simulation go on.
            seq = c.write({"Engine": {"engine.throttle": THROTTLE}})
            queued = c.wait(seq, until="queued")
            expect(queued.target("Engine").status == "queued", f"Engine queued the throttle ({queued.target('Engine').status})")

            c.sim.step(ms(500))
            stands = c.sim.wait_paused()
            expect(stands.now == ms(500), f"it stands still at 500 ms ({stands.now / 1e6:g} ms)")

            done = c.wait(seq)
            expect(done.target("Engine").status == "applied" and done.target("Engine").applied_at == 0,
                   f"the throttle took effect in the step at 0 ({done.target('Engine').status} at "
                   f"{done.target('Engine').applied_at})")
            values = c.values("Engine").values
            expect(values["engine.throttle"] == THROTTLE, f"engine.throttle reads {values['engine.throttle']}")
            expect(values["engine.speed"] == EXPECTED_RPM, f"engine.speed reads {values['engine.speed']} rpm "
                                                           f"(expected {EXPECTED_RPM})")
```

- `Launch(..., start_paused=True)` は、`vehicle_sil_launch run --events --stdin-commands --control 0 --start-paused` を子プロセスで起動します。全員がそろうと、どの ECU も時刻 0 のステップの前で止まります。
- 止まっている間に書いた値は、時刻 0 のステップで反映されます。`c.wait(seq, until="queued")` で ECU が受け取ったことを確かめてから進めます。
- 書く先は participant 名（`"Engine"`）と、`--describe` の `variables[].name`（`"engine.throttle"`）です。
- `c.sim.step(ms(500))` で 500 ms 進めて止め、`c.values("Engine")` で読みます。スロットル 50 % で、回転数は 1800 rpm です。

つまずく所:

- 範囲の外の値（`engine.throttle` は 0〜100）は、例外ではなく、操作の結果が `invalid` になります。
- 動いている途中で一時停止してから書くときは、`at` を止まっている時刻より後にします。理由は [control-api.md](../control-api.md) の「Python」です。
- 時刻と複数の ECU の値を同じ瞬間のものとして読むときは、`c.snapshot()` を使います（[control-api.md](../control-api.md) の「時刻と値をまとめて読む」）。
- Python を書かずに試すときは、CLI（`python -m vehicle_sil_control`）も使えます（[control-api.md](../control-api.md) の「CLI」）。

## 次に読むもの

- J1939 で送る、受ける、Request に答える、部品を使う: [j1939-guide.md](../j1939-guide.md)
- 信号を YAML で定義する: [j1939-codec.md](../j1939-codec.md)
- `ecu_main` の引数、終了コード、止まるよう求められたとき: [ecu-executable.md](../ecu-executable.md)
- 変数の宣言とクライアントの API: [control-api.md](../control-api.md)
- Python から変数を操作する、時間を止めて進める: [信号を外から操作する](operate-signals.md)
- 決まった手順で値を入れて確かめる: [シナリオで試験を書く](scenario-tests.md)
- ほかの人の ECU とまとめる: [ECU をまとめて動かす](integrate-ecus.md)
- やりたいこと別の例: [examples/README.md](../../examples/README.md)
