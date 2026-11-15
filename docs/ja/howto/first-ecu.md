# 最初の ECU を作る

[English](../../howto/first-ecu.md)

テンプレートのプロジェクトに自分の ECU を 1 つ追加し、ほかの ECU と一緒にシミュレーションで実行する手順です。最後に、その ECU の変数を Python から読み書きします。追加するのは、水温が目標の温度に近づいていくだけの小さな ECU（`Cooler`）です。CAN での通信は、手順の後の「J1939 で通信させる」で追加します。

手順のとおりに書いたファイルの完成形は、[examples/first_ecu](../../../examples/first_ecu) にあります。

## 前提

| 必要なもの | 条件 |
|---|---|
| OS | Linux か、Windows の MSYS2 UCRT64 シェルで作業します。 |
| コンパイラ、CMake、Ninja、Git | バージョンは、テンプレートの [README](../../../template/README.ja.md) の「前提」の表にあります。 |
| Python | `get_tools.py` の実行に、3.9 以上を使います。 |
| [uv](https://docs.astral.sh/uv/) | 手順 7 で、Python のクライアントを実行するのに使います。 |

コマンドは Linux のものです。Windows で変わるコマンドは、手順 1 の表にあります。

## 全体の流れ

| 手順 | すること | できるもの |
|---|---|---|
| 1 | テンプレートをビルドして、そのまま動かす | 2 つの ECU の実行ファイルと、`check` と `run` の出力 |
| 2 | ECU のクラスと `main()` を書く | `cooler/main.cpp` |
| 3 | CMake に追加してビルドする | `bin/cooler_ecu` |
| 4 | ECU を単体で確認する | ECU の構成を表す JSON |
| 5 | 設定ファイルに追加して照合する | 3 つの ECU が並んだ `simulation.yaml` |
| 6 | シミュレーションを実行する | 3 つの ECU が終了コード 0 で終わったことを示す表 |
| 7 | Python から変数を読み書きする | 書き込んだ目標の温度と、読み出した水温 |

手順 1〜7 は順に進めます。この文書の「ステップ」は、シミュレーションが仮想時間を 1 回進める単位のことで、手順の番号ではありません。その後の「J1939 で通信させる」と「デバッガーから動かす」は、必要になったときに読みます。

## 手順 1: テンプレートをビルドして動かす

テンプレートに入っている 2 つの ECU（エンジンと表示）をそのまま動かして、ビルドと実行の環境がそろっていることを確認します。

[リリースのページ](https://github.com/rahanahu/vehicle-sil/releases)からテンプレートのアーカイブ（`vehicle-sil-template-<version>.zip`）をダウンロードして展開します。zip の中の `vehicle-sil-template-<version>` のディレクトリを、自分のプロジェクトの名前に変えます。そのディレクトリで次のコマンドを実行します。これ以降のコマンドも、すべて同じディレクトリ（`simulation.yaml` のある所）で実行します。

```sh
python3 get_tools.py             # ビルド済みのツールを tools/ にダウンロードし、最後に export PATH=... の行を表示する
export PATH="$PWD/tools/vehicle-sil-tools-<version>-linux-x86_64/bin:$PATH"   # 表示された行。シェルを開くたびに実行する
cmake --preset linux             # vehicle-sil と SIL Kit をダウンロードする
cmake --build --preset linux     # 最初の 1 回だけ、SIL Kit のコンパイルに数分かかる
vehicle_sil_launch check simulation.yaml
vehicle_sil_launch run simulation.yaml
```

Windows では、次の 4 つが変わります。

| すること | Linux | Windows（MSYS2 UCRT64 シェル） |
|---|---|---|
| ツールのダウンロード | `python3 get_tools.py` | `python get_tools.py` |
| configure | `cmake --preset linux` | `cmake --preset windows` |
| ビルド | `cmake --build --preset linux` | `cmake --build --preset windows` |
| ツールのディレクトリ | `tools/vehicle-sil-tools-<version>-linux-x86_64` | `tools/vehicle-sil-tools-<version>-windows-x86_64` |

### 確認

`check` は、設定ファイルと各 ECU を照合するだけで、シミュレーションは実行しません。最後の行が `passed:` なら成功で、終了コードは 0 です。

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

`run` は、同じ照合をしてから、SIL Kit の registry と 2 つの ECU を起動し、仮想時間の 1000 ms まで実行します。標準出力に次の結果を表示します。`ended` の列が、2 つとも `exit code 0` になっていることを確認します。

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

標準エラーには、実行の経過が表示されます。その中の `[Dashboard]` で始まる行で、表示の ECU がエンジンの回転数を受信していることがわかります。

```text
[Dashboard] t=0 ms engine speed 0 rpm (0 messages)
[vehicle_sil_launch] simulation running
[Dashboard] t=500 ms engine speed 1200 rpm (5 messages)
[Dashboard] t=1000 ms engine speed 1200 rpm (10 messages)
```

### 実行した設定ファイル

`check` と `run` に渡した `simulation.yaml` は、シミュレーションに参加する ECU の実行ファイルとネットワークを並べた設定ファイルです。手順 5 で、ここに自分の ECU を追加します。

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

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `get_tools.py` が `may not be published yet` と表示して止まる | そのバージョンが、まだリリースされていません。 | README の「[リポジトリからビルドする](../../../README.ja.md#リポジトリからビルドする)」でビルドした `vehicle_sil_launch` を使います。 |
| ネットワークからダウンロードできない | PC からリリースのページに接続できません。 | ブラウザで `vehicle-sil-tools-<version>-linux-x86_64.tar.gz`（Windows は `-windows-x86_64.zip`）をダウンロードし、`python3 get_tools.py --archive <ダウンロードしたファイル>` を実行します（SHA-256 の確認は同じです）。 |
| `vehicle_sil_launch` が見つからない | 新しいシェルで、`PATH` への追加を実行していません。 | `python3 get_tools.py` をもう一度実行し、表示された `export` の行を実行します。 |
| `check` が、末尾に `[executable]` の付いたエラーを表示する | ECU の実行ファイルが `bin/` にありません。 | `cmake --build --preset linux` を実行します。 |

`get_tools.py` のほかの使い方は、テンプレートの [README](../../../template/README.ja.md) にあります。

## 手順 2: ECU のクラスと `main()` を書く

プロジェクトのディレクトリ（`simulation.yaml` のある所）にディレクトリ `cooler` を作り、その中の `main.cpp` に次の内容を書きます。これがファイルの全文です。

<!-- examples/first_ecu/cooler/main.cpp -->
```cpp
// The cooler ECU: the coolant temperature moves towards a target that is written from outside.
#include "vehicle_sil/simulation/ecu_main.hpp"

#include <chrono>

using namespace std::chrono_literals;

namespace {

class Cooler
{
public:
    explicit Cooler(vehicle_sil::Ecu& ecu)
    {
        ecu.input("cooler.target", &target_,
                  {.unit = "degC", .min = 0.0, .max = 120.0, .description = "target coolant temperature"});
        ecu.output("cooler.temperature", &temperature_, {.unit = "degC"});
        // The model: every 100 ms the temperature moves 0.5 degC towards the target.
        ecu.every(100ms, [this](vehicle_sil::SimTime) {
            temperature_ += temperature_ < target_ ? 0.5 : temperature_ > target_ ? -0.5 : 0.0;
        });
    }

private:
    double target_ = 90.0;
    double temperature_ = 20.0;
};

} // namespace

int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Cooler>(argc, argv, {.name = "Cooler", .step = 100ms});
}
```

ECU は、`vehicle_sil::Ecu&` を受け取る普通のクラスです。コンストラクターで、変数と周期的な処理を宣言します。

| コード | 意味 |
|---|---|
| `ecu.input("cooler.target", &target_, {...})` | シナリオ、Python、GUI から書き込める変数を宣言し、書き込まれた値を `target_` に入れます。 |
| `ecu.output("cooler.temperature", &temperature_, {...})` | シナリオ、Python、GUI から読み出せる変数を宣言し、`temperature_` の値を出力します。 |
| `{.unit = ..., .min = ..., .max = ..., .description = ...}` | 単位、範囲、説明で、どれも省略できます。 |
| `ecu.every(100ms, ...)` | 仮想時間の 100 ms ごとに実行する処理を登録します。 |
| `ecu_main<Cooler>(argc, argv, {...})` | このクラスを、どの ECU とも同じ引数（`--describe`、`--local` など）を持つ実行ファイルにします。 |
| `.name = "Cooler"` | ECU の名前で、`--describe` の `name` になります。 |
| `.step = 100ms` | ステップ幅（ECU が 1 回に進める仮想時間）です。 |

書くときのルールは次のとおりです。

- 変数の名前は、`ecu.` と `peer.` で始められません。vehicle-sil が自動で追加する変数の名前に使うためです。
- 変数の読み書きと `every()` の処理は、同じスレッドで実行されます。モデルにロックは必要ありません。
- `ecu_main()` は、`main()` の最後で呼びます。`--describe` のときに標準出力を標準エラーに切り替え、戻った後も元に戻さないためです。
- この ECU のように、ネットワークを宣言せず、変数だけを持つ ECU も作れます。

変数に使える型と宣言の種類は、[control-api.md](../control-api.md) の「ECU の変数」にあります。書いたコードは、次の手順のビルドで確認します。

## 手順 3: CMake に追加してビルドする

`CMakeLists.txt` の末尾に、次の 4 行を追加します。エンジンの ECU の 4 行（[template/CMakeLists.txt](../../../template/CMakeLists.txt) の 32〜36 行からコメントを除いたもの）の、名前とソースファイルを変えたものです。

<!-- examples/first_ecu/CMakeLists.txt -->
```cmake
add_executable(cooler_ecu cooler/main.cpp)
target_include_directories(cooler_ecu PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(cooler_ecu PRIVATE vehicle_sil::simulation)
vehicle_sil_copy_runtime_dlls(cooler_ecu)
```

| 行 | 意味 |
|---|---|
| `add_executable` | 実行ファイルの名前（`cooler_ecu`）とソースファイルを指定します。 |
| `target_include_directories` | プロジェクトのルートにあるヘッダー（`engine_speed.hpp` など）を `#include` できるようにします。 |
| `target_link_libraries` | `ecu_main()` と、ECU が使うライブラリ（変数、CAN、LIN、J1939 のスタック）をリンクします。 |
| `vehicle_sil_copy_runtime_dlls` | Windows で、`SilKit.dll` を実行ファイルの隣にコピーします。 |

ビルドします。`CMakeLists.txt` を変更したので、CMake が configure を自動でやり直します。

```sh
cmake --build --preset linux
ls bin
```

### 確認

`bin/` に `cooler_ecu` が増えています。

```text
cooler_ecu  dashboard_ecu  engine_ecu
```

## 手順 4: ECU を単体で確認する

シミュレーションに追加する前に、実行ファイルだけで、宣言した内容と、例外なく動くことを確認します。

```sh
bin/cooler_ecu --describe
bin/cooler_ecu --local --run-for-ms 1000; echo "exit code $?"
```

### 確認

`--describe` は、何も実行せず、どこにも接続せずに、ECU の構成を JSON で標準出力に表示します。次は、その出力から、この手順で見るキーだけを抜き出したものです（変数は 1 行にまとめています）。

<!-- cooler_ecu --describe: an excerpt -->
```json
{
  "name": "Cooler",
  "step_ns": 100000000,
  "networks": [],
  "variables": [
    { "name": "ecu.connected", "type": "bool", "causality": "input", "builtin": true },
    { "name": "cooler.target", "type": "real", "causality": "input", "unit": "degC", "min": 0, "max": 120, "builtin": false },
    { "name": "cooler.temperature", "type": "real", "causality": "output", "unit": "degC", "min": null, "max": null, "builtin": false }
  ]
}
```

| キー | この ECU の値 | 一致するはずのもの |
|---|---|---|
| `name` | `"Cooler"` | `ecu_main()` に渡した `.name` です。 |
| `step_ns` | `100000000` | `.step` の 100 ms をナノ秒で表した値です。 |
| `networks` | `[]` | 宣言したネットワークで、この ECU にはありません。 |
| `variables` | `ecu.connected`、`cooler.target`、`cooler.temperature` | 宣言した 2 つの変数と、どの ECU にも自動で付く `ecu.connected`（`builtin` が `true`）です。 |

Python やシナリオから変数を指定するときの名前は、この `variables` の `name` です。ほかの人が作った ECU の変数の名前も、同じ `--describe` で調べられます。すべてのキーと出力の全体の形式は、[ecu-executable.md](../ecu-executable.md) の「JSON の形式」にあります。

`--local --run-for-ms 1000` は、この ECU だけを仮想時間で 1000 ms 動かします。成功したときは何も表示しないので、終了コードで確認します。

```text
exit code 0
```

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| 使い方を表示して、終了コード 4 で終わる | 引数がないか、引数にエラーがあります。 | `--describe`、`--local --run-for-ms <ms>` のどちらかを付けます。 |
| `error: Cooler: ...` を表示して、終了コード 3 で終わる | コンストラクターが例外を投げました（変数の名前の重複や、`peer.` で始まる名前など）。 | `error:` の行の理由に従って、コードを直します。 |
| `--local` が終了コード 1 で終わる | `every()` の処理が例外を投げました。 | `error:` の行の理由に従って、コードを直します。 |

引数と終了コードの一覧は、[ecu-executable.md](../ecu-executable.md) の「引数」にあります。

## 手順 5: 設定ファイルに追加して照合する

`simulation.yaml` の `participants` の末尾に、次の 3 行を追加します。

<!-- examples/first_ecu/simulation.yaml: the lines added -->
```yaml
  - name: Cooler
    command: [bin/cooler_ecu]
    step_ms: 100
```

| キー | 書くもの |
|---|---|
| `name` | シミュレーションの中での ECU の名前（participant 名）で、Python やシナリオからはこの名前で指定します。 |
| `command` | 実行ファイルのパスで、相対パスは設定ファイルのあるディレクトリを基準にします。 |
| `step_ms` | ステップ幅で、書くと ECU の `.step` と照合し、省略もできます。 |

`command` に `--name` などの引数は書きません。`vehicle_sil_launch` が、`name` の値を `--name` で ECU に渡します。`Cooler` はネットワークを使わないので、`networks` には何も追加しません。キーの一覧は [simulation-config.md](../simulation-config.md) にあります。

```sh
vehicle_sil_launch check simulation.yaml
```

### 確認

表に `Cooler` の行が増え、最後の行が `passed:` になります。`networks` の列の `-` は、ネットワークを使わないことを表します。

<!-- first ECU: vehicle_sil_launch check simulation.yaml -->
```text
simulation.yaml: 3 participants, 1 network

participant  step    networks
Engine       10 ms   J1939_CAN (j1939 0x00)
Dashboard    5 ms    J1939_CAN (j1939 0x28)
Cooler       100 ms  -

network    kind  participants
J1939_CAN  can   Engine, Dashboard

passed: 0 errors, 0 warnings, 0 notes
```

### うまくいかないとき

設定ファイルと ECU の不整合は、`error:`、`warning:`、`note:` で始まる行で報告されます。1 行に、ファイルの行番号、不整合の内容、直し方、指摘のコードが入っています。`step_ms` を `10` と書き間違えたときの出力は、次のとおりです。

<!-- first ECU: vehicle_sil_launch check simulation.yaml with step_ms: 10, the end -->
```text
error: simulation.yaml:20: participant 'Cooler': step_ms is 10, but the executable's step is 100 ms: write step_ms: 100, or remove step_ms [step-mismatch]

failed: 1 error, 0 warnings, 0 notes
```

| 行の末尾のコード | 原因 | 直し方 |
|---|---|---|
| `[step-mismatch]` | `step_ms` が、ECU の `.step` と違います。 | `step_ms` を ECU に合わせるか、削除します。 |
| `[executable]` | `command` のパスに実行ファイルがありません。 | ビルドするか、パスを直します。 |
| `[single-participant]` | ネットワークに ECU が 1 つしかなく、送信したメッセージを誰も受信しません。 | 下の `--disable` で意図して外したときは、そのままで構いません。 |

ほかの ECU を外して試すときは、設定ファイルを変更せずに、`--disable` に participant 名を指定します。たとえば `vehicle_sil_launch check simulation.yaml --disable Dashboard` とすると、`J1939_CAN` に `Engine` だけが残るので、次の警告が出ます。警告だけなら、最後の行は `passed:` のままです。

<!-- first ECU: vehicle_sil_launch check simulation.yaml --disable Dashboard, the warning -->
```text
warning: simulation.yaml:9: network 'J1939_CAN': only Engine is on it, so what it sends there reaches no one: add the other participants of this network, or remove it [single-participant]
```

指摘のコードの一覧は、[simulation-check.md](../simulation-check.md) にあります。

## 手順 6: シミュレーションを実行する

3 つの ECU を、`stop_at_ms` に書いた 1000 ms まで実行します。

```sh
vehicle_sil_launch run simulation.yaml
```

### 確認

標準出力の最後の表で、`Cooler` の `ended` が `exit code 0` になっていることを確認します。

<!-- first ECU: vehicle_sil_launch run simulation.yaml, after the tables of the check -->
```text
run: the simulation ran to stop_at_ms (1000 ms) and ended

participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0
Cooler       yes       exit code 0

passed: 0 errors, 0 warnings, 0 notes
```

`run` の出力に、変数の値は表示されません。標準エラーに `[Dashboard]` の行があるのは、表示の ECU が自分で `printf` を呼んでいるからで、何も出力しない `Cooler` の行はありません。変数の値は、次のどれかで見ます。

| 方法 | 向いている場面 | 手順 |
|---|---|---|
| Python のクライアント | スクリプトから値を読み書きするとき | 手順 7 |
| シナリオ | 値の設定と確認を、自動のテストにするとき | [シナリオでテストを書く](scenario-tests.md) |
| ブラウザの GUI | 値をグラフで見ながら、手で操作するとき | [gui.md](../gui.md) |

途中で止めるときは Ctrl+C です。出力の形式と終了コードは、[simulation-run.md](../simulation-run.md) の「出力」にあります。

## 手順 7: Python から変数を読み書きする

`Cooler` の目標の温度を書き込み、500 ms 進めてから、水温を読み出します。ECU のコードに追加するものはありません。`ecu_main()` で作った ECU の変数は、そのまま外から読み書きできます。

`read_cooler.py` に次の内容を書きます。これがファイルの全文です。

<!-- examples/first_ecu/read_cooler.py -->
```python
from vehicle_sil_control import Control, Launch, ms

# Starts "vehicle_sil_launch run simulation.yaml" with its control server, paused before the first step.
with Launch("vehicle_sil_launch", "simulation.yaml", start_paused=True) as launch:
    with Control(launch.address) as c:
        c.sim.wait_paused()                                   # every ECU stands still at 0 ms
        seq = c.write({"Cooler": {"cooler.target": 22.0}})    # participant name, then variable name
        c.wait(seq, until="queued")                           # the ECU has received the write

        c.sim.step(ms(500))                                   # run 500 ms, then pause again
        now = c.sim.wait_paused().now
        values = c.values("Cooler").values                    # the values of the last step
        print(f"at {now / 1e6:g} ms: cooler.target = {values['cooler.target']}, "
              f"cooler.temperature = {values['cooler.temperature']}")
```

Python のクライアント（`vehicle_sil_control`）は、ビルド済みのツールの `python/` に入っています。uv で実行します。`<version>` は、`vehicle_sil_launch version` が表示するバージョン（`tools/` の下のディレクトリの名前と同じ）です。

```sh
T=tools/vehicle-sil-tools-<version>-linux-x86_64
PYTHONPATH=$T/python uv run --no-project --no-build --python '>=3.11,<3.15' \
    --with-requirements $T/python/requirements.txt python read_cooler.py
```

| 引数 | 意味 |
|---|---|
| `PYTHONPATH=$T/python` | クライアントのパッケージの場所です。 |
| `--with-requirements` | `requirements.txt` に固定したバージョンのパッケージ（grpcio、protobuf など）をインストールします。 |
| `--python '>=3.11,<3.15'` | この範囲の Python を使い、PC になければ uv がダウンロードします。 |
| `--no-project` | 上のディレクトリに `pyproject.toml` があっても読みません。 |
| `--no-build` | ビルド済みのパッケージ（wheel）だけを使います。 |

### 確認

<!-- examples/first_ecu/read_cooler.py: the output -->
```text
at 500 ms: cooler.target = 22.0, cooler.temperature = 22.0
```

水温は 20 degC から 100 ms ごとに 0.5 degC ずつ上がり、書き込んだ目標の 22 degC で止まっています。書き込まなければ、目標は 90 degC のままです。

スクリプトの各行がすることは、次のとおりです。

| 行 | すること |
|---|---|
| `Launch(..., start_paused=True)` | `vehicle_sil_launch run` を子プロセスとして起動し、どの ECU も最初のステップの前で止めます。 |
| `Control(launch.address)` | 起動した `vehicle_sil_launch` の Control Server に、gRPC で接続します。 |
| `c.sim.wait_paused()` | シミュレーションが止まるまで待ちます。 |
| `c.write({"Cooler": {"cooler.target": 22.0}})` | 設定ファイルの participant 名と、`--describe` の変数の名前を指定して書き込みます。 |
| `c.wait(seq, until="queued")` | ECU が書き込みを受信するまで待ち、値は時刻 0 のステップで反映されます。 |
| `c.sim.step(ms(500))` | 仮想時間を 500 ms 進めて、また止めます。 |
| `c.values("Cooler").values` | ECU が最後に送信した値を、変数の名前をキーとする dict で返します。 |

`with` を抜けると、シミュレーションを止めて終わります。

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `ModuleNotFoundError: No module named 'vehicle_sil_control'` | `PYTHONPATH` を指定していません。 | コマンドの先頭に `PYTHONPATH=$T/python` を付けます。 |
| `FileNotFoundError: ... 'vehicle_sil_launch'` | `vehicle_sil_launch` が `PATH` にありません。 | 手順 1 の `export` の行を実行します。 |
| `AttributeError: 'NoneType' object has no attribute 'values'` | ステップを 1 回も進めていない間は値がなく、`c.values()` が `None` を返します。 | `c.sim.step()` で進めてから読み出します。 |
| 書き込んだ値が反映されない | 範囲の外の値（`cooler.target` は 0〜120）や、`output()` の変数への書き込みは、例外にならず、結果が `invalid` になります。 | `c.wait(seq).target("Cooler").status` で結果を確認します。 |
| dict に `peer.steps` などの知らない名前がある | vehicle-sil が自動で追加する監視用の変数です。 | 自分の変数は、`--describe` で `builtin` が `false` のものです。 |

動いている途中で止めて書き込む方法と、複数の ECU の値を同じ時刻で読み出す方法は、[信号を外から操作する](operate-signals.md) にあります。

## J1939 で通信させる

`Cooler` はネットワークを使いませんでした。CAN の上の J1939 で送受信する ECU は、テンプレートのエンジン（送信）と表示（受信）が例になります。変数だけの ECU に追加するものは、次のとおりです。

| 追加するもの | 例のある場所 |
|---|---|
| メッセージの定義（PGN とデータの形式）を、送信側と受信側で共有するヘッダー | [template/engine_speed.hpp](../../../template/engine_speed.hpp) |
| ヘッダーの `#include` と、名前空間の別名 `j1939` | [template/engine/main.cpp](../../../template/engine/main.cpp) の 11 行と 18 行 |
| ネットワークへの接続と、J1939 のノード | 同じファイルの 27〜28 行 |
| 周期的な送信（`node_.send()`） | 同じファイルの 38〜40 行 |
| 受信（`node.on_message()`） | [template/dashboard/main.cpp](../../../template/dashboard/main.cpp) の 22〜27 行 |
| 設定ファイルの `networks` の項目 | 手順 1 の `simulation.yaml` の `J1939_CAN` |

エンジンの ECU のクラスは、次のとおりです（`template/engine/main.cpp` の 22〜47 行）。

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

ほかの ECU と同じネットワークで動かすために、次の 3 つを決めます。

| 決めるもの | ルール | 違反したとき |
|---|---|---|
| ネットワークの名前（`"J1939_CAN"`） | コードと、設定ファイルの `networks` で同じ名前にします。 | `check` が `[unknown-network]` のエラーを報告します。 |
| J1939 のアドレスと NAME | 同じネットワークのほかのノードと重ならない値にします。 | `check` が `[address-conflict]`、`[address-shared]`、`[name-conflict]` のどれかを報告します。 |
| Manufacturer Code | 例の `0x7F0` は仮の値なので、自分の値に変えます。 | — |

`--describe` の `networks` で、ネットワークの名前、アドレス、NAME を確認できます。送受信の書き方は [j1939-guide.md](../j1939-guide.md) に、信号を YAML で定義して型を生成する方法は [j1939-codec.md](../j1939-codec.md) にあります。

## デバッガーから動かす

自分の ECU をデバッガーで止めながらシミュレーションを動かすときは、その ECU だけを自分で起動します。

1. 設定ファイルは変更せずに、`--manual` に participant 名を指定して実行します。

   ```sh
   vehicle_sil_launch run simulation.yaml --manual Cooler
   ```

2. `vehicle_sil_launch` は、ほかの ECU を起動し、`Cooler` を起動するコマンドを標準エラーに表示して待ちます（`<dir>` はプロジェクトのディレクトリです）。

   ```text
   [vehicle_sil_launch] not starting Cooler: start it yourself (manual: true, or --manual); the simulation waits for it to join. In the directory <dir>, run:
   [vehicle_sil_launch]   <dir>/bin/cooler_ecu --name Cooler --registry-uri silkit://127.0.0.1:8500 --start-timeout-ms 1000000000000
   [vehicle_sil_launch] waiting for Cooler to join (manual): start it with the command above; Ctrl+C stops the run
   ```

3. デバッガーに、2 行目の実行ファイル、その後の引数、1 行目の `In the directory` の後の作業ディレクトリを設定して、起動します。端末で試すときは、そのディレクトリで 2 行目のコマンドを実行します。

4. `Cooler` が参加するとシミュレーションが始まり、終わると `Cooler` も自分で終了します。

### 確認

結果の表で、`Cooler` の `ended` が `started by you (manual)` になります。

```text
participant  required  ended
Engine       yes       exit code 0
Dashboard    yes       exit code 0
Cooler       yes       started by you (manual)

passed: 0 errors, 0 warnings, 1 note
```

### 知っておくこと

| 状況 | 起きること |
|---|---|
| `Cooler` を起動するまで | `vehicle_sil_launch` は、時間の制限なく待ちます。 |
| ブレークポイントで止めている間 | シミュレーション全体の仮想時間が止まります。 |
| 参加した後、シミュレーションが始まる前に `Cooler` を終了させた | 開始に失敗して終わるので、`vehicle_sil_launch` から起動し直します。 |
| 表示されるコマンド | registry のポートが 8500 に決まるので毎回同じで、デバッガーの設定を使い回せます。 |
| ポート 8500 をほかのプログラムが使っている | 設定ファイルの `registry_uri` に、別のポートを書きます。 |
| `Cooler` の出力 | 起動した側（デバッガーや端末）に表示され、`vehicle_sil_launch` の標準エラーには出ません。 |
| PowerShell にコマンドを貼り付ける | `"` で囲まれた実行ファイルの前に `&` を付けます。 |

待ち方と止め方のルールは、[simulation-config.md](../simulation-config.md) の「participant を自分で起動する」にあります。

## 次に読むもの

- ほかの人が作った ECU と一緒に動かす → [複数の ECU をつないで動かす](integrate-ecus.md)
- 値の設定と確認を、自動のテストにする → [シナリオでテストを書く](scenario-tests.md)
- 動いている途中で止めて書き込む、時間を進める → [信号を外から操作する](operate-signals.md)
