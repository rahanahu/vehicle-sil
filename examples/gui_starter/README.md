# 自分の画面のひな形（gui_starter）

GUI サーバー（`vehicle-sil-gui`）に配らせる、自分の画面のいちばん小さい形です。クライアントのライブラリ（[gui/client](../../gui/client/README.md)）だけを使い、ビルドなしの素の HTML と ES モジュールで書いてあります。つなぐ、構成ファイルを照合する、実行を始めて止める、変数を 1 つ選んで見て書く、スライダーとランプ、までです。これを写して、自分の画面に育てます。

| ファイル | 中身 |
|---|---|
| `index.html` | 画面。スクリプトはファイルから読む（インラインのスクリプトは CSP で動かない） |
| `main.js` | ライブラリを使うところ（トークン、接続、照合、開始と停止、変数の watch と書き込み、部品と変数をつなぐ `bind`） |
| `style.css` | 見た目 |
| `vehicle-sil-gui-client.mjs` | ライブラリを 1 つにまとめたファイル。追跡しない。下の手順で置く |

## 動かす

ライブラリを 1 つにまとめたファイルを `index.html` の隣に置き、`--web` でこのディレクトリを渡して GUI サーバーを起動します。ビルド済みの道具の圧縮ファイルの `gui_starter/` には、ライブラリが置いてあるので、そのまま `vehicle-sil-gui --web <道具>/gui_starter simulation.yaml` で動きます。

```sh
(cd gui/web && npm ci && npm run build:client)    # build/gui-client/vehicle-sil-gui-client.mjs
cp build/gui-client/vehicle-sil-gui-client.mjs examples/gui_starter/
uv run vehicle-sil-gui --launcher build/<preset>/tools/launch/vehicle_sil_launch \
  --web examples/gui_starter build/<preset>/examples/ecu_main/simulation.yaml
```

ブラウザが開き、サーバーが表示した URL（`#token=` つき）でこの画面が出ます。「Check」で照合、「Start」で実行を始めます。変数は、ランチャーが Control Server を持つときに一覧に出ます（例の ECU では `Engine.engine.throttle` に数を書くと、`Engine.engine.speed` が変わります）。

「Slider and lamp」は、部品の作り方の見本です。範囲のある書ける数の変数のうち最初のものをスライダーに、bool の変数（読むだけのものがあればそれ）をランプにします。どちらも `client.variables.bind(ref, options)` が返す `state` を描いているだけで、値の状態（まだない、古い、終わった実行の値）、書けない理由（実行がない、ECU が切れている、など）、書き込みの様子（送った、受け付けた、反映した、うまくいかなかった理由）は、ライブラリが決めます。スライダーを続けて動かしても、サーバーが前の書き込みを受け取るまでは、最後の値だけを覚えて送ります。自分の部品（スイッチ、数値の入力、選択肢）も同じ形で作れます（[gui/client/README.md](../../gui/client/README.md) の「部品を作る（`bind`）」）。

自分の画面は、別のディレクトリに写して作ります（`cp -r examples/gui_starter my-screen`、`--web my-screen`）。`--web` に渡すのは、ビルドした画面の置き場所（`index.html` のあるディレクトリ）です。配る決まり（配るファイル、CSP、トークン）は [docs/gui.md](../../docs/gui.md) の「自分の画面を作る」にあります。

## TypeScript と好きな枠組みで作る

ライブラリは、型つきの npm のパッケージ（`.tgz`）にもなります。`gui/web` で `npm run pack:client` を実行すると、`build/gui-client/vehicle-sil-gui-client-<版>.tgz` ができます（リリースのページと、道具の圧縮ファイルの `client/` にもあります）。自分のプロジェクトで `npm install <その .tgz>` をすると、`@vehicle-sil/gui-client` として import できます（依存はありません）。

```ts
import { WriteError, createClient, takeToken } from '@vehicle-sil/gui-client';

const token = takeToken(window); // URL の #token= を読み、URL から消す（経路が URL を書き換える前に）
if (token === null) throw new Error('open the URL that vehicle-sil-gui printed');
// 口はサーバーの根から（経路 /settings/foo の上でも api/app が /settings/api/app にならないように）
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

// 部品 1 つと変数 1 つ: 値の状態、書けない理由、書き込みの様子が 1 つの state にまとまる
const throttle = client.variables.bind({ peer: 'Engine', name: 'engine.throttle' }, { needsRange: true });
throttle.state.subscribe(({ reading, pending, block, writeStatus }) =>
  console.log(pending ?? reading?.value, block, writeStatus.kind),
);
throttle.write(60);
```

この例は、`npm run pack:client` が、作ったパッケージの型で検査します（型の解決 `nodenext` と `bundler` の両方）。状態はどれも「読める値」（`get()` と `subscribe(fn)`）なので、枠組みの状態（React の `useSyncExternalStore`、Vue の `ref`、Svelte の store、Angular の signal など）へは、`subscribe` で写します。開発サーバー（Vite など）で作りながらつなぐ手順は、docs/gui.md の「開発中のやり方」です。
