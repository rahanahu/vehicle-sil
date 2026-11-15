# ブラウザの GUI に自分の画面を作る

[English](../../howto/gui-screen.md)

画面のテンプレート（`gui_starter`）をコピーし、ECU の変数を 1 つ表示する画面に書き換えて、GUI サーバー（`vehicle-sil-gui`）に配信させる手順です。画面を作るのに、ビルドも Node.js も使いません（手順 4 で値を API から確認するときだけ、Node.js を使います）。各手順の「確認」には、ブラウザがなくても `curl` でできる方法も書いてあります。

## 前提

| 必要なもの | 条件 |
|---|---|
| テンプレートのプロジェクト | [最初の ECU を作る](first-ecu.md) の手順 1 まで済ませて、ECU をビルドし、ツールの `bin` を `PATH` に追加しておきます。 |
| Python | 3.11 以上が必要で、[uv](https://docs.astral.sh/uv/) があれば、Python とパッケージは uv がインストールします。 |
| ブラウザか `curl` | ブラウザは、GUI サーバーと同じ PC のものを使います。 |
| Node.js | 22 以上。ブラウザを使わずに、手順 4 で値を確認するときだけ使います。 |

コマンドは、テンプレートのプロジェクトのディレクトリ（`simulation.yaml` のある所）で実行します。ビルド済みのツールのディレクトリを、次の変数に設定しておきます。

```sh
TOOLS=tools/vehicle-sil-tools-<version>-linux-x86_64   # <version> は vehicle_sil_launch version が表示するもの
```

リポジトリをビルドして使う場合は、次のように読み替えます。

| | テンプレートのプロジェクト | リポジトリ（ルートで実行） |
|---|---|---|
| 画面のテンプレート | `$TOOLS/gui_starter/` | `examples/gui_starter/` |
| ライブラリのファイル（`vehicle-sil-gui-client.mjs`） | `gui_starter/` に最初から入っています。 | `(cd gui/web && npm ci && npm run build:client)` で作り、`build/gui-client/` から画面のディレクトリにコピーします。 |
| 起動するコマンド | `vehicle-sil-gui --web my-screen simulation.live.yaml` | `uv run vehicle-sil-gui --launcher build/<preset>/tools/launch/vehicle_sil_launch --web my-screen build/<preset>/examples/ecu_main/simulation.live.yaml` |
| ランチャーと ECU | `vehicle_sil_launch`、`bin/engine_ecu` | `build/<preset>/tools/launch/vehicle_sil_launch`、`build/<preset>/examples/ecu_main/` の実行ファイル |

## 全体の流れ

| 手順 | すること | できるもの |
|---|---|---|
| 1 | 画面のテンプレートをコピーして、そのまま配信する | `my-screen/` と、テンプレートの画面 |
| 2 | 表示する変数の名前を調べる | participant の名前 `Engine` と、変数の名前 `engine.speed` |
| 3 | 変数を 1 つ表示する画面に書き換える | `my-screen/index.html` と `my-screen/main.js` |
| 4 | 配信して、値が表示されることを確認する | 回転数を表示する画面 |

## 手順 1: 画面のテンプレートをコピーして配信する

画面のテンプレートを `my-screen/` にコピーし、`--web` でそのディレクトリを渡して GUI サーバーを起動します。

```sh
cp -r "$TOOLS/gui_starter" my-screen
vehicle-sil-gui --web my-screen simulation.live.yaml
```

`my-screen/` には、次のファイルができます。

| ファイル | 内容 |
|---|---|
| `index.html` | 画面です。 |
| `main.js` | ライブラリを呼び出すスクリプトです。 |
| `style.css` | 見た目です。 |
| `vehicle-sil-gui-client.mjs` | クライアントのライブラリを 1 つにまとめたファイルです。 |
| `README.md` | テンプレートの説明です（配信されません）。 |

### 確認

端末に URL が表示され、ブラウザに「My vehicle-sil screen」という画面が開きます（ブラウザを開かないときは、`--no-browser` を付けて起動します）。「Start」を押すと、「Phase」が `running` になります。

```text
http://127.0.0.1:36917/#token=<起動のたびに変わるトークン>
vehicle-sil-gui: open the URL above (it has the token); Ctrl+C stops the server
```

ブラウザがないときは、別の端末から、表示された URL の `#` より前の部分を `curl` で読みます。ポートは起動のたびに変わるので、表示された URL から取り出します（決まったポートにするときは `--port`）。画面のファイルは、トークンなしで読めます。

```sh
URL='<サーバーが表示した URL>'
BASE="${URL%%/#*}"
curl -s "$BASE/" | grep '<title>'
```

```text
    <title>My vehicle-sil screen</title>
```

確認したら、Ctrl+C で GUI サーバーを止めます。別の端末からは、`kill <PID>`（SIGTERM）で止まります。

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `there is no index.html in it` と表示して、終了コード 2 で終わる | 渡したディレクトリに `index.html` がありません。 | `index.html` のあるディレクトリを渡します。 |
| `it has package.json, so it is the project and not its build` と表示して、終了コード 2 で終わる | ビルドの出力でなく、プロジェクトのディレクトリを渡しています。 | ビルドの出力のディレクトリ（`dist/` など）を渡します。 |
| 画面に `No token: open the URL that vehicle-sil-gui printed (with #token=...).` と表示される | `#token=` のない URL を開いています。 | 端末に表示された URL を、`#token=` の後ろまで含めて開きます。 |

## 手順 2: 表示する変数の名前を調べる

変数は、participant の名前と、変数の名前の組で指定します。名前は、次のどれかで調べます。

| 方法 | 向いているとき |
|---|---|
| `vehicle_sil_launch check --json <設定ファイル>` の `participants[].variables[]` | 設定ファイルの全部の ECU の変数を、実行の前に調べる |
| ECU の実行ファイルの `--describe`（`bin/engine_ecu --describe`）の `variables[]` | ECU を 1 つだけ調べる |
| 実行中の `GET api/session` の `session.variables.peers[].variables[]` | 実行中に追加される監視用の変数も含めて調べる |

ここでは、1 つ目の方法で、ECU が宣言した変数（`builtin` が偽のもの）を一覧にします。

```sh
vehicle_sil_launch check --json simulation.live.yaml | python3 -c "
import json, sys
for p in json.load(sys.stdin)['participants']:
    for v in p['variables']:
        if not v['builtin']:
            print(p['name'], v['name'], v['type'], v['causality'], v['unit'], v['description'], sep=' | ')
"
```

### 確認

participant の名前、変数の名前、型、入出力の向き、単位、説明が表示されます。説明は ECU の作者が書いたもので、空のこともあります。どの変数かを名前と説明で決められないときは、ECU の作者に確認します。

```text
Engine | engine.throttle | real | input | % | accelerator pedal position
Engine | engine.speed | integer | output | rpm |
Dashboard | dashboard.engine_speed | integer | output | rpm | the speed shown
Dashboard | dashboard.engine_lost | bool | output |  | no engine speed for more than 300 ms (it comes every 100 ms)
```

この手順では、`Engine` の `engine.speed`（エンジンの回転数）を表示します。`output` の変数は読み取り専用で、`input` の変数には書き込めます。ライブラリには `{ peer: 'Engine', name: 'engine.speed' }` の形で渡します（シナリオとダッシュボードのファイルでは `Engine.engine.speed` と書きます）。

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `vehicle_sil_launch` が見つからない | ツールの `bin` が `PATH` にありません。 | `get_tools.py` が表示した `export PATH=...` の行を実行します。 |

## 手順 3: 変数を 1 つ表示する画面に書き換える

`my-screen/index.html` を、次の内容に置き換えます。スクリプトは、ファイルから読み込みます（インラインの `<script>` は、GUI サーバーが付ける CSP で実行されません）。

```html
<!doctype html>
<html lang="ja">
  <head>
    <meta charset="utf-8" />
    <title>エンジンの回転数</title>
    <script type="module" src="main.js"></script>
  </head>
  <body>
    <h1>エンジンの回転数</h1>
    <p>接続: <output id="connection">-</output></p>
    <button type="button" id="start">開始</button>
    <button type="button" id="stop">停止</button>
    <p>Engine.engine.speed: <output id="value">-</output> rpm（<output id="condition">-</output>）</p>
    <p id="error" role="alert"></p>
  </body>
</html>
```

`my-screen/main.js` を、次の内容に置き換えます。

```js
import { createClient, errorMessage, takeToken } from './vehicle-sil-gui-client.mjs';

const CONFIG = 'simulation.live.yaml'; // 起動時に渡した設定ファイルのディレクトリ（--root）からの相対パス
const SPEED = { peer: 'Engine', name: 'engine.speed' }; // participant の名前と、変数の名前

const show = (id, text) => (document.getElementById(id).textContent = text);

// サーバーが表示した URL の #token= を読み取り、URL から消す
const token = takeToken(window);
if (token === null) {
  show('connection', 'vehicle-sil-gui が表示した URL（#token=... つき）から開いてください');
  throw new Error('no token');
}
const client = createClient({ baseUrl: new URL('/', location.href).href, token });
client.connection.state.subscribe((state) => show('connection', state));

// 変数 1 つの値と、値の状態（live、paused、stale、ended など）を表示する
const speed = client.variables.bind(SPEED, {});
const draw = ({ reading, condition }) => {
  show('value', reading === undefined ? '-' : String(reading.value));
  show('condition', condition.kind);
};
speed.state.subscribe(draw);
draw(speed.state.get());

const fail = (error) => show('error', errorMessage(error));
document.getElementById('start').onclick = () =>
  client.run.start({ file: CONFIG, disable: [], speed: null, start_paused: false }).catch(fail);
document.getElementById('stop').onclick = () => client.run.stop().catch(fail);

await client.loadApp();
client.connect();
```

`style.css` と `README.md` は使わないので、消してかまいません。スクリプトがしていることは、次のとおりです。

| 呼び出し | すること |
|---|---|
| `takeToken(window)` | URL の `#token=` を読み取って URL から消し、再読み込みに備えてタブの sessionStorage に置きます。 |
| `createClient({ baseUrl, token })` | クライアントを作ります（`baseUrl` は、サーバーのルートにします）。 |
| `client.variables.bind(ref, options)` | 変数 1 つの値と状態を、1 つの `state` にまとめます。 |
| `state.subscribe(fn)`、`state.get()` | `subscribe` は変化のたびに `fn` を呼び、`get()` はいまの状態を返します。 |
| `client.run.start(...)`、`client.run.stop()` | 実行を開始し、停止します。 |
| `client.loadApp()`、`client.connect()` | ランチャーの機能を読み込んでから、WebSocket で接続します。 |

`condition.kind` は値の状態で、コードではこの文字列を比較します。

| `condition.kind` | 意味 |
|---|---|
| `unavailable` | 変数の機能が使えない（実行していない、など） |
| `missing` | この実行にない変数 |
| `waiting` | まだ値がない |
| `live` | 動いている途中の値 |
| `paused` | 止まっている時刻の値 |
| `stale` | 古い値（理由つき） |
| `ended` | 終わった実行の最後の値 |

## 手順 4: 配信して確認する

```sh
vehicle-sil-gui --web my-screen simulation.live.yaml
```

### 確認

ブラウザに「エンジンの回転数」の画面が開きます。

| 操作 | 表示 |
|---|---|
| 開いた直後 | `接続: open`、`Engine.engine.speed: - rpm（unavailable）` |
| 「開始」を押す | `Engine.engine.speed: 1200 rpm（live）` |
| 「停止」を押す | `Engine.engine.speed: 1200 rpm（ended）` |

ブラウザがないときは、配信されていることを `curl` で確認します。`URL` と `BASE` は、手順 1 と同じように、今回表示された URL で設定し直します。

```sh
URL='<サーバーが表示した URL>'
BASE="${URL%%/#*}"
curl -s "$BASE/" | grep '<title>'
curl -s -o /dev/null -w '%{http_code} %{content_type}\n' "$BASE/main.js"
```

```text
    <title>エンジンの回転数</title>
200 text/javascript; charset=utf-8
```

値まで確認するには、API を呼びます。API にはトークンが必要で、`POST` には `Origin` も必要です。トークンをコマンドの引数に書くと、同じ PC のほかの利用者にプロセスの一覧から見えるので、ヘッダーをファイルに置いて渡します。

```sh
(umask 077; printf 'Authorization: Bearer %s\n' "${URL##*#token=}" > token.header)

# 実行を開始する
curl -s -X POST -H @token.header -H "Origin: $BASE" -H 'Content-Type: application/json' \
  -d '{"file": "simulation.live.yaml", "disable": [], "speed": null, "start_paused": false}' \
  "$BASE/api/run" | head -c 60; echo
```

```text
{"session": {"id": 1, "file": "simulation.live.yaml", "disab
```

値は WebSocket で届くので、`curl` では読めません。Node.js（22 以上）があれば、画面と同じライブラリで読めます。次の内容を `read-value.mjs` として保存し、実行します。

```js
// 自分の画面と同じライブラリで、変数の値を 1 回読み取って表示する（Node 22 以上）。
import { createClient } from './my-screen/vehicle-sil-gui-client.mjs';

const url = new URL(process.env.VEHICLE_SIL_GUI_URL); // サーバーが表示した URL
const client = createClient({ baseUrl: `${url.origin}/`, token: url.hash.replace('#token=', '') });
await client.loadApp();
client.connect();
const speed = client.variables.bind({ peer: 'Engine', name: 'engine.speed' }, {});
speed.state.subscribe(({ reading, condition }) => {
  if (reading === undefined) return;
  console.log(`Engine.engine.speed = ${reading.value} (${condition.kind})`);
  speed.close();
  client.close();
});
```

```sh
VEHICLE_SIL_GUI_URL="$URL" node read-value.mjs
```

```text
Engine.engine.speed = 1200 (live)
```

終わったら、実行を停止し、トークンのファイルを消して、Ctrl+C で GUI サーバーを止めます。

```sh
curl -s -X POST -H @token.header -H "Origin: $BASE" -H 'Content-Type: application/json' \
  -d '{"force": false}' "$BASE/api/stop" > /dev/null
rm token.header
```

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| 値が `-`、状態が `unavailable` のまま | 実行を開始していません。 | 「開始」を押します。 |
| 状態が `missing` | participant の名前か変数の名前が、この実行にありません。 | 手順 2 で名前を確認します。 |
| 画面に何も表示されず、ブラウザのコンソールに CSP のエラーが出る | インラインの `<script>` や `onclick="..."` を使っています。 | スクリプトをファイルに置き、`addEventListener` か `onclick` のプロパティで登録します。 |
| ファイルが 404 になる | 配信されない拡張子か、`.` で始まる名前、シンボリックリンクです。 | [gui.md](../gui.md) の「`--web` で配信するファイル」の表を確認します。 |
| `curl` の `POST` が `forbidden-origin` で拒否される | `Origin` ヘッダーがないか、サーバー自身の origin と違います。 | `-H "Origin: http://127.0.0.1:<port>"` を付けます。 |
| ファイルを直したのに、画面が変わらない | ブラウザが前のページを表示しています。 | ページを読み込み直します（GUI サーバーの再起動は不要です）。 |

## 次に読むもの

- 値を書き込むスライダーやスイッチを作る → テンプレートの `index.html` の「Slider and lamp」と、それを動かす `main.js` の `showSlider()`、[gui/client/README.md](../../../gui/client/README.ja.md) の「コンポーネントを作る（`bind`）」
- TypeScript とフレームワーク（React、Vue など）で作る → [examples/gui_starter/README.md](../../../examples/gui_starter/README.ja.md) の「TypeScript とフレームワークで作る」
- 配信されるファイル、CSP、開発サーバー、守るルールを調べる → [gui.md](../gui.md) の「自分の画面のリファレンス」
