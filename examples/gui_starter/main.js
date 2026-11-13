// 自分の画面の最小の形: クライアントのライブラリ（@vehicle-sil/gui-client）だけを使う、ビルドなしの素の JavaScript。
// つなぐ、構成ファイルを照合する、実行を始めて止める、変数を 1 つ見て書く。README.md に使い方がある。
import {
  WriteError,
  createClient,
  errorMessage,
  takeToken,
} from './vehicle-sil-gui-client.mjs';

const $ = (id) => document.getElementById(id);

// サーバーが表示した URL の #token= を読み、URL から消す（画面が URL を書き換える前に）
const token = takeToken(window);
if (token === null) {
  $('connection').textContent = 'No token: open the URL that vehicle-sil-gui printed (with #token=...).';
  throw new Error('no token');
}

const client = createClient({ baseUrl: document.baseURI, token });
const show = (id, text) => ($(id).textContent = text);
const fail = (error) => show('error', errorMessage(error));

// つながり方
client.connection.state.subscribe((state) => show('connection', `connection: ${state}`));
client.session.rejected.subscribe((rejected) => {
  if (rejected) show('connection', 'The server refused the token: open the URL it printed again.');
});

// 構成ファイルの一覧（起動のときに渡したものを選んでおく）
const app = await client.loadApp();
client.connect();
const { files } = await client.configs();
for (const file of files) $('config').add(new Option(file.path, file.path));
if (app.initial_file) $('config').value = app.initial_file;

// 照合、開始、停止
const file = () => $('config').value;
$('check').onclick = async () => {
  show('error', '');
  try {
    const check = await client.check({ file: file(), disable: [] });
    show('check-result', `${check.result} (errors ${check.errors}, warnings ${check.warnings})`);
  } catch (error) {
    fail(error);
  }
};
$('start').onclick = () => {
  show('error', '');
  client.run.start({ file: file(), disable: [], speed: null, start_paused: false }).catch(fail);
};
$('stop').onclick = () => client.run.stop().catch(fail);

// 実行の状態（セッションの写し）
client.session.session.subscribe((session) => {
  show('phase', session?.phase ?? '-');
  show('result', session?.result?.result ?? '-');
});

// 変数: 一覧から 1 つ選んで見る、書く
let variables = [];
let watch = null;
client.variables.state.subscribe((state) => {
  show('variables', state.reason ? `${state.status}: ${state.reason}` : state.status);
  const next = state.peers.flatMap((peer) => peer.variables.map((info) => ({ peer: peer.name, info })));
  const key = (v) => `${v.peer}.${v.info.name}`;
  if (next.map(key).join() === variables.map(key).join()) return;
  const chosen = $('variable').value;
  variables = next;
  $('variable').replaceChildren(...variables.map((v) => new Option(key(v), key(v))));
  if (variables.some((v) => key(v) === chosen)) $('variable').value = chosen;
  watchChosen();
});

function chosen() {
  return variables.find((v) => `${v.peer}.${v.info.name}` === $('variable').value) ?? null;
}

function watchChosen() {
  watch?.close(); // 使い終わった watch は必ず閉じる
  watch = null;
  const v = chosen();
  show('value', '-');
  if (!v) return;
  const ref = { peer: v.peer, name: v.info.name };
  watch = client.variables.watch([ref], { periodMs: 100 });
  watch.values.subscribe(() => {
    const reading = watch?.value(ref);
    if (reading) show('value', `${reading.value} ${v.info.unit} (at ${reading.simMs} ms)`);
  });
}
$('variable').onchange = watchChosen;

$('write').onclick = async () => {
  const v = chosen();
  if (!v) return;
  const text = $('new-value').value.trim();
  // 型に合わせる: bool は true / false、列挙は項目の名前か数、ほかは数
  const item = v.info.enum?.find((e) => e.name === text);
  const value = v.info.type === 'bool' ? text === 'true' : item ? item.value : Number(text);
  show('write-result', 'sending…');
  try {
    const operation = await client.variables.write({ [v.peer]: { [v.info.name]: value } });
    show('write-result', `applied at ${operation.peers[v.peer]?.appliedAtMs} ms`);
  } catch (error) {
    // 反映されたかわからない（no-answer）書き込みは、確かめずに送り直さない
    show('write-result', error instanceof WriteError ? `${error.kind}: ${error.message}` : errorMessage(error));
  }
};
