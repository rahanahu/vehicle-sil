# Control API の使い方

実行中のシミュレーションを、外から操作・監視します。Peer に Control Agent を組み込み、Control Server につなぎます。クライアント（Python、CLI、将来の GUI）は Control Server の gRPC の API（`proto/vehicle_sil/control/v1/control.proto`）を使います。契約の詳細（反映の時刻、順序、上限、スレッド）は [architecture.md](architecture.md) の「外部操作（Control API）と GUI」にあります。

```
クライアント（Python、CLI）──gRPC（TLS、トークン）──┐
                                                  ▼
                      vehicle_sil_control_server（Hub、時間ゲート）
                         ▲ gRPC（Agent が接続する）     │ SIL Kit（開始、停止、時刻同期）
          Peer + Agent ──┘                              ▼
          （どの PC でもよい）           SIL Kit のレジストリと CAN/LIN（データプレーン）
```

## ビルド

```sh
cmake --preset linux-gcc-release -DVEHICLE_SIL_BUILD_CONTROL_SERVER=ON
cmake --build --preset linux-gcc-release
```

gRPC が見つからなければ FetchContent で取り込みます（初回は取得とビルドに数分かかります）。Windows（MSYS2 UCRT64）は `pacman -S mingw-w64-ucrt-x86_64-grpc` で入れておきます。詳しくは [dependencies.md](dependencies.md)。

## Peer に Agent を組み込む

```cpp
#include "vehicle_sil/control/peer_agent.hpp"
#include "vehicle_sil/control_grpc/agent_client.hpp"

double throttle = 0.0;                      // モデル（Peer より先に宣言する）
double rpm = 800.0;
vehicle_sil::control::Agent agent;          // Agent も Peer より先
agent.input("engine.throttle", &throttle, {.unit = "%", .min = 0.0, .max = 100.0});
agent.output("engine.speed", &rpm, {.unit = "rpm"});

vehicle_sil::Peer peer(options);            // Coordinated、仮想時間
// ... J1939 のネットワーク、ノード、on_step() ...
vehicle_sil::control::attach(peer, agent);  // start() の前。J1939 の監視の出力も足す
vehicle_sil::control_grpc::AgentClient link(agent, {
    .server = "ctrl-pc:8510", .participant = options.participant,
    .status = [&] { return vehicle_sil::control::PeerStatus{peer.failed(), peer.error()}; },
    .token = agent_token, .root_certificates_pem = server_certificate});
peer.start();
peer.wait();
agent.finish();                             // 残っている操作を Cancelled に
link.stop();                                // その結果を送ってから閉じる
```

- 変数は bool、整数、実数、列挙で、単位、範囲、刻み、説明を付けられます。値は Agent のステップのスレッドで読み書きするので、モデルにロックは要りません。
- 外からの書き込みは、指定した仮想時刻以後の最初のステップで、アプリの処理の前に反映されます。
- 例: `examples/control/control_peer.cpp`（プロセスごとに 1 つの Peer）、`examples/control/control_demo.cpp`（1 つのプロセスにすべて）。

## Control Server を起動する

```sh
vehicle_sil_control_server --listen 0.0.0.0:8510 \
    --registry-uri silkit://localhost:8500 --required Engine,Body --start-paused 1 \
    --credentials credentials.txt --tls-cert server.crt --tls-key server.key --audit-log audit.log
```

- `--required` を付けると、Server が SimulationController を兼ね、必要な participant がそろったところでシミュレーションを始めます。`--gate-step-ms`（既定 10）は止められる時刻の刻みです。`--start-paused 1` で T = 0 で止めて始めます。
- 認証情報のファイルは 1 行に 1 つで、`<role> <name> <token> [participant,...]` です（role は viewer、operator、admin、agent。トークンは 16 文字以上の秘密の文字列）。agent の行に participant 名を書くと、その名前でしか接続できません。

```text
# role     name     token                     participants
operator   tester   0f3c9a...（長い乱数）
admin      lead     7b21e4...
agent      engine   c4d8f0...                 Engine
agent      body     91aa3b...                 Body
```

- ループバック以外で待ち受けるときは TLS と認証情報が必須です（試すだけなら `--allow-insecure 1`）。

## Python

```sh
pip install grpcio grpcio-tools protobuf
python -m vehicle_sil_control.generate          # スタブを python/vehicle_sil_control/_generated に作る
export PYTHONPATH=$PWD/python
```

```python
from vehicle_sil_control import Control, ms

with Control("ctrl-pc:8510", token=token, root_certificates=open("server.crt", "rb").read()) as c:
    print([p.participant for p in c.peers()])
    c.sim.wait_paused()
    seq = c.write({"Engine": {"engine.throttle": 50.0}, "Body": {"cabin.lamp": 2}}, at=ms(100))
    c.wait(seq, until="queued")         # 全員が受け取ってから進める（遅すぎる書き込みが起きない）
    c.sim.run_until(ms(500)); c.sim.wait_paused()
    print(c.wait(seq).target("Engine").applied_at)   # 100 ms（ns で）
    print(c.values("Engine").values["engine.speed"])
    for update in c.watch(peers=["Body"]):           # Peer、Values、EventInfo、Operation
        print(update)
        break
```

時刻は仮想時間のナノ秒です（`ms()`、`s()` で作る）。呼び出しの失敗は `ControlError`（`code` は gRPC の状態コード）で、Agent が断った書き込みは例外ではなく、操作の結果（`invalid` など）になります。

## CLI

```sh
export VEHICLE_SIL_CONTROL_SERVER=ctrl-pc:8510 VEHICLE_SIL_CONTROL_TOKEN=... VEHICLE_SIL_CONTROL_ROOT_CERT=server.crt
python -m vehicle_sil_control peers
python -m vehicle_sil_control show Engine
python -m vehicle_sil_control set Engine engine.throttle=50 --at-ms 100   # 既定は全員が積むまで待つ（--wait done で反映まで）
python -m vehicle_sil_control run-until 500
python -m vehicle_sil_control step 10
python -m vehicle_sil_control speed 1        # 実時間。max でできるだけ速く
python -m vehicle_sil_control watch --peer Body --count 10
python -m vehicle_sil_control stop
```

## 複数の PC

- SIL Kit のレジストリと Control Server を 1 台に置き、各 PC の Peer は両方へ外向きにつなぎます（Peer 側で待ち受けるポートは要りません。SIL Kit は participant どうしが直接つながらないとき、レジストリを中継に使います）。
- 発見: Server（`--required` のとき）は、シミュレーションの participant を SIL Kit のレジストリを通して見ているので、どの PC の Peer も、Agent がつながる前から一覧に出ます（`peers` の `has_agent` が偽、`simulation_state` に SIL Kit の状態）。Agent がつながると、その変数と値が加わります。
- Peer のプロセスが落ちて起動し直すと、Server は別の incarnation として扱い、前の未完了の操作を Unknown（反映されたかわからない）にします。回線が切れただけなら、Agent はつなぎ直し、Agent が受け取っていた操作はそのまま続きます（受け取ったと知らされる前に切れた操作は Unknown）。黙って切れた接続は keepalive で数十秒以内に見つけます。
- 試験: `tests/control_server/multi_process.py` は、レジストリ、Server、2 つの Peer をプロセスに分けて動かし、Python から操作します。`--remote-body` で Body を別の PC で動かせます。

## 制限

- 時間の操作（一時停止、ステップ実行、速度）は、Server が `--required` でシミュレーションを調停しているときだけです。止まるのは仮想時間で同期している Peer だけで、実時間の Peer は走り続けます。
- リセット、Peer の起動（Launcher）、CAN/LIN のチャネルの選択はありません。
- 値の購読は最新値です（読む前に来た新しい値で置き換えます）。すべての値の記録は、SIL Kit のトレースか `TapPort` を使います。
