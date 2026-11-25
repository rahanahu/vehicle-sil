# ブラウザの GUI に自分の画面を作る

[English](../../howto/gui-screen.md)

画面のテンプレート（`gui_starter`）をコピーし、中の変数の名前を自分の ECU のものに変えて、ECU の変数を 1 つ表示する画面にし、GUI サーバー（`vehicle-sil-gui`）に配信させる手順です。画面は、あらかじめ用意した部品（`<vsil-slider>`、`<vsil-value>` など）でできているので、書くのは HTML だけで、スクリプトは書きません。ビルドは使いません。各手順の「確認」には、ブラウザで確かめる方法と、できるときはブラウザなしの方法（`curl`。手順 4 では Node.js も）が書いてあります。何が要るかは、次の前提に書いてあります。

自分の画面は、サンプルの画面と置き換わります。サンプルの画面を残して自分の ECU の部品を足すなら、代わりにカスタムパネルを書きます。3 つの道を比べた表と、自分の画面を作る 3 つの層（部品だけ、自分のスクリプト、フレームワークのプロジェクト）は、[gui.md](../gui.md#自分の画面を作る) の「自分の画面を作る」にあります。

## 前提

| 必要なもの | 条件 |
|---|---|
| テンプレートのプロジェクト | [最初の ECU を作る](first-ecu.md) の手順 1 まで済ませて、ECU をビルドし、ツールの `bin` を `PATH` に追加しておきます。 |
| Python | 3.11 以上が必要で、[uv](https://docs.astral.sh/uv/) があれば、Python とパッケージは uv がインストールします。 |
| ブラウザ | 画面で確かめるときに使います。GUI サーバーと同じ PC のものを使います。 |
| `curl` | ブラウザなしで確かめるときに使います。 |
| Node.js | 22 以上。ブラウザなしで値を読むときだけ使います（手順 4 の最後）。 |

コマンドは、テンプレートのプロジェクトのディレクトリ（`simulation.yaml` のある所）で実行します。ビルド済みのツールのディレクトリを次の変数に設定し、その `bin` を `PATH` に入れておきます（新しいシェルでは、もう一度実行します）。

```sh
TOOLS=tools/vehicle-sil-tools-<version>-linux-x86_64   # <version> は tools/ の下のディレクトリの名前のもの
export PATH="$PWD/$TOOLS/bin:$PATH"                    # vehicle-sil-gui と vehicle_sil_launch
```

リポジトリをビルドして使う場合は、次のように読み替えます。

| | テンプレートのプロジェクト | リポジトリ（ルートで実行） |
|---|---|---|
| 画面のテンプレート | `$TOOLS/gui_starter/` | `examples/gui_starter/` |
| ライブラリと部品 | GUI サーバーが画面に配信します（`/vsil/`）。 | `(cd gui/web && npm ci && npm run build:client)` で 1 回ビルドします。GUI サーバーが `build/gui-client/` から配信します。 |
| 起動するコマンド | `vehicle-sil-gui --web my-screen simulation.live.yaml` | `uv run vehicle-sil-gui --launcher build/<preset>/tools/launch/vehicle_sil_launch --web my-screen build/<preset>/examples/ecu_main/simulation.live.yaml` |
| ランチャーと ECU | `vehicle_sil_launch`、`bin/engine_ecu` | `build/<preset>/tools/launch/vehicle_sil_launch`、`build/<preset>/examples/ecu_main/` の実行ファイル |

## 全体の流れ

| 手順 | すること | できるもの |
|---|---|---|
| 1 | 画面のテンプレートをコピーして、そのまま配信する | `my-screen/` と、テンプレートの画面 |
| 2 | 表示する変数の名前を調べる | participant の名前 `Engine` と、変数の名前 `engine.speed` |
| 3 | 変数を 1 つ表示する画面に変える | `my-screen/index.html` |
| 4 | 配信して、値が表示されることを確認する | 回転数を表示する画面 |

## 手順 1: 画面のテンプレートをコピーして配信する

画面のテンプレートを `my-screen/` にコピーし、`--web` でそのディレクトリを渡して GUI サーバーを起動します。

```sh
cp -r "$TOOLS/gui_starter" my-screen
vehicle-sil-gui --web my-screen simulation.live.yaml   # ブラウザを開く（開かないときは --no-browser で URL だけ表示）。Ctrl+C で止まる
```

`my-screen/` には、次のファイルができます。

| ファイル | 内容 |
|---|---|
| `index.html` | 画面です。 |
| `main.js` | 部品にないこと（表示、ボタン）のための、短い自分のスクリプトです。 |
| `style.css` | 部品の見た目です。 |
| `README.md` | テンプレートの説明です（配信されません）。 |
| `README.ja.md` | 同じ説明の日本語版です（配信されません）。 |

`index.html` は、部品とライブラリを `my-screen/` からではなく GUI サーバーから読みます。`/vsil/vehicle-sil-gui-elements.mjs`（部品）、`/vsil/vehicle-sil-gui-client.mjs`（部品と `main.js` が import するライブラリ）、`/timeline/vehicle-sil-timeline.js` です。どれかが 404 になるときは、[gui.md](../gui.md#vsil-で配るファイル) の「`vsil/` で配るファイル」を見てください。

### 確認

端末に URL が表示され、ブラウザに「My vehicle-sil screen」という画面が開きます（ブラウザを開かないときは、`--no-browser` を付けて起動します）。「Start」を押すと、画面の一番上の行が、仮想時間つきの `running` になります。

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
| `missing or wrong token: send Authorization: Bearer <token>, the token in the URL the server printed (#token=...)` という文が 1 つ表示される | トークン認証が有効なサーバーに対して `#token=` のない URL を開いています（文はページで 1 回だけ、`<vsil-status>` に出ます。ページに `<vsil-status>` がなければ最初の部品に出ます。どの部品にも `data-error` が付きます）。 | 端末に表示された URL を、`#token=` の後ろまで含めて開きます（信頼できるローカル環境でトークン認証が不要な場合は `--no-auth` で起動します）。 |

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

一覧に出るのは、設定ファイルの ECU が宣言した変数です（このテンプレートでは 4 つ。自分の ECU を使うなら、その変数が出ます）。participant の名前、変数の名前、型、入出力の向き、単位、説明が表示されます。説明は ECU の作者が書いたもので、空のこともあります。どの変数かを名前と説明で決められないときは、ECU の作者に確認します。

```text
Engine | engine.throttle | real | input | % | accelerator pedal position
Engine | engine.speed | integer | output | rpm |
Dashboard | dashboard.engine_speed | integer | output | rpm | the speed shown
Dashboard | dashboard.engine_lost | bool | output |  | no engine speed for more than 300 ms (it comes every 100 ms)
```

この手順では、`Engine` の `engine.speed`（エンジンの回転数）を表示します。`output` の変数は読み取り専用で、`input` の変数には書き込めます。部品、シナリオ、ダッシュボードのファイルでは `Engine.engine.speed`（participant の名前、ドット、変数の名前。最初のドットで切るので、名前にドットのある participant はスクリプトで `{ peer, name }` を使います）と書きます。

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| `vehicle_sil_launch` か `vehicle-sil-gui` が見つからない | ツールの `bin` が `PATH` にありません（新しいシェルでは、もう一度入れる必要があります）。 | 「[前提](#前提)」の `TOOLS=...` と `export PATH=...` の行を実行します。 |

## 手順 3: 変数を 1 つ表示する画面に変える

テンプレートは、部品を並べたページです。部品は HTML の要素で、1 つのものを描き、その仕事を自分で行います。だから `<vsil-value variable="Engine.engine.speed">` は、スクリプトなしで、その変数を表示します。自分の変数を表示するには、`variable` 属性を、手順 2 で調べた participant の名前と変数の名前をドットでつないだもの（`Engine.engine.speed`。名前は最初のドットで切ります）に変えます。

ページに最低限必要なものを見るために、`my-screen/index.html` を次の内容に置き換えます。ページは部品を GUI サーバー（`/vsil/`）から読むので、コピーするものはありません。`src` で読み込むのは、GUI サーバーが付ける CSP が、インラインの `<script>` を止めるからです。

```html
<!doctype html>
<html lang="ja">
  <head>
    <meta charset="utf-8" />
    <title>エンジンの回転数</title>
    <script type="module" src="/vsil/vehicle-sil-gui-elements.mjs"></script>
  </head>
  <body>
    <h1>エンジンの回転数</h1>
    <vsil-status></vsil-status>
    <vsil-run-controls></vsil-run-controls>
    <p>Engine.engine.speed: <vsil-value variable="Engine.engine.speed"></vsil-value></p>
  </body>
</html>
```

このページの部品です。

| 要素 | 見せるもの、すること |
|---|---|
| `<vsil-status>` | 接続、実行、仮想時間と、いま伝える問題 1 つ（断られた開始など） |
| `<vsil-run-controls>` | 「Start」と「Stop」。「Start」は、最後の実行のファイル（最初は、サーバーの起動時に渡した設定ファイル）を実行します。 |
| `<vsil-value variable="...">` | 単位つきの値。生きている値でないときは、理由の文が出ます（まだ実行がない、一時停止、古い、実行が終わった）。 |

このページで使っていない `<vsil-value>` の属性です。

| 属性 | 意味 |
|---|---|
| `on`、`off` | bool の変数で、true と false に出す文字です（`on="LOST" off="ok"` なら `LOST` か `ok` と出ます）。指定しないと `on` と `off` と出ます。 |
| `no-unit` | 単位を出しません（`1200 rpm` でなく `1200`）。 |

部品の全部と属性は、[gui/client/README.md](../../../gui/client/README.ja.md#部品custom-element) の「部品（custom element）」にあります。テンプレートは、そのほとんど（スライダー、値を書き込むボタン、ランプ、時間とシナリオの操作）を使っています。

このページにはスクリプトがないので、`main.js` はもう使いません。`main.js` と `style.css` は消してかまいません（テンプレートにあるのは、「自分のスクリプト」と部品の見た目の付け方の見本としてです。その README を見てください）。部品には自分の見た目がありません。`data-*` 属性が付いた部品の見た目の付け方は、テンプレートの `style.css` にあります。

このページの細かい点です。

| 項目 | 内容 |
|---|---|
| 名前 | この実行にない名前は、ページのエラーにはなりません。部品が「this variable is not in the current run: check the names (ECU and variable)」と表示します。 |
| 接続は 1 本 | 部品はページの接続を共有します。自分のスクリプトも同じ接続を使います。テンプレートの `main.js` の `connect()` が同じ接続を返します。断られた開始、停止、時間の操作は `<vsil-status>` に出ます。断られた変数の書き込みは、その変数を出している部品（たとえばスライダー）の所と、自分のスクリプトが受ける `ActionError` に出ます。 |
| ボタンが使えない理由 | 今は仕事をできない部品（実行がないときの「Stop」）は `aria-disabled="true"` になり、理由のコードが `data-reason` に入ります。文はスクリーンリーダーが読みます。何もすることがないと言うだけの理由（実行がない）は、画面には出ません。 |

## 手順 4: 配信して確認する

手順 1 のサーバーが動いたままなら、起動し直さず、ブラウザでページを読み込み直します（サーバーはリクエストのたびにファイルを読み直します）。サーバーを起動するには、次を実行します。

```sh
vehicle-sil-gui --web my-screen simulation.live.yaml   # ブラウザを開く（開かないときは --no-browser で URL だけ表示）。Ctrl+C で止まる
```

### 画面で確かめる

ブラウザに「エンジンの回転数」の画面が開きます。

| 操作 | 表示 |
|---|---|
| 開いた直後 | 一番上の行は `ready`。`Engine.engine.speed: -` と、変数がいまは使えない（実行がない）という文。 |
| 「Start」を押す | 一番上の行は `running` と仮想時間。`Engine.engine.speed: 1200 rpm` |
| 「Stop」を押す | 一番上の行は `The run was stopped by a stop request without errors`。`Engine.engine.speed: 1200 rpm` と、「the last value of the run that has ended」 |

### ブラウザなしで確かめる（API と Node.js）

まず `curl` で、ファイルが配信されていることを確かめます。`URL` と `BASE` は、手順 1 と同じように、今回表示された URL で設定します。

```sh
URL='<サーバーが表示した URL>'
BASE="${URL%%/#*}"
curl -s "$BASE/" | grep '<title>'
curl -s -o /dev/null -w '%{http_code} %{content_type}\n' "$BASE/vsil/vehicle-sil-gui-elements.mjs"
```

```text
    <title>エンジンの回転数</title>
200 text/javascript; charset=utf-8
```

次に、API を呼びます。API にはトークンが必要で、`POST` には `Origin` も必要です。トークンをコマンドの引数に書くと、同じ PC のほかの利用者にプロセスの一覧から見えるので、ヘッダーをファイルに置いて渡します。実行を開始します。

```sh
(umask 077; printf 'Authorization: Bearer %s\n' "${URL##*#token=}" > token.header)

# 実行を開始する（あとで stop-session に渡す {instance, run_id} を保存）
RUN_JSON=$(curl -s -X POST -H @token.header -H "Origin: $BASE" -H 'Content-Type: application/json' \
  -d '{"file": "simulation.live.yaml", "disable": [], "start_paused": false}' \
  "$BASE/api/run")
echo "$RUN_JSON"
```

```text
{"instance": "0123456789abcdef", "run_id": "0123456789abcdef-1"}
```

値は WebSocket で届くので、`curl` では読めません。値は、Node.js（22 以上）と、画面と同じライブラリで読みます。

ライブラリのファイルを、プロジェクトのディレクトリにコピーします（ツールでは `$TOOLS/client/`、リポジトリでは `build/gui-client/` にあります）。

```sh
cp "$TOOLS/client/vehicle-sil-gui-client.mjs" .
```

次の内容を、プロジェクトのディレクトリに `read-value.mjs` として保存します。

```js
// 自分の画面と同じライブラリで、変数の値を 1 回読み取って表示する（Node 22 以上）。
import { connect, until } from './vehicle-sil-gui-client.mjs';

const url = new URL(process.env.VEHICLE_SIL_GUI_URL); // サーバーが表示した URL
const client = await connect({ base_url: `${url.origin}/`, token: url.hash.replace('#token=', '') });
const speed = client.variable('Engine.engine.speed');
// 生きている値を待つ（`note` は、値が生きていないときの文）
const view = await until(speed, (v) => v.value !== null && v.note === null, { timeout_ms: 10_000 });
console.log(`Engine.engine.speed = ${view.text} ${view.unit}`);
client.close();
```

同じ場所で実行します。

```sh
VEHICLE_SIL_GUI_URL="$URL" node read-value.mjs
```

```text
Engine.engine.speed = 1200 rpm
```

終わったら、`instance` と `run_id` を `POST /api/stop-session` に送って実行を停止し、トークンのファイルを消します。`"force": false` は実行を順に止め、`"force": true` は待たずに止めます。

```sh
STOP_BODY=$(node -e 'const r = JSON.parse(process.argv[1]); console.log(JSON.stringify({instance: r.instance, run_id: r.run_id, force: false}));' "$RUN_JSON")
curl -s -X POST -H @token.header -H "Origin: $BASE" -H 'Content-Type: application/json' \
  -d "$STOP_BODY" "$BASE/api/stop-session" > /dev/null
rm token.header
```

この要求への答え（`{"instance": "...", "run_id": "..."}`）は、サーバーがその実行への停止要求を受け付けたことを言うだけです。実行が終わるのは数秒後です（`GET $BASE/api/session` の `session` の `state` が `"ended"` になります）。そのあと、Ctrl+C で GUI サーバーを止めます。

### うまくいかないとき

| 症状 | 原因 | 直し方 |
|---|---|---|
| 値が `-` のまま、変数がいまは使えないという文が出る | 実行を開始していないか、実行に Control Server がありません。 | 「Start」を押します。開始済みなら、Control Server のあるランチャーで GUI サーバーを起動します。 |
| 部品が `this variable is not in the current run` と言う | participant の名前か変数の名前が、この実行にありません。 | 手順 2 で名前を確認します。 |
| 画面に何も表示されず、ブラウザのコンソールに CSP のエラーが出る | インラインの `<script>` や `onclick="..."` を使っています。 | スクリプトをファイルに置き、`addEventListener` か `onclick` のプロパティで登録します。 |
| ファイルが 404 になる | 配信されない拡張子か、`.` で始まる名前、シンボリックリンクです。 | [gui.md](../gui.md) の「`--web` で配信するファイル」の表を確認します。 |
| `curl` の `POST` が `forbidden-origin` で拒否される | `Origin` ヘッダーがないか、サーバー自身の origin と違います。 | `-H "Origin: http://127.0.0.1:<port>"` を付けます。 |
| ファイルを直したのに、画面が変わらない | ブラウザが前のページを表示しています。 | ページを読み込み直します（GUI サーバーの再起動は不要です）。 |
| `cannot listen on 127.0.0.1:<port> (Address already in use)` | 動いているサーバーと同じ `--port` で、2 つ目の GUI サーバーを起動しました。 | 1 つ目を止めるか、別の `--port` を選びます（`--port` を付けなくてもかまいません）。`--port` なしなら、2 つ目は別のポートで、自分のトークンと自分のランチャーで起動します。表示された URL を使ってください。 |

## ネットワークを外につなぐ（任意）

構成ファイルの `can` か `ethernet` のネットワークは、ランチャーのブリッジを通して実機のバスにつなげます（[simulation-run.md](../simulation-run.md#バスを外につなぐ--connect) の「バスを外につなぐ（`--connect`）」）。部品 2 つで行います。

```html
<label>ETH_EXT <vsil-connect network="ETH_EXT"></vsil-connect></label>
<vsil-outside></vsil-outside>
```

| 要素 | 表示や動き |
|---|---|
| `<vsil-connect network="...">` | 次の実行でネットワークをつなぐ先。最初は `SIL Kit only (not connected)` です。先の一覧は、開いたときにだけ取ります。一覧は GUI サーバーが動いている PC のもので、ブラウザーの PC のものではありません。選ぶと、その選択について check が言うことを、ランチャーの文のまま出します（たとえば、実行が実時間を保つこと）。`<vsil-run-controls>` の「Start」は、この選択で開始します。 |
| `<vsil-outside>` | 実行中、つないだネットワークごとに 1 行: リンクかバス、状態、フレームの数。なければ何も出しません。 |

外につないだ実行は実時間を保つので、一時停止とステップは使えません。それでも使うときは、つないだ先が実時間でないメッセージを受けても困らないときだけ、自分のスクリプトで時間の方針を決めます: `pageIntent.update({ time_policy: 'free' })`（`pageIntent` は `/vsil/vehicle-sil-gui-elements.mjs` が export します。それが何を意味するかは check が言います）。

試験台は、選択を[要求ファイル](../simulation-run.md#要求ファイル--request)に持てます。スクリプトで、要求ファイルを次の実行の選択に読み込み、選択を要求ファイルとして保存できます。GUI サーバーはファイルを書きません。

```js
import { pageIntent } from '/vsil/vehicle-sil-gui-elements.mjs';

// <input type="file" id="request">: read the request file that the user picks
document.querySelector('#request').addEventListener('change', async (event) => {
  const request = JSON.parse(await event.target.files[0].text());
  delete request.$schema; // for an editor only
  pageIntent.update(request);
});
// <button type="button" id="save">: save the choice of the next run as a request file
document.querySelector('#save').addEventListener('click', () => {
  const text = JSON.stringify({ vehicle_sil_run_request: 1, ...pageIntent.get() }, null, 2);
  const link = document.createElement('a');
  link.href = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
  link.download = 'request.json';
  link.click();
});
```

- `<vsil-connect>` は、ファイルから読んだ先を書かれたとおりに出します。GUI サーバーは、そのネットワークの最後の一覧にある先だけを受けるので、開始の前に選択を開いてください。一覧にない先は SIL Kit だけに戻り、選択の下に、どの先をなぜ戻したかの 1 行が出ます。一覧がないと、開始は `target-not-listed` で断られます。
- 要求ファイルの `scenario` は構成ファイルのディレクトリからのパスです。GUI サーバーは、代わりにシナリオの一覧のパスを受けます（`<vsil-scenario-select>` が書きます）。

## 次に読むもの

- スクリプトで、自分の表示、ボタン、ルールを足す → テンプレートの `main.js`、[gui/client/README.md](../../../gui/client/README.ja.md#正面の口connect) の「正面の口（`connect()`）」。スクリプトは部品と同じ接続を使います。
- TypeScript とフレームワーク（React、Vue、Angular など）で作る（部品は custom element なので、どのテンプレートにも書けます） → [examples/gui_starter/README.md](../../../examples/gui_starter/README.ja.md#フレームワークのプロジェクト) の「フレームワークのプロジェクト」（正面の口だけでの実行の一巡と、Angular の signal があります）
- シナリオの実行の時間の軸を自分の画面に出す（スクリプト 1 つとタグ 1 つ。フレームワークを選びません） → [gui.md](../gui.md#自分の画面に時間の軸を置く) の「自分の画面に時間の軸を置く」
- 配信されるファイル、CSP、開発サーバー、守るルールを調べる → [gui.md](../gui.md) の「自分の画面のリファレンス」
- サンプルの画面を残して部品を足す → [gui.md](../gui.md#カスタムパネル) の「カスタムパネル」
