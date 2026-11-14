# 自分の画面のテンプレート（gui_starter）

GUI サーバー（`vehicle-sil-gui`）に配信させる自分の画面の、最小の形です。クライアントのライブラリ（[gui/client](../../gui/client/README.md)）だけを使い、ビルドなしの素の HTML と ES モジュールで書いてあります。これをコピーして、自分の画面に書き換えます。変数を 1 つ表示する画面にするまでの手順は、ガイド [ブラウザの GUI に自分の画面を作る](../../docs/howto/gui-screen.md) にあります。

| ファイル | 内容 |
|---|---|
| `index.html` | 画面です。スクリプトはファイルから読み込みます（インラインのスクリプトは CSP で実行されません）。 |
| `main.js` | ライブラリを使うスクリプトです（トークン、接続、照合、開始と停止、変数の watch と書き込み、コンポーネントと変数を接続する `bind`）。 |
| `style.css` | 見た目です。 |
| `vehicle-sil-gui-client.mjs` | ライブラリを 1 つにまとめたファイルです。 |

画面にあるのは、接続の状態、設定ファイルの照合、実行の開始と停止、変数を 1 つ選んで値を見て書き込む操作、スライダーとランプです。

## 実行する

`--web` でこのディレクトリを渡して、GUI サーバーを起動します。ライブラリのファイル（`vehicle-sil-gui-client.mjs`）が `index.html` の隣に必要で、その用意のしかたが 2 通りあります。

| 使うもの | ライブラリのファイル | 起動するコマンド |
|---|---|---|
| ビルド済みのツール | `<ツール>/gui_starter/` に最初から入っています。 | `vehicle-sil-gui --web <ツール>/gui_starter simulation.yaml` |
| リポジトリ | Git で管理していないので、下のコマンドでビルドしてコピーします。 | 下のコマンドの 3 行目 |

リポジトリでは、ルートで次を実行します。

```sh
(cd gui/web && npm ci && npm run build:client)    # build/gui-client/vehicle-sil-gui-client.mjs
cp build/gui-client/vehicle-sil-gui-client.mjs examples/gui_starter/
uv run vehicle-sil-gui --launcher build/<preset>/tools/launch/vehicle_sil_launch \
  --web examples/gui_starter build/<preset>/examples/ecu_main/simulation.yaml
```

### 確認

ブラウザが開き、サーバーが表示した URL（`#token=` つき）でこの画面が表示されます。

| 操作 | 起きること |
|---|---|
| 「Check」を押す | 設定ファイルを照合し、結果を表示します。 |
| 「Start」を押す | 実行を開始します。 |
| 「Variable」で変数を選ぶ | その変数の値を表示します。 |
| 「New value」に値を入力して「Write」を押す | 変数に書き込みます。 |

変数は、ランチャーが Control Server を持つときに一覧に表示されます。例の ECU では、`Engine.engine.throttle` に数を書き込むと、`Engine.engine.speed` が変わります。

## スライダーとランプ

「Slider and lamp」は、コンポーネントの作り方の見本です。

| コンポーネント | 対象の変数 |
|---|---|
| スライダー | 範囲のある書き込める数の変数のうち、最初のもの |
| ランプ | bool の変数（読み取り専用のものがあればそれ） |

どちらも `client.variables.bind(ref, options)` が返す `state` を描いているだけで、次のことはライブラリが決めます。

| `state` が持つもの | 例 |
|---|---|
| 値の状態 | まだない、古い、終わった実行の値 |
| 書き込めない理由 | 実行がない、ECU が切断されている |
| 書き込みの状態 | 送った、受け付けた、反映した、失敗した理由 |

スライダーを続けて動かしても、サーバーが前の書き込みを受け取るまでは、最後の値だけを覚えて送ります。自分のコンポーネント（スイッチ、数値の入力、選択肢）も同じ形で作れます（[gui/client/README.md](../../gui/client/README.md) の「コンポーネントを作る（`bind`）」）。

## 自分の画面にする

自分の画面は、別のディレクトリにコピーして作ります。

| 使うもの | コピーするコマンド | 起動するときの引数 |
|---|---|---|
| ビルド済みのツール | `cp -r <ツール>/gui_starter my-screen` | `--web my-screen` |
| リポジトリ | `cp -r examples/gui_starter my-screen` | `--web my-screen` |

`--web` に渡すのは、ビルドした画面の置き場所（`index.html` のあるディレクトリ）です。配信のルール（配信するファイル、CSP、トークン）は [docs/gui.md](../../docs/gui.md) の「自分の画面のリファレンス」にあります。

## TypeScript とフレームワークで作る

ライブラリは、型つきの npm のパッケージ（`.tgz`）にもなっています。自分のプロジェクトで `npm install <その .tgz>` をすると、`@vehicle-sil/gui-client` として import できます（依存はありません）。

| 入手先 | 場所 |
|---|---|
| ビルド済みのツール | `<ツール>/client/vehicle-sil-gui-client-<バージョン>.tgz` |
| リリースのページ | 同じファイル |
| リポジトリ | `gui/web` で `npm run pack:client` を実行すると、`build/gui-client/vehicle-sil-gui-client-<バージョン>.tgz` ができます。 |

```ts
import { WriteError, createClient, takeToken } from '@vehicle-sil/gui-client';

const token = takeToken(window); // URL の #token= を読み、URL から消す（ルーティングが URL を書き換える前に）
if (token === null) throw new Error('open the URL that vehicle-sil-gui printed');
// API の URL はサーバーのルートから作る（画面のパス /settings/foo の上でも api/app が /settings/api/app にならないように）
const client = createClient({ baseUrl: new URL('/', location.href).href, token });
await client.loadApp();
client.connect();
client.session.session.subscribe((session) => console.log(session?.phase, session?.result?.result));

const ref = { peer: 'Engine', name: 'engine.speed' };
const watch = client.variables.watch([ref], { periodMs: 100 });
watch.values.subscribe(() => console.log(watch.value(ref)?.value));
try {
  await client.variables.write({ Engine: { 'engine.throttle': 50 } });
} catch (error) {
  if (error instanceof WriteError) console.log(error.kind, error.message);
}

// コンポーネント 1 つと変数 1 つ: 値の状態、書き込めない理由、書き込みの状態が 1 つの state にまとまる
const throttle = client.variables.bind({ peer: 'Engine', name: 'engine.throttle' }, { needsRange: true });
throttle.state.subscribe(({ reading, pending, block, writeStatus }) =>
  console.log(pending ?? reading?.value, block, writeStatus.kind),
);
throttle.write(60);
```

- この例は、`npm run pack:client` が、作ったパッケージの型で検査します（型の解決 `nodenext` と `bundler` の両方）。
- 状態はどれも `Readable`（`get()` と `subscribe(fn)` を持つ、読み取り専用の値）です。フレームワークの状態（React の `useSyncExternalStore`、Vue の `ref`、Svelte の store、Angular の signal など）へは、`subscribe` で変換します。
- 開発サーバー（Vite など）で作りながら接続する手順は、[docs/gui.md](../../docs/gui.md) の「開発サーバーを使う」です。
