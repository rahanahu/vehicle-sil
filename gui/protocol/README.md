# gui/protocol

GUI サーバー（`gui/server`）の HTTP と WebSocket の口の、公開の契約です。私たちの画面（`gui/web`）も、クライアントのライブラリ（`gui/client`）を通して、この口だけを使います。自分の画面や道具を作るときも、この文書と例に従えば、同じことができます（作り方は [docs/gui.md](../../docs/gui.md) の「自分の画面や道具を作る」）。

`examples/` の JSON は、サーバーの試験（`gui/server/tests/test_protocol_examples.py`）が偽のランチャーとの実際のやりとりから作り、毎回照合します。試験が作らないファイルが `examples/` に残っていても試験は落ちます。形を変えたら `UPDATE_PROTOCOL_EXAMPLES=1 scripts/verify-gui.py --skip-web` で書き直し、ライブラリの型と読み取り（`gui/client/src/protocol.ts`）と、その単体試験（同じファイルを読む）を合わせます。プロセス ID、時刻、インスタンス、この PC のパス、決まらない数は、決まった値に置き換えてあります（`t_ms` は `seq × 10`）。

## 版と互換

口の版は `vehicle_sil_gui`（今は 1）です。サーバーは `GET api/app` の応答、WebSocket の `hello`、`--json` で起動したときの 1 行に書きます。

- **足すのは互換**: 欄、`type`、`code`、事象、要求、認証のメッセージの任意の欄を足しても、版は上げません。読む側は、知らない欄、知らない `type`、知らない事象（`event`）、知らない `code`、値の集合が増えうる欄（`phase`、`state`、`result` など）の知らない値を、無視するか、そのまま通します。
- **消す、意味を変えるのは版を上げる**: ここにある欄や要求を消す、型や意味を変える、送る順の決まりを変えるときは、`vehicle_sil_gui` を上げます。読む側は、知らない版の `api/app` と `hello` を読まずに、版が違うと知らせます。待っても直らないので、つなぎ直しません（ライブラリは `api/app` で `ProtocolError`、`hello` で `session.incompatible` を立てて止まる）。
- **読み飛ばすものと、受け直すもの**: 知らない `type`、欄、事象、値は、読み飛ばすか、そのまま通すだけです（足すのは互換なので）。一方、WebSocket のメッセージが JSON として読めない、または `hello` と `session` の要約の芯（`id`、`state`、`launcher`、`stop`）が読めないときは、何が抜けたかわからないので、つなぎ直して `hello` から受け直します（要約が正。続けて起きるたびに、つなぎ直すまでの間を延ばす）。
- **頼んだときだけ変わる形**: 前の形のままでは重いものは、クライアントが認証のメッセージで頼んだときだけ形を変えます（`session_without_check`）。頼まないクライアントには、いままでの形で送ります。
- ランチャーの事象（`event` の中身）は、ランチャーの `run --events` の形そのままで、その版（`hello` の事象の `vehicle_sil_events`）はランチャーが決めます（[docs/simulation-config.md](../../docs/simulation-config.md) の「機械から使う」）。
- 誤りの `code` の名前は変えません（足すことはあります）。`message` は人向けの英語の 1 文で、文言は変わることがあります。機械は `code` で分けます。

## 認証と Origin

- サーバーは `127.0.0.1` だけで待ち、`Host` は `127.0.0.1:<port>`（開発のときは `--dev-origin` のホストも）だけを受け付けます。
- トークンは起動のたびに作られ、サーバーが表示した URL の `#token=` の後ろにだけあります。HTTP は `Authorization: Bearer <token>`、WebSocket は最初のメッセージ（`auth`）で送ります。
- `POST` と WebSocket の握手は、`Origin` がサーバー自身（`http://127.0.0.1:<port>`。開発のときは `--dev-origin`）のものだけを受け付けます。ブラウザは自分で付けます。ブラウザの外（Node、Python など）から使うときは、サーバー自身の origin を `Origin` に書きます（`examples/gui_client/run.mjs`）。
- CORS は許しません。別の origin のページからは使えません。

## 使い方の例

- 画面を使わない最小のクライアント: [examples/gui_client/run.mjs](../../examples/gui_client/run.mjs)（素の JavaScript、依存なし、Node 22 以上）。照合して実行を始め、事象を読み、終わりを待ちます。`scripts/verify-gui.py` が、偽のランチャーを相手に動かして確かめます。
- TypeScript のクライアントのライブラリ: [gui/client](../client/README.md)。接続、再接続、要約と事象の当て方、時刻の命令、ECU の変数（watch、書き込みと結果）を持ちます。
- ECU の変数を読み書きする例: [examples/gui_client/variables.mjs](../../examples/gui_client/variables.mjs)（ライブラリを使う）。

## 考え方

読む側は、知らない欄、知らない `type`、知らない事象（`event`）、知らない `code` を無視して通します。ランチャーの事象は、ランチャーが書いた形のまま届きます。`heartbeat` もそのまま通します（画面は読み飛ばします。ランチャーが動いているかはサーバーが見て、要約の `launcher.silent_s` で知らせます）。participant の知らない `state`（`failed` など、ランチャーが足すもの）も、そのまま通して要約に重ねます。サーバーが形を確かめるのは、事象の一部の欄（下の `bad-event`）だけです。ほかの欄は、なくても型が違っても、そのまま通して要約に重ねます（`ws-loose-fields.json`）。読む側は要約と事象を部分ごとに読み、読めない部分だけを読み飛ばします。

- **要約が正**。実行の要約（下の「要約」）に、画面を作り直すのに要るものを全部入れます。画面は `hello` と `session` を受けたら、手元の状態をその要約で置き換えます。
- 事象は届くたびに `event` で送ります。画面は `hello` の後に届いた事象を要約に当てて進めます。
- つなぎ直したとき（`hello` の後）に再送するのは、log の行（`from_seq` 以後で、サーバーが覚えている分）だけです。段階や participant の事象は再送しません（要約に入っているので）。
- サーバーが自分で持つ状態は、ランチャーから届いたものと、サーバーがランチャーにしたこと（起動、停止の依頼）だけです。

## HTTP

どの `api/` にも `Authorization: Bearer <token>` が要ります（token は、サーバーが表示した URL の `#token=` の後ろ）。`Host` は `127.0.0.1:<port>`（開発のときは `--dev-origin` のホストも）だけを受け付けます。`POST` は `Origin` がサーバー自身（開発のときは `--dev-origin`）のものだけを受け付け、本文があれば `Content-Type: application/json` が要ります。応答は JSON で、失敗は `{"error": {"code", "message", "detail"}}` です（`code` は機械が読む名前で、下の表にあります。`message` は人向けの英語の 1 文で、直し方を含みます）。

| 要求 | 応答 | 例 |
|---|---|---|
| `GET api/app` | サーバーの版、`instance`（起動ごとに変わる）、root、`initial_file`、ランチャー（`usable`、`problems`、`version --json` の内容）、変数を読み書きできるか（`variables`: `available`、できない理由 `reason`）、上限（`limits`: `speed_min`、`speed_max`、`step_ms_max`、変数の `watches`、`watch_variables`、`watch_period_ms_min`、`watch_period_ms_max`、`writes_per_request`、`write_at_ms_max`、`write_timeout_s` など） | `app.json` |
| `GET api/configs` | root の下の `.yaml` / `.yml`（`path` は root からの相対で `/` 区切り、`modified`、`size`、`simulation`: 先頭に `vehicle_sil_simulation` があるか）、`truncated` | `configs.json` |
| `POST api/check` `{file, disable}` | `{file, disable, exit_code, check}`。`check` は `check --json` の結果そのまま。照合が失敗しても 200 | `check-*.json` |
| `POST api/run` `{file, disable, speed, start_paused}` | `{session}`（始めた実行の要約） | `run.json` |
| `POST api/stop` `{force}` | `{session}`（今の要約。実行がなければ `null`） | `stop*.json` |
| `POST api/time` `{command, ms, value}` | `{session_id, command}`（ランチャーに送った命令） | `time.json` |
| `POST api/write` `{session, writes, at_ms, late}` | `{session_id, operation}`（操作の番号。結果は WebSocket の `operation`） | `write.json` |
| `GET api/session` | `{session}`（なければ `null`） | `session-*.json` |

- `disable` に書けるのは、そのファイルの照合の結果にある participant 名だけです（`unknown-participant`）。
- `speed`（`null`、`"max"`、数）と `start_paused`（真偽）は、`api/app` の `launcher.version.features` に `speed`、`start-paused` があるときだけ値を送ります。ないときは欄を省くか `null` / `false` を送ります（ほかの値は 400 `unsupported`）。`speed` の数の範囲は `limits.speed_min` から `limits.speed_max` まで（ランチャーと同じ）。`null` は構成ファイルの `speed` のまま（ランチャーに `--speed` を渡さない）です。
- **時刻の命令**: `api/time` の本文は `{"command": "pause"}`、`{"command": "resume"}`、`{"command": "step", "ms": <1 から limits.step_ms_max の整数>}`、`{"command": "speed", "value": <"max" か speed と同じ範囲の数>}` のどれかです（ほかの欄は 400 `invalid-field`）。サーバーは、動いている実行のランチャーの標準入力に 1 行で書き、応答を待たずに返します（止まっているランチャーで要求が固まらないように）。受け付けたかどうかと理由は、ランチャーの `command` の事象（`accepted`、`message`、`step` の実際に進む `ms`）で、効いたことは `time` の事象で届きます（要約の `last_command` と `time` にも残ります）。動いている実行がなければ（終わった、始まりかけ、ランチャーの入力が閉じた）409 `not-running`、その実行のランチャー（始めたときの `version --json`）の `features` に `time-commands` がなければ 400 `unsupported` です。ランチャーが入力を読まずに詰まっているときも、要求は長くて約 2 秒で返ります（命令はランチャーの入力に残ります）。命令の意味（一時停止は刻みの境目で効く、ステップ送りは一時停止の間だけ、など）はランチャーの決まりです（[docs/simulation-config.md](../../docs/simulation-config.md) の「標準入力の命令」）。
- **開始**: 同時に送られた `api/run` は、1 つだけが始めます。ほかは、実行が始まりかけているか動いている間、409 `already-running` です。応答の要約を、画面はその場で当てます（後から届く `session` も同じ内容です）。
- **照合**: 同時の `api/check` は断らず、順に 1 つずつ実行します。待っている要求が多すぎるか、待ちが照合の時間の上限（`api/app` の `limits.check_timeout_s`）を過ぎたら 503 `check-busy` です。`api/run` も、participant 名を確かめるために同じ順番で照合することがあります。
- **停止**: 本文は `{"force": 真偽}`（省略は `false`。本文なしで `Content-Type` もない要求も `false`）。`force: false` は「順に止める」（ランチャーの標準入力に `{"command":"stop"}`）で、すでに頼んであれば何もしません。`force: true` は「待たずに止める」（まだ頼んでいなければ `stop` を 2 回、頼んであればもう 1 回）で、すでに頼んであれば何もしません。どちらも、実行がない、または終わっているときは 409 にせず、今の要約を返します（何度呼んでも同じ結果）。実行が始まりかけている間（`api/run` が応答する前）の停止は、ランチャーが起動したところで届けます（応答の要約は前の実行のもの。始まった実行の要約の `stop` に出ます）。止めた経過は、要約の `stop` と、ランチャーの `command` / `phase` の事象で見えます。標準入力を読まないランチャーは、これでは止まりません（サーバーを止めると、シグナルで止めます）。
- **書き込み**: 下の「ECU の変数」。
- サーバーが終わり始めたら、`api/run`、`api/check`、`api/write` は 503 `shutting-down` です。

## 誤りの `code`

画面が見るコードです。名前は変えません（足すことはあります）。

| HTTP | `code` | いつ |
|---|---|---|
| 400 | `invalid-json` | 本文が JSON のオブジェクトでない |
| 400 | `invalid-field` | 欄の型か範囲が違う（`speed`、`start_paused`、`force`、`disable`、`api/time` の `command`、`ms`、`value`、`api/write` の `writes`、`at_ms`、`late` と、変数の型か範囲に合わない値。`detail` に `{peer, name}`） |
| 400 | `invalid-path` | `file` が root からの相対パスの文字列でない（空、長すぎる、`\`、NUL、`:`、対のないサロゲート） |
| 400 | `outside-root` | 絶対パス、`..`、root の外へのシンボリックリンク |
| 400 | `not-a-configuration` | `.yaml` / `.yml` の通常のファイルでない |
| 400 | `unknown-participant` | `disable` の名前が照合の結果にない |
| 400 | `unsupported` | ランチャーが `speed` / `start-paused` を持たないのに値を送った。`time-commands` を持たないのに `api/time` を送った。`control-server` を持たないのに `api/write` を送った |
| 400 | `unknown-variable` | `api/write`: その participant か変数が、実行の変数の一覧にない（`detail` に `{peer, name}`） |
| 400 | `not-writable` | `api/write`: 出力の変数（`detail` に `{peer, name}`） |
| 400 | `write-refused` | `api/write`: ランチャーの Control Server が断った（文言に理由） |
| 401 | `unauthorized` | トークンがないか違う（`WWW-Authenticate: Bearer` つき） |
| 403 | `forbidden-host` | `Host` が `127.0.0.1:<port>` でない |
| 403 | `forbidden-origin` | `Origin` がサーバー自身でない（`POST` と WebSocket は `Origin` が要る） |
| 403 | `cors-not-allowed` | `OPTIONS`（CORS は許さない） |
| 404 | `file-not-found` | `file` が root の下にない |
| 404 | `not-found` | そういう API や画面のファイルはない |
| 405 | `method-not-allowed` | 画面のファイルに `GET` 以外 |
| 409 | `already-running` | 実行が始まりかけているか動いている |
| 409 | `not-running` | `api/time`、`api/write`: 動いている実行がない |
| 409 | `session-changed` | `api/write`: `session` が、動いている実行の id と違う（実行が替わる間に送った書き込み）。何も書いていない。例 `error-session-changed.json` |
| 409 | `variables-not-ready` | `api/write`: 変数の機能の状態が `ready` でない（文言に、要約の `variables.reason`） |
| 413 | `too-large` | 本文が 64 KiB を超える |
| 415 | `unsupported-media-type` | 本文があるのに `Content-Type: application/json` でない |
| 500 | `internal` | サーバーの不具合（例はない。起きたら標準エラーを見る） |
| 502 | `launcher-start` | ランチャーを起動できない |
| 502 | `launcher-output` | `check --json` が照合の結果を書かなかった |
| 502 | `write-failed` | `api/write`: 書き込みが Control Server に届かなかった（切れた） |
| 503 | `launcher-unusable` | ランチャーが使えない（`api/app` の `launcher.problems` に理由） |
| 503 | `check-busy` | 照合の待ちが多すぎるか、待ちが長すぎる |
| 503 | `write-busy` | `api/write`: 前の書き込みが Control Server の答えを待っていて、順番が来なかった（送っていない） |
| 503 | `shutting-down` | サーバーが終わろうとしている |
| 503 | `too-many-connections` | WebSocket の数の上限（握手の応答） |
| 504 | `check-timeout` | 照合が時間の上限までに終わらなかった（止めた） |
| 504 | `write-timeout` | `api/write`: Control Server が時間内（`limits.write_timeout_s`）に答えなかった。受け取られたかもしれない（送り直さない） |

例は `error-<code>.json` です（`internal` と、`write-refused`、`write-failed`、`write-busy`、`write-timeout` を除く）。`error-invalid-watch.json` は WebSocket の `error` の例です。`stop.json`、`stop-force.json`、`stop-ended.json` は停止の応答、`time.json` は時刻の命令の応答です。

## WebSocket（`ws`）

つないだら最初に `{"type": "auth", "token": "...", "session": <id>, "from_seq": <n>, "session_without_check": true}` を送ります（`session` と `from_seq` は、再接続で持っている分を言うときだけ。0 以上の整数でなければ、ないものとして扱います。`session_without_check` は任意で、`true` なら、この接続への `session` の通知の要約から `check` の欄を省きます。`hello` の要約には入り、照合の結果は `check` の事象で届くので、手元の値を使い続けます。照合の結果は大きく、実行の間に変わらないためです。例は `ws-without-check.json`）。トークンが違えば `error`（`code: unauthorized`）の後に、5 秒以内に `auth` が届かなければそのまま、閉じるコード 4401 で閉じます（理由の文は `unauthorized` か `auth-timeout`）。サーバーが終わるときは 1001 で閉じます。握手の段の拒否（`Host`、`Origin`、`too-many-connections`）は、ブラウザの JavaScript からは理由が見えません（1006 になります）。画面は、握手に失敗したら `GET api/app` を 1 回呼んで理由を知ります。認証した接続は 32 本まで（それを超える握手は 503 `too-many-connections`。握手の後、認証までの間に埋まったときは、閉じるコード 1013、理由の文 `too-many-connections`）。認証前の接続は別に 16 本までで、あふれたら最も古い認証前の接続を、閉じるコード 1013（理由の文は `too-many-unauthenticated`）で閉じて場所を空けます。1013 で閉じられた画面は、つなぎ直します。

認証の後に届くものは、この順です。

1. `hello`: `instance`、`session`（要約。なければ `null`）、`replay`（`from_seq`: 再送の始まり。`null` は最初から）。画面は状態をこの要約で作り直す。
2. `dropped`（あれば）: `from_seq` 以後の log の行のうち、サーバーが覚えていないので送れない数（source ごと）。
3. `event`: 覚えている log の行のうち `from_seq` 以後のものを、`seq` の順に。
4. `synced`: `{session, last_seq}`。再送の終わり。
5. 以後は、届くたびに `event`、`session`、`error`、`dropped`、`resync`、`variables`、`values`、`operation`。

認証の後に画面が送れるのは、変数の `watch` と `unwatch` です（下の「ECU の変数」）。JSON のオブジェクトで `type` が文字列でないものは `error`（`code: invalid-message`）、知らない `type` は読み飛ばします。どちらでも接続は続きます。

`session` の `id` が `hello` の要約と違う、または `from_seq` が今のセッションのものでないときは、最初からの再送になります（`replay.from_seq` が `null`）。

| `type` | 内容 | 例 |
|---|---|---|
| `hello` | 上の 1 | `ws-*.json` の最初 |
| `event` | `{session, event}`。ランチャーの事象そのまま（サーバーが形を確かめて通したもの） | `ws-pass.json`、`ws-log-dropped.json`、`ws-time.json`（時刻の命令）、`ws-late.json`（遅れ） |
| `synced` | `{session, last_seq}` | `ws-pass.json` |
| `session` | `{session}`（要約）。実行を始めたとき、止めるよう頼んだとき、実行が終わったとき（`launcher.running` が `false`、`state` が `ended`。ランチャーの最後の事象の後）、ランチャーが黙ったとき（`launcher.silent_s` が変わったとき）と、また書き始めたとき。`session_without_check` を頼んだ接続には `check` の欄がない | `ws-pass.json`、`ws-without-check.json` |
| `error` | `{session, code, message, detail}`。サーバーが気づいたこと（下の表）。要約の `problems` にも残る | `ws-bad-lines.json` |
| `dropped` | `{session, reason, log, at_least, events}`（`ws-slow-client.json` の数は例）。この画面に送らなかったものの数。`log` は source ごとの log の行の数（送らなかった `log_dropped` の数も、その source に足す）、`at_least` はその数が下限でしかない source（もっと落ちているかもしれない）、`events` はほかの事象の数。source の名前は要約の `log` と同じです（65 個目からの source は `*`）。`reason` が `not-kept` はサーバーが覚えていない（再送の前）、`slow-client` は画面の受け取りが遅く、送る分から落とした。画面は、その source のログのその位置に「ここで N 行が落ちた」を出す | `ws-dropped.json`、`ws-slow-client.json` |
| `resync` | `{reason, message}`。画面の受け取りが遅く、落とせない知らせまであふれた。すぐ後に `hello` から送り直す | `ws-resync.json` |
| `variables` | `{session, variables}`。その実行の変数の機能の状態か一覧（要約の `variables` と同じ形）。実行が始まった直後（その `session` の通知のすぐ後）と、変わったとき | `ws-variables.json`、`ws-variables-failed.json` |
| `values` | `{id, session, sim_ms, paused, peers}`。watch している変数の最後の値（下の「ECU の変数」） | `ws-variables.json` |
| `operation` | `{session, seq, state, done, peers}`。書き込みの結果が変わった（下の「ECU の変数」）。全部の接続へ。落とさない | `ws-variables.json` |

受け取りが遅い画面には、まず log の行、`log_dropped`、知らない事象を落とし（`dropped`）、`time` は最後の 1 つにまとめ、`heartbeat` は数えずに落とします。それでも追いつかないときだけ `resync` です。`values` は、送るときに、そのときの最後の値で作ります（待ち行列には watch ごとに 1 つ分の場所だけがあり、送れない間の値はためません）。`variables` と `operation` は落としません。

### `error` の `code`（サーバーの知らせ）

| `code` | 意味 |
|---|---|
| `unauthorized` | 認証の誤り（`session` は `null`。この後 4401 で閉じる） |
| `invalid-message` | 画面が送ったものが、JSON のオブジェクトで `type` を持つものでない（`session` は `null`。その接続にだけ送る。接続は続く） |
| `invalid-watch`、`too-many-watches` | `watch` の形か範囲が違う、接続の watch が上限を超えた（`session` は `null`、`detail.id` に watch の id。その接続にだけ送る。接続は続く） |
| `launcher-output` | ランチャーが事象でない行を書いた（JSON のオブジェクトでない、`event` がない、`seq` が 0 から 2^53 までの整数でない）。`1e999` のような有限でない数を含む行も、ここに入る |
| `bad-event` | 知っている事象の欄の型が違う（`t_ms` が 0 から 2^53 までの整数でない、`log_dropped` の `count` と `sources` の値が数でない、を含む）か、サーバーがその事象を中継できなかった。その事象は捨てる（中継も要約への反映もしない） |
| `line-too-long` | 1 行が上限（16 MiB）を超えた。捨てる |
| `seq-gap`、`seq-order` | `seq` が飛んだ、戻った |
| `no-hello` | 最初の事象が `hello` でない |
| `events-format` | `hello` の `vehicle_sil_events` が、このサーバーの読む版と違う |
| `launcher-stderr` | ランチャーが標準エラーに書いた行。最初の 20 行はすぐに送る。それより後は最後の 30 行だけを、実行が終わったときに送る。間で落とした行があれば、その前に 1 つ（`detail.skipped`: 落とした行の数） |
| `kept-limit` | 覚える量の上限を超えたので、要約に一部しか入れなかった（`detail.what`: `check`、`result`、`findings`、`participants`、または 4 KiB を超えた事象の種類 `phase`、`time`、`registry`、`command`、`participant`）。4 KiB を超えた事象は、大事な欄（下の「上限」）を残し、ほかの欄は入る分だけ入れて、入らない欄の名前を `omitted_fields` に書く。種類ごとに 1 回 |
| `no-result` | ランチャーが `result` を書かずに終わった（`detail.exit_code`） |
| `output-left-open` | ランチャーは終わったが、ランチャーが起動したプロセスが出力を開いたままなので、残りを読まずに終えた |
| `stop-not-delivered` | 停止の命令をランチャーの標準入力に書けなかった |
| `stop-escalated` | サーバーが終わるとき、ランチャーが時間内に止まらず、シグナルで止めるよう頼んだ |
| `launcher-silent` | ランチャーのプロセスが動いているのに、結果の前に `--silent-after`（既定 5 秒）より長く 1 行も書かない（ランチャーは心拍 `heartbeat` を 1 秒ごとに書く）。止まっているかもしれない。黙るたびに 1 回（`detail.seconds`）。続く間は要約の `launcher.silent_s` が増え、また書いたら `null` に戻る |

## 要約（`session`）

| 欄 | 内容 |
|---|---|
| `id` | 実行の番号（サーバーの起動ごとに 1 から） |
| `file`、`disable`、`speed`、`start_paused` | 始めたときの指定（`speed` の `null` は構成ファイルのまま。いまの速さは `time.speed`） |
| `started_at`、`ended_at` | 時刻（UTC、ISO 8601）。`ended_at` は終わるまで `null` |
| `state` | `running` か `ended`。`ended` は、ランチャーが終わり、その出力を読み終えた（または `output-left-open` で打ち切った）。その間（長くて 2 秒）は、`api/session` で `launcher.running: false` と `running` が見えることがある |
| `launcher` | `{pid, running, exit_code, signal, silent_s}`。`running` はランチャーのプロセスが動いているか。止められるかどうかはこれで決める。終わったら `exit_code`（シグナルで終わったら `null` で、`signal` にその番号。POSIX だけ）。`silent_s` は、動いているランチャーが何も書かない秒数（`--silent-after` を超えたときだけ、その倍数ごとに更新。書けば `null`） |
| `stop` | サーバーがランチャーに頼んだ停止: `none`、`requested`（順に止める）、`forced`（待たずに止める） |
| `phase` | 最後の `phase` 事象の段階（まだなければ `null`） |
| `last_seq`、`last_t_ms` | 最後に受け取った事象の `seq` と `t_ms` |
| `time` | 最後の `time` 事象（そのまま。なければ `null`）。仮想時間 `sim_ms`、いまの速さ `speed`、一時停止 `paused`、実際の速さ `actual_speed`、遅れの数 `late_steps`、`max_late_ms`、`slipped_ms`、`late_ms`（古いランチャーは `sim_ms` だけ）。途中から開いた画面も、これで今の時刻の状態がわかる。大きすぎた欄があれば `omitted_fields` |
| `check` | `check` 事象の `check`（照合の結果を丸ごと。participant の `required`、`enabled`、`variables` を含む）。まだなければ `null`。`session_without_check` を頼んだ接続への `session` の通知には、この欄がない（`hello` と HTTP の応答にはある） |
| `registry` | 最後の `registry` 事象から `event`、`seq`、`t_ms` を除いたもの（なければ `null`）。大きすぎた欄があれば `omitted_fields` |
| `participants` | participant ごとの今の状態。`participant` 事象の欄（`event`、`seq`、`t_ms` を除く）を名前ごとに重ねたもの（`name`、`state`、`pid`、`exit_code`、`signal` など。`failed` の `reason` は、後で `ended` が来ても残る）。ランチャーの `control_link` 事象（変数を読み書きする口のつながり方）は、その participant の `control_link` の欄に、`name` を除いた欄のオブジェクト（`{"state", "variables"}` など。`state` の値はランチャーのまま）として重ねる（例は `ws-control-trouble.json`）。初めて現れた順。`required` と `enabled` は `check` を見る。`stop_requested` の事象が来た participant には、サーバーが `stop_requested: true` を付け、後の状態でも残す（止めるよう求めた後の終わりを、失敗と区別するため）。大きすぎて入れなかった欄があれば、その名前の配列 `omitted_fields`（後の事象でその欄が届けば消える。例は `ws-large-fields.json`） |
| `participants_omitted` | 上限（256 人）を超えたので覚えていない participant の事象の数 |
| `findings`、`findings_omitted` | `finding` 事象（`event`、`seq`、`t_ms` を除く）と、上限を超えて覚えていない数 |
| `last_command` | 最後の `command` 事象から `event`、`t_ms` を除いたもの（`seq`、`command`、`source`、`accepted`、`message`、受け付けた `step` の `ms`。なければ `null`）。`seq` で、新しい答えか、前と同じ答えかがわかる。大きすぎた欄があれば `omitted_fields` |
| `result` | `result` 事象の `result`（なければ `null`） |
| `variables` | ECU の変数の機能の状態、participant ごとの変数、最近の操作（下の「ECU の変数」）。`hello` の要約と HTTP の応答にある。**`session` の通知の要約には、この欄がない**（大きく、めったに変わらないので、変わったときに `variables` の通知で送る。同じ実行の `session` の通知では、手元の値を使い続ける。別の実行の `session` の通知の直後には、その実行の `variables` の通知が届く） |
| `log` | source ごとの `{kept, dropped}`: サーバーが覚えている行の数と、落とした行の数（サーバーが覚えずに落とした行と、ランチャーが読む側の遅さで書けなかった行。後者は `log_dropped` の `sources`）。`log_dropped` の事象は、名前の挙がった source の log の、来た位置に覚え（つなぎ直した画面にも同じ位置に届く。1 つの事象は 1 回だけ送る）、画面はそこに「ここで N 行が落ちた」を出す |
| `problems`、`problems_omitted` | サーバーの知らせ（`error` と同じ `{code, message, detail}`）を起きた順に、と、上限を超えて覚えていない数 |

ランチャーが `result` を出さずに終わったときは、`launcher.running: false` と `exit_code`、`state: ended`、知らせの `no-result` になります。

## ECU の変数

ランチャーが Control Server を持つとき（`api/app` の `variables.available`）、GUI サーバーは実行ごとにその Control Server につなぎ、ECU が宣言した変数を読み書きします。持たないランチャー、gRPC のない GUI サーバー、`--no-variables` では、`variables.state` が `off` で、理由が `reason` にあります。例は `ws-variables.json`（一覧、watch、始まる前の書き込み、ステップ送り、終わり）と `ws-variables-failed.json`（つながらない）です。

### 状態と一覧（要約の `variables`、`variables` の通知）

`{"state", "reason", "peers", "peers_omitted", "variables_omitted", "operations", "operations_from_seq", "operations_open_omitted"}`

| 欄 | 内容 |
|---|---|
| `state` | `off`（この実行に変数はない）、`connecting`（Control Server につなぎに行っている。切れてつなぎ直している間も）、`ready`（読み書きできる）、`failed`（つながらなかった、断られた）、`ended`（実行が終わった。最後に読めた値が残っている）。値は増えることがあります |
| `reason` | `off`、`connecting`、`failed` の理由（人向けの英語の文。なければ `null`）。`ended` では、実行の終わりより前に接続が切れていたとき（値が終わりのものより古いかもしれない）だけ |
| `peers` | `[{"name", "connected", "variables": [...]}]`。Control Server が知っている participant（変数のないものも）。`connected` は、その ECU がいまつながっているか |
| `peers[].variables[]` | `{"name", "type", "role", "writable", "unit", "min", "max", "step", "description", "enum"}`。`type` は `bool`、`integer`、`real`、`enum`。`role` は `input`（実行中も書ける）、`parameter`（最初のステップの前だけ書ける）、`output`（読むだけ）。`writable` は `role` が `output` でないこと。`min`、`max`、`step`（編集の刻みの目安）、は数か `null`。`enum` は、`type` が `enum` のとき `[{"name", "value"}]`、ほかは `null` |
| `peers_omitted`、`variables_omitted` | 上限を超えたので入れていない participant と変数の数 |
| `operations` | 操作（下の `operation` から `type` と `session` を除いたもの。サーバーが初めて見た順）。サーバーが覚えている未確定の操作の全部と、最後の 32 件です。途中から開いた画面と、つなぎ直した画面のため |
| `operations_from_seq` | この番号（`seq`）以上の操作は、全部 `operations` に入っています（0 なら、サーバーが見た操作の全部）。`operations` になく、番号がこれより小さい操作は、確定しています（`operations_open_omitted` が 0 のとき）。その `operation` の通知は、もう届きません |
| `operations_open_omitted` | サーバーが覚えきれなかった未確定の操作の数（ふつうは 0）。1 以上のとき、`operations` になく `operations_from_seq` より小さい番号の操作は、確定済みとは限りません |

結果を待っている操作があるクライアントは、つなぎ直した後の `hello`（か `api/session`）の要約で、次のように見分けます。`operations` にあれば、その状態（未確定なら、後で `operation` の通知が届きます）。なく、番号が `operations_from_seq` 以上なら、まだ届いていないだけです。なく、番号が `operations_from_seq` より小さければ、切れている間に確定していて、結果はもう届きません（`operations_open_omitted` が 1 以上なら、確定したかどうかもわかりません）。ライブラリは、この最後の場合を `no-answer` にします。

participant がつながるたびに一覧が変わるので、`variables` の通知は、短い間の変化をまとめて送ります。

### 値（`watch`、`unwatch`、`values`）

画面は、認証の後に次を送ります。

- `{"type": "watch", "id": <0 から 2^53 の整数>, "variables": "all" または [{"peer", "name"}, ...], "period_ms": <整数>}`: 変数の値を受け取り始めます。`id` は、その接続の中で watch を名指す番号で、同じ `id` をもう一度送ると置き換えます。`period_ms` は `limits.watch_period_ms_min`（20）から `limits.watch_period_ms_max` まで（省くと下限）。名前で挙げられる変数は `limits.watch_variables`（2000）個まで、接続ごとの watch は `limits.watches`（16）個までです。超えたものと形の違うものは、`error`（`invalid-watch`、`too-many-watches`）で、接続は続きます。**1 通は 256 KiB までで、これを超える 1 通を送ると、サーバーは `error` を返さずに接続を閉じます（閉じるコード 1009）。** 長い名前を多く挙げるときは、送る前に大きさを確かめ、超えるなら `"all"` を頼むか、watch を分けます（ライブラリは `"all"` に替えます）。同じものを送り直すと、また閉じられます。
- `{"type": "unwatch", "id": <id>}`: やめます（ない `id` は何もしません）。接続が切れると、その接続の watch は全部なくなります（つなぎ直したら送り直します）。

watch は実行をまたいで続きます。実行がないとき、変数の状態が `ready` と `ended` のどちらでもないとき、いまの実行にない変数（その分だけ）には、何も届きません。いまの実行にない名前を挙げても誤りではありません（実行の前に watch できます）。

サーバーは `{"type": "values", "id", "session", "sim_ms", "paused", "peers": {"<peer>": {"sample_ms", "values": {"<name>": <値>}}}}` を送ります。

- watch の直後に 1 回、その後は `period_ms` ごとに、**前に送ったものから変わったときだけ**（値を読んだステップが進んだ、`sim_ms` か `paused` が変わった）。止まっている間は届きません。実行が始まったとき、変数の状態か一覧が変わったとき（`variables` の通知）、実行が終わったときは、`period_ms` を待たずに送ります。ある実行の `values` は、その実行の `variables` の通知（か `hello`）より後に届きます。
- `sample_ms` は、その participant の値を読んだステップの仮想時間です。値は、bool は真偽、整数と列挙は数、実数は数で、有限でない実数（NaN、無限大）は `null` です。2^53 を超える整数は、JSON の数として正確でないことがあります。まだ 1 回も値を読んでいない participant は `peers` にありません。
- **`paused` と `sim_ms` と値は、同じ瞬間のもの**です。`paused` が `true` のとき、シミュレーションは `sim_ms` で止まっていて、`sim_ms` より前に始まったステップの値は、この通知に全部入っています（ちょうど `sim_ms` に始まるステップの値は、入っていることも、止まっている間に後から届くこともあります）。画面は、これを「止まっている時刻の値」として見せられます。サーバーは、止まっている間に時刻と値を読み直して確かめてから `true` にするので、止まった直後の少しの間は `false` のまま届くことがあります（止まっている限り、確かめられるまで読み直し続けます）。`api/time` で再開かステップ送りを送ると、サーバーはその場で `true` をやめます。ほかのクライアントが Control Server に直接再開を頼んだときは、サーバーがそれを知るまでの短い間（約 0.1 秒）、止まっていた時刻の値が `true` のまま届くことがあります（値は、その時刻のもので、新しい値と混ざりません）。`false` のときは、動いている途中の最後の値で、`sim_ms` はサーバーが最後に知った仮想時間です（値のほうが新しいことがあります。わからなければ `null`）。最初のステップの前で止まっている間（`start_paused`）は、`sim_ms` 0、`paused` `true` で、`peers` は空です。
- 値は「最後の値」です。全部の値の列ではありません（速いシミュレーションでは、間のステップの値は届きません）。サーバーは、watch されている participant の値だけを、頼まれた頻度でランチャーから読みます。履歴はサーバーが持たないので、要る画面が自分で持ちます。
- 実行が終わると、状態は `ended` になり、最後に読めた値が 1 回届きます（`paused` は `false`）。終わった後に始めた watch にも 1 回届きます。

### 書き込み（`api/write`、`operation`）

`POST api/write` `{"session": <実行の id>, "writes": {"<peer>": {"<name>": <値>}}, "at_ms": <整数>, "late": "reject" | "apply"}` → `{"session_id", "operation"}`

- `session`（必須）は、書き込みを向ける実行の id（要約の `id`）です。動いている実行の id と違えば、409 `session-changed` で断り、何も書きません。ある実行を見ながら送った書き込みが、その実行が終わって次が始まった後に届いても、次の実行に書かれないようにするためです。断られたら、いまの実行の変数を見てから書き直します。
- `writes` の値は、変数の型に合わせます: `bool` は `true` / `false`、`integer` は整数（整数値の実数も可）、`real` は有限の数、`enum` は項目の値（数）か項目の名前（文字列）。`min` と `max` があれば、その範囲（両端を含む）。サーバーは、変数の一覧と照らして、合わないものを 400 で断ります（`invalid-field`、`unknown-variable`、`not-writable`。1 つでも合わなければ、全部を送りません）。1 回の要求は `limits.writes_per_request`（256）個までです。
- `at_ms` は、反映する仮想時刻（0 から `limits.write_at_ms_max`）で、その時刻以後に始まる最初のステップで反映されます。省くと次のステップです。`late` は、届いたときにその時刻のステップがもう走っていたときの扱いで、`reject`（既定。反映せず、結果が `late`）か `apply`（次のステップで反映し、結果の `late` が `true`）です。
- 1 回の要求の書き込みは、participant ごとに、同じステップでまとめて反映されます。要求は届いた順にランチャーへ渡します（1 つずつ）。
- 最初のステップの前で止まっている間（`start_paused`）に書いたものは、時刻 0 のステップで反映されます（`parameter` も書けます）。一時停止してから書くときは、`at_ms` を止まっている時刻より後にします（その時刻のステップは、もう走った ECU があるため。ランチャーの決まり: [docs/control-api.md](../../docs/control-api.md)）。止まっている時刻そのもの（か、それより前）を指定した書き込みは、いったん `queued` になり、ECU の次のステップで `late` に確定します（`late: "apply"` なら、そのステップで反映されます）。
- 応答は、ランチャーが操作を受け取ったところで返ります（番号だけ）。反映されたかは、`operation` の通知で届きます。応答より先に `operation` が届くことがあります。

`{"type": "operation", "session", "seq", "state", "done", "peers": {"<peer>": {"state", "applied_at_ms", "late", "reason"}}}`

- participant ごとの `state`: 途中は `sent`（ランチャーが ECU へ送った）、`queued`（ECU が受け取った）、`disconnected`。確定は `applied`（`applied_at_ms` のステップで反映した）、`late`、`rejected`（ECU の setter が失敗した）、`cancelled`（反映の前にシミュレーションが終わった）、`invalid`（ECU が断った: 最初のステップの後の `parameter` など。`reason` に理由）、`busy`、`unknown`（反映されたかわからない）。
- 全体の `state`: 途中は `sent`（まだ受け取っていない ECU がある）か `queued`。確定（`done` が `true`）したら、全部の participant が同じならその状態、違えば `mixed` です。確定した操作は変わりません。
- 状態が変わるたびに、全部の接続へ送ります（ほかのクライアントが Control Server に直接書いた操作も届きます）。受け取りの遅い接続でも、`operation` は落としません。ただし、その接続の待ち行列があふれて `resync`（`hello` からの送り直し）になったときは、要約の `operations`（未確定の操作と、最後の 32 件）にあるものだけが届き直します。それより前に確定した操作の結果は、その接続には届きません（どれがそうかは、要約の `operations_from_seq` でわかります）。
- 実行が終わるまでに確定しなかった操作は、サーバーが `unknown`（`reason`: `the run ended before the result came`）で確定させて送ります。未確定の操作が 256 件を超えて、サーバーが古いものを覚えていられなかったとき（要約の `operations_open_omitted`）も、その操作の変化が届けば `operation` で送り、確定しないまま実行が終われば、終わりに `unknown` を送ります（そのときは `peers` が空です）。実行が終わるのと同時にランチャーが受け取った書き込みは、応答で番号を返し、`unknown` で確定させて送ります。
- GUI サーバーと Control Server の接続が切れている間に確定した操作と、Control Server が GUI サーバーへ送りきれなかった変化は、つながり直したとき（送りきれなかったと知ったとき）に読み直して送ります。

## 上限

サーバーが覚える量には上限があり、超えた分は数えて知らせます。log は source ごとに直近 2000 行と 1 MiB（64 source まで。それより後の source は共有の `*`）、全体で 16 MiB。64 KiB を超える 1 行の log は覚えません（中継はします）。`check` と `result` は 4 MiB まで（超えたら `check` は `null`、`result` は入れ子でない短い欄だけ。例は `ws-kept-limit.json`）、`findings` は 1000 個と 1 MiB、`participants` は 256 人で 1 人 4 KiB、`phase`、`time`、`registry`、`command` は 1 つ 4 KiB。4 KiB を超えても、大事な欄は必ず入れます（participant の `name`、`state`、`pid`、`exit_code`、`signal`、`reason`、`stop_requested`、`control_link`、`phase` の段階の名前、`time` の `sim_ms`、`speed`、`paused`、`late_steps`、`max_late_ms`、`slipped_ms`、`late_ms`、`registry` の `uri`、`command` の `seq`、`command`、`accepted`、`ms`。これだけで 4 KiB を超えるときは入れません）。`problems` は、ランチャーが書いたもの（標準エラーを除く）が 100 個、サーバー自身の知らせ（`kept-limit`、`no-result`、`output-left-open`、`stop-not-delivered`、`stop-escalated`、`launcher-silent`）が別に 50 個、標準エラーが別に最初の 20 行と最後の 30 行です。知らない事象は中継だけして覚えません（つなぎ直した画面には届きません）。 ECU の変数: 一覧は participant 256 人、変数は全部で 10000 個まで（超えた分は `peers_omitted`、`variables_omitted` に数える。名前が 256 文字を超えるもの、列挙の項目が 256 個を超えるもの、知らない型のものも入れない）。単位、説明、列挙の項目の名前は 256 文字、理由の文は 500 文字で切ります。値は participant ごとに最後の 1 組だけ。操作は 256 件まで覚え（確定したものから忘れる）、要約には、覚えている未確定の操作の全部と最後の 32 件。watch は接続ごとに 16 個、名前で挙げる変数は 1 つに 2000 個、間隔は 20 ms から 60 秒。`api/write` は 1 回に 256 個、順番を待つ要求は 32 個まで（超えたら 503 `write-busy`）、Control Server の答えを待つのは 5 秒まで。画面が送る WebSocket のメッセージは 256 KiB まで。値を読むのにかける時間は、長くても全体の半分で、同時に出す読み取りは 32 本までです（participant が多く、間隔が短いときは、頼まれたより遅くなります）。Control Server が、つないでから 10 秒たっても今の状態を送らなければ、つなぎ直します。
