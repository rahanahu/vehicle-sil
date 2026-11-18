# 自分の画面のスターター（gui_starter）

[English](README.md)

GUI サーバー（`vehicle-sil-gui`）に配信させる自分の画面です。部品でできています。`<vsil-slider variable="Engine.engine.throttle">` のような要素を HTML に並べると、スクリプトもビルドもなしで動きます。これをコピーして、変数の名前を自分の ECU のものに変えて使います。その手順は、ガイド [ブラウザの GUI に自分の画面を作る](../../docs/ja/howto/gui-screen.md) にあります。

| ファイル | 内容 |
|---|---|
| `index.html` | 画面です。部品と、スクリプトのためのわずかな要素が入っています。 |
| `main.js` | 部品にないことのための、短い自分のスクリプトです（表示 1 つ、ボタン 1 つ）。 |
| `style.css` | 見た目です。部品は見た目を持たないので、部品が付ける `data-*` 属性と `aria-disabled` に当てる CSS が入っています。 |

部品とライブラリはこのディレクトリにありません。画面が GUI サーバーから読み込みます（`/vsil/vehicle-sil-gui-elements.mjs` と `/vsil/vehicle-sil-gui-client.mjs`）。

## 実行する

`--web` でこのディレクトリを渡して、GUI サーバーを起動します。

| 使うもの | コマンド |
|---|---|
| ビルド済みのツール | `vehicle-sil-gui --web <tools>/gui_starter simulation.yaml` |
| リポジトリ | ライブラリのファイルを 1 回ビルドしてから、サーバーを起動します（下のコマンド）。 |

リポジトリでは、ルートで次を実行します。

```sh
(cd gui/web && npm ci && npm run build:client)    # build/gui-client/ に、サーバーが /vsil/ で配るファイルができます
uv run vehicle-sil-gui --launcher build/<preset>/tools/launch/vehicle_sil_launch \
  --web examples/gui_starter build/<preset>/examples/ecu_main/simulation.yaml
```

### 確認

ブラウザが開き、サーバーが表示した URL（`#token=` つき）でこの画面が表示されます。

| 操作 | 起きること |
|---|---|
| 画面を開く | 一番上の状態の行に `ready` と出ます。変数は、いまは使えない（実行がない）と言います。 |
| 「Start」を押す | 状態の行が `running` と仮想時間（`running 2820 ms`）になります。スライダーと値が動き出します。 |
| 「Throttle」のスライダーを動かす | つまみを離したときに値を書き込みます。「Engine speed」が変わります。 |
| 「Throttle to 100 %」（自分のボタン）を押す | スライダーが 100 になり、「Your own script」のメーターがそれに続きます。実行がないときは、断られた理由が出ます。 |
| 「Pause」、「Step」、「Resume」を順に押す | 状態の行が `paused` と時間になります。「Step」は 10 ms（隣の欄）進め、「Resume」で続きます。 |
| 「Stop」を押す | 状態の行が `The run was stopped by a stop request without errors` になります。スライダーは使えなくなり、値は、終わった実行の最後の値だと言います。 |
| 「Scenario」でシナリオを選んで「Start」を押す | 実行は自分で終わります。結果が `passed`（か `failed`）と各ステップで出て、実行の時間の軸が現れます。 |

変数は、ランチャーが Control Server を持つときに出ます。例の ECU では、スロットルが `Engine.engine.speed` を動かします。

## 自分の画面にする

スターターを別のディレクトリにコピーして、そこで自分の画面を作ります。

| 使うもの | コピーするコマンド |
|---|---|
| ビルド済みのツール | `cp -r <tools>/gui_starter my-screen` |
| リポジトリ | `cp -r examples/gui_starter my-screen` |

そのうえで `--web my-screen` を付けてサーバーを起動します。渡すのは `index.html` のあるディレクトリです。配信のルール（配信するファイル、CSP、トークン）は [docs/gui.md](../../docs/ja/gui.md#自分の画面のリファレンス) の「自分の画面のリファレンス」にあります。

作り方は 3 つの層で、必要なところまで使えば足ります。

| 層 | 書くもの | 読むところ |
|---|---|---|
| 部品だけ | HTML（`index.html` の要素の変数名を自分のものに）と CSS。 | 下の表 |
| 自分のスクリプト | `main.js`。部品にないこと（表示、ボタン、ルール）を書きます。部品と同じ接続を使います。 | `gui/client/README.md` の「[正面の口](../../gui/client/README.ja.md#正面の口connect)」 |
| フレームワークのプロジェクト | ビルドのあるプロジェクト。部品は custom element なので、どのフレームワークのテンプレートにも書けます。 | このページの終わり |

### 部品だけ

| 要素 | すること | 属性 |
|---|---|---|
| `<vsil-status>` | 接続、実行、仮想時間、いま伝える問題 1 つ | なし |
| `<vsil-run-controls>` | Start、Stop、待たずに止める。最後の実行のファイル（最初は、サーバーを起動したときのファイル）を開始します。 | `label-*` |
| `<vsil-time-controls>` | Pause、Resume、Step、Run until、速さ | `speeds`、`step-ms` |
| `<vsil-value>` | 単位つきの値。bool は `on` と `off` の文字で出ます（CSS でランプにできます）。 | `variable`、`on`、`off` |
| `<vsil-slider>` | 変数の範囲の数。つまみを離したときに書き込みます。 | `variable` |
| `<vsil-write>` | 1 つの値を書き込むボタン | `variable`、`value`、`label` |
| `<vsil-scenario-select>`、`<vsil-scenario-result>` | 次の実行のシナリオと、そのステップと結果 | なし |
| `<vsil-timeline>` | シナリオの実行の時間の軸。中に置いた `<vehicle-sil-timeline>` に出します（なければ 1 つ足します）。その属性（`compact`、`hide`、`lang`）は自分で決めます。要素のスクリプトはページが読みます: `<script src="/timeline/vehicle-sil-timeline.js">` | なし |

変数は `<participant>.<variable>` と書き、最初のドットで切ります（`Engine.engine.throttle` は、participant `Engine` の変数 `engine.throttle`）。自分の ECU の名前を調べるには、ガイドの「手順 2: 表示する変数の名前を調べる」を見てください。名前が違うと、部品が `this variable is not in the current run: check the names (ECU and variable)` と言います。部品の一覧と、部品が守ること（接続、マークアップ、CSS を当てる `data-*` 属性）は、[gui/client/README.md](../../gui/client/README.ja.md#部品custom-element) の「部品（custom element）」にあります。

### 自分のスクリプト

`main.js` は、スクリプトの役目の 2 つを見せます。`connect()` を引数なしで呼ぶと、部品がすでに使っている接続が返ります。だからページの WebSocket は 1 本です。断られた開始、停止、時間の操作は `<vsil-status>` に出ます。断られた変数の書き込みは、その変数を出している部品の所と、自分のスクリプトが受ける `ActionError` に出ます。

| `main.js` の中 | 見せること |
|---|---|
| `watch(client.variable('Engine.engine.speed'), fn)` | 自分の流儀で描く値（メーター）。`fn` はいま呼ばれ、変わるたびに呼ばれます。 |
| `await client.variable('Engine.engine.throttle').write(100)` | 自分の操作。通らなければ `ActionError` で reject する Promise です（文は `message`、`outcome` は `refused` か `unknown`）。 |

スターターに入れていないものは、サンプルの画面がやっていることです。設定ファイルの選択、「Check」、任意の変数を選んで値を入力する欄です。設定ファイルを選ぶには、`client.low.configs()` で一覧を取り、自分のボタンで `client.run.start(intent, { file })` を呼びます（`<vsil-run-controls>` は最後の実行のファイルを開始します）。同じ接続の低い口 `client.low`（`check`、`variables.bind` など）も使えます。自分の部品の作り方は「[コンポーネントを作る（`bind`）](../../gui/client/README.ja.md#コンポーネントを作るbind)」にあります。

### ネットワークを外につなぐ

構成ファイルの `can` か `ethernet` のネットワークを実機のバスにつなぐには、`<vsil-connect network="<network>">`（次の実行でつなぐ先。最初は SIL Kit だけで、GUI サーバーの PC の先の一覧は開いたときにだけ取ります）と `<vsil-outside>`（実行中、つないだネットワーク）を足します。ネットワークの名前は構成ファイルのものなので、スターターにはどちらも置いていません。外につないだ実行は実時間を保ちます。つないだ先が実時間でないメッセージを受けても困らないときは、スクリプトから `pageIntent.update({ time_policy: 'free' })` で外せます。試験台の要求ファイルを次の実行に読み込む方法と、保存する方法は、[ガイド](../../docs/ja/howto/gui-screen.md#ネットワークを外につなぐ任意) の「ネットワークを外につなぐ（任意）」にあります。

### フレームワークのプロジェクト

ライブラリは、型つきの npm のパッケージ（`.tgz`）としても配っています（部品も入っています）。自分のプロジェクトで `npm install <その .tgz>` をして、`@vehicle-sil/gui-client` として import します。依存はありません。

| 入手元 | 場所 |
|---|---|
| ビルド済みのツール | `<tools>/client/vehicle-sil-gui-client-<version>.tgz` |
| Releases のページ | 同じファイル |
| リポジトリ | `gui/web` で `npm run pack:client` を実行します。`build/gui-client/vehicle-sil-gui-client-<version>.tgz` ができます。 |

```ts
import '@vehicle-sil/gui-client/elements'; // 部品を定義します。テンプレートに <vsil-slider ...> と書けます
import { ActionError, connect, watch } from '@vehicle-sil/gui-client';

const client = await connect(); // 部品が使う接続
watch([client.variable('Engine.engine.speed')], (view) => console.log(view.text, view.unit));
try {
  await client.variable('Engine.engine.throttle').write(60);
} catch (error) {
  if (error instanceof ActionError) console.log(error.code, error.message, error.outcome);
}
```

- `npm run pack:client` は、この例を、作ったパッケージの型で検査します（`nodenext` と `bundler` の両方のモジュール解決で）。
- どの状態も `Readable` です。`get()` と `subscribe(fn)` を持つ、読み取り専用の値です。自分のフレームワークの状態（React の `useSyncExternalStore`、Vue の `ref`、Svelte のストア、Angular の signal など）にするには、`subscribe` で変換します。
- 開発サーバー（Vite など）で、接続したまま開発するには、[docs/gui.md](../../docs/ja/gui.md#開発サーバーを使う) の「開発サーバーを使う」を見てください。
