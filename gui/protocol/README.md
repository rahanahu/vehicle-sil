# gui/protocol

GUI サーバー（`gui/server`）の HTTP と WebSocket の口の、公開の契約です。サンプルの画面（`gui/web`）も、クライアントのライブラリ（`gui/client`）を通して、この口だけを使います。自分の画面や道具を作るときも、この文書と例に従えば、同じことができます（作り方は [docs/gui.md](../../docs/gui.md) の「自分の画面を作る」）。

`examples/` の JSON は、サーバーの試験（`gui/server/tests/test_protocol_examples.py`）が、本物のランチャーの出力を録ったものを流し直すランチャー（`gui/server/tests/replay_launcher.py`）との実際のやりとりから作り、毎回照合します。ファイルに残すのは、形ごとに 1〜2 個と、文書や画面とライブラリの試験が読むものです。同じ形のほかの場合（ほかの誤りのコード、ほかの終わり方の実行）は、試験の中で、誤りのコードややりとりの終わり方を確かめます。試験が作らないファイルが `examples/` に残っていても試験は落ちます。形を変えたら `UPDATE_PROTOCOL_EXAMPLES=1 scripts/verify-gui.py --skip-web` で書き直し、ライブラリの型と読み取り（`gui/client/src/protocol.ts`）と、その単体試験（同じファイルを読む）を合わせます。プロセス ID、時刻、インスタンス、この PC のパス、決まらない数は、決まった値に置き換えてあります（`t_ms` は `seq × 10`）。

## 版と互換

口の版は `vehicle_sil_gui`（今は 1）です。サーバーは `GET api/app` の応答、WebSocket の `hello`、`--json` で起動したときの 1 行に書きます。

- **足すのは互換**: 欄、`type`、`code`、事象、要求、認証のメッセージの任意の欄を足しても、版は上げません。読む側は、知らない欄、知らない `type`、知らない事象（`event`）、知らない `code`、値の集合が増えうる欄（`phase`、`state`、`result` など）の知らない値を、無視するか、そのまま通します。
- **消す、意味を変えるのは版を上げる**: ここにある欄や要求を消す、型や意味を変える、送る順の決まりを変えるときは、`vehicle_sil_gui` を上げます。読む側は、知らない版の `api/app` と `hello` を読まずに、版が違うと知らせます。待っても直らないので、つなぎ直しません（ライブラリは `api/app` で `ProtocolError`、`hello` で `session.incompatible` を立てて止まる）。
- **読み飛ばすものと、受け直すもの**: 知らない `type`、欄、事象、値は、読み飛ばすか、そのまま通すだけです（足すのは互換なので）。一方、WebSocket のメッセージが JSON として読めない、または `hello` と `session` の要約の芯（`id`、`state`、`launcher`、`stop`）が読めないときは、何が抜けたかわからないので、つなぎ直して `hello` から受け直します（要約が正。続けて起きるたびに、つなぎ直すまでの間を延ばす）。
- **頼んだときだけ変わる形**: 前の形のままでは重いものは、クライアントが認証のメッセージで頼んだときだけ形を変えます（`session_without_check`）。頼まないクライアントには、いままでの形で送ります。
- ランチャーの事象（`event` の中身）は、ランチャーの `run --events` の形そのままで、その版（`hello` の事象の `vehicle_sil_events`）はランチャーが決めます（[docs/simulation-events.md](../../docs/simulation-events.md) の「機械から使う」）。
- 誤りの `code` の名前は変えません（足すことはあります）。`message` は人向けの英語の 1 文で、文言は変わることがあります。機械は `code` で分けます。

## 認証と Origin

- サーバーは `127.0.0.1` だけで待ち、`Host` は `127.0.0.1:<port>`（開発のときは `--dev-origin` のホストも）だけを受け付けます。
- トークンは起動のたびに作られ、サーバーが表示した URL の `#token=` の後ろにだけあります。HTTP は `Authorization: Bearer <token>`、WebSocket は最初のメッセージ（`auth`）で送ります。サーバーが配る画面（サンプルの画面と、`--web` で配る自分の画面）は、開いた URL の `#token=` を読んで、すぐに URL から消します（クライアントのライブラリの `takeToken`）。トークンを URL の query やログに載せません（query のトークンは受け付けません）。
- `POST` と WebSocket の握手は、`Origin` がサーバー自身（`http://127.0.0.1:<port>`。開発のときは `--dev-origin`）のものだけを受け付けます。ブラウザは自分で付けます。ブラウザの外（Node、Python など）から使うときは、サーバー自身の origin を `Origin` に書きます（`examples/gui_client/run.mjs`）。
- CORS は許しません。別の origin のページからは使えません。

## 使い方の例

- 画面を使わない最小のクライアント: [examples/gui_client/run.mjs](../../examples/gui_client/run.mjs)（素の JavaScript、依存なし、Node 22 以上）。照合して実行を始め、事象を読み、終わりを待ちます。`scripts/verify-gui.py` が、流し直すランチャーを相手に動かして確かめます。
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
| `POST api/check` `{file, disable, manual, scenario}` | `{file, disable, manual, scenario, exit_code, check}`。`check` は `check --json` の結果そのまま。照合が失敗しても 200 | `check-*.json` |
| `POST api/run` `{file, disable, manual, speed, start_paused, scenario}` | `{session}`（始めた実行の要約） | `run.json` |
| `POST api/stop` `{force}` | `{session}`（今の要約。実行がなければ `null`） | `stop*.json` |
| `POST api/time` `{command, ms, value}` | `{session_id, command}`（ランチャーに送った命令） | `time.json` |
| `POST api/write` `{session, writes, at_ms, late}` | `{session_id, operation}`（操作の番号。結果は WebSocket の `operation`） | `write.json` |
| `GET api/session` | `{session}`（なければ `null`） | `session-*.json` |
| `GET api/dashboard?file=<構成ファイル>` | 構成ファイルの隣のダッシュボードのファイル: `{file, path, found, errors, dashboard}`（下の「ダッシュボードのファイル」） | `dashboard*.json` |
| `GET api/scenarios?file=<構成ファイル>` | 構成ファイルの隣の `scenarios` のシナリオ: `{file, directory, found, scenarios, truncated}`（下の「シナリオ」） | `scenarios.json` |
| `GET api/recording` | いまの（最後の）実行の、反映された書き込みをシナリオの YAML にしたもの: `{recording}`（実行がなければ `null`。下の「操作の記録」） | `recording.json` |

- `disable` と `manual` に書けるのは、そのファイルの照合の結果にある participant 名だけです（`unknown-participant`）。`-` で始まる名前は、ランチャーが引数の値として受け取れない（ランチャーの照合もそのような名前を断る）ので 400 `name-like-option` です（構成ファイルで名前を変える）。同じ名前を両方に書くと 400 `invalid-field` です（ランチャーも断ります）。
- **自分で起動する participant**: `manual` に書いた participant は、ランチャーに `--manual` で渡り、ランチャーは起動せずに参加を待ちます（[docs/simulation-config.md](../../docs/simulation-config.md) の「participant を自分で起動する」。構成ファイルの `manual: true` は、照合の結果の `participants[].manual` に出ます）。`api/app` の `launcher.version.features` に `manual-participants` があるときだけ送ります（ないときは欄を省きます。送ると 400 `unsupported`）。起動するコマンドは、その participant の事象（`state` が `manual`）の `command`、`working_directory`、`command_line` で届き、要約の `participants` に残ります（例は `check-manual.json`、`ws-manual.json`）。
- `speed`（`null`、`"max"`、数）と `start_paused`（真偽）は、`api/app` の `launcher.version.features` に `speed`、`start-paused` があるときだけ値を送ります。ないときは欄を省くか `null` / `false` を送ります（ほかの値は 400 `unsupported`）。`speed` の数の範囲は `limits.speed_min` から `limits.speed_max` まで（ランチャーと同じ）。`null` は構成ファイルの `speed` のまま（ランチャーに `--speed` を渡さない）です。
- **時刻の命令**: `api/time` の本文は `{"command": "pause"}`、`{"command": "resume"}`、`{"command": "step", "ms": <1 から limits.step_ms_max の整数>}`、`{"command": "speed", "value": <"max" か speed と同じ範囲の数>}` のどれかです（ほかの欄は 400 `invalid-field`）。サーバーは、動いている実行のランチャーの標準入力に 1 行で書き、応答を待たずに返します（止まっているランチャーで要求が固まらないように）。受け付けたかどうかと理由は、ランチャーの `command` の事象（`accepted`、`message`、`step` の実際に進む `ms`）で、効いたことは `time` の事象で届きます（要約の `last_command` と `time` にも残ります）。動いている実行がなければ（終わった、始まりかけ、ランチャーの入力が閉じた）409 `not-running`、その実行のランチャー（始めたときの `version --json`）の `features` に `time-commands` がなければ 400 `unsupported` です。ランチャーが入力を読まずに詰まっているときも、要求は長くて約 2 秒で返ります（命令はランチャーの入力に残ります）。命令の意味（一時停止は刻みの境目で効く、ステップ送りは一時停止の間だけ、など）はランチャーの決まりです（[docs/simulation-events.md](../../docs/simulation-events.md) の「標準入力の命令」）。
- **開始**: 同時に送られた `api/run` は、1 つだけが始めます。ほかは、実行が始まりかけているか動いている間、409 `already-running` です。応答の要約を、画面はその場で当てます（後から届く `session` も同じ内容です）。
- **照合**: 同時の `api/check` は断らず、順に 1 つずつ実行します。待っている要求が多すぎるか、待ちが照合の時間の上限（`api/app` の `limits.check_timeout_s`）を過ぎたら 503 `check-busy` です。`api/run` も、participant 名を確かめるために同じ順番で照合することがあります。
- **停止**: 本文は `{"force": 真偽}`（省略は `false`。本文なしで `Content-Type` もない要求も `false`）。`force: false` は「順に止める」（ランチャーの標準入力に `{"command":"stop"}`）で、すでに頼んであれば何もしません。`force: true` は「待たずに止める」（まだ頼んでいなければ `stop` を 2 回、頼んであればもう 1 回）で、すでに頼んであれば何もしません。どちらも、実行がない、または終わっているときは 409 にせず、今の要約を返します（何度呼んでも同じ結果）。実行が始まりかけている間（`api/run` が応答する前）の停止は、ランチャーが起動したところで届けます（応答の要約は前の実行のもの。始まった実行の要約の `stop` に出ます）。止めた経過は、要約の `stop` と、ランチャーの `command` / `phase` の事象で見えます。標準入力を読まないランチャーは、これでは止まりません（サーバーを止めると、シグナルで止めます）。
- **書き込み**: 下の「ECU の変数」。
- **シナリオ**: 下の「シナリオ」。
- **ダッシュボードのファイル**: 下の「ダッシュボードのファイル」。`file` は `api/check` と同じ検査を通ります（`invalid-path`、`outside-root`、`not-a-configuration`、`file-not-found`）。
- サーバーが終わり始めたら、`api/run`、`api/check`、`api/write` は 503 `shutting-down` です。

## 誤りの `code`

画面が見るコードです。名前は変えません（足すことはあります）。

| HTTP | `code` | いつ |
|---|---|---|
| 400 | `invalid-json` | 本文が JSON のオブジェクトでない |
| 400 | `invalid-field` | 欄の型か範囲が違う（`speed`、`start_paused`、`force`、`disable`、`manual`（`disable` と同じ名前も）、`api/time` の `command`、`ms`、`value`、`api/write` の `writes`、`at_ms`、`late` と、変数の型か範囲に合わない値。`detail` に `{peer, name}`） |
| 400 | `invalid-path` | `file` が root からの相対パスの文字列でない（空、長すぎる、`\`、NUL、`:`、対のないサロゲート） |
| 400 | `outside-root` | 絶対パス、`..`、root の外へのシンボリックリンク |
| 400 | `not-a-configuration` | `.yaml` / `.yml` の通常のファイルでない |
| 400 | `unknown-participant` | `disable` か `manual` の名前が照合の結果にない |
| 400 | `name-like-option` | `disable` か `manual` の名前が `-` で始まる（ランチャーに渡せない） |
| 400 | `scenario-not-listed` | `scenario` が、`api/scenarios` がその構成ファイルに挙げるシナリオでない（`scenarios` がない、リンク、ほかの場所のファイル） |
| 400 | `unsupported` | ランチャーが `speed` / `start-paused` / `scenario` / `manual-participants` を持たないのに値を送った。`time-commands` を持たないのに `api/time` を送った。`control-server` を持たないのに `api/write` を送った |
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
| 409 | `scenario-running` | `api/write`: 動いている実行がシナリオを回している（シナリオの結果を変えないため、書かない）。例 `error-scenario-running.json` |
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

例は `error-already-running.json`、`error-not-writable.json`、`error-scenario-not-listed.json`、`error-scenario-running.json`、`error-session-changed.json` です（ほかのコードは、GUI サーバーの試験の中で確かめます）。`error-invalid-watch.json` は WebSocket の `error` の例です。`stop.json`、`stop-force.json`、`stop-ended.json` は停止の応答、`time.json` は時刻の命令の応答です。

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
| `file`、`disable`、`manual`、`speed`、`start_paused` | 始めたときの指定（`manual` は `api/run` で指定したものだけ。構成ファイルの `manual: true` は `check` にある）（`speed` の `null` は構成ファイルのまま。いまの速さは `time.speed`） |
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
| `scenario` | シナリオの実行（`api/run` の `scenario`）の経過。シナリオのない実行では `null`（下の「シナリオ」） |

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

- watch の直後に 1 回、その後は `period_ms` ごとに、**前に送ったものから変わったときだけ**（値を読んだステップが進んだ、`sim_ms` か `paused` が変わった）。止まっている間は、値が変わらなければ届きません。実行が始まったとき、変数の状態か一覧が変わったとき（`variables` の通知）、実行が終わったときは、`period_ms` を待たずに送ります。ある実行の `values` は、その実行の `variables` の通知（か `hello`）より後に届きます。
- `sample_ms` は、その participant の値を読んだステップの仮想時間です。値は、bool は真偽、整数と列挙は数、実数は数で、有限でない実数（NaN、無限大）は `null` です。2^53 を超える整数は、JSON の数として正確でないことがあります。まだ 1 回も値を読んでいない participant は `peers` にありません。
- **`paused` と `sim_ms` と値は、1 回の読み取りのもの**です。サーバーは、watch している participant の値と仮想時間を、Control Server の 1 回の呼び出し（`GetSnapshot`。[docs/control-api.md](../../docs/control-api.md) の「時刻と値をまとめて読む」）で読み、`paused` はその応答の印（`consistent`）から決めます。
  - `paused` が `true`: シミュレーションは `sim_ms` で止まっていて、値は、その時刻のものです。`sim_ms` より前に始まったステップの値は全部入っていて、`sim_ms` より後のステップの値は 1 つも入っていません。画面は、これを「止まっている時刻の値」として見せられます。
  - ちょうど `sim_ms` に始まるステップの値は、入っているかどうか決まりません（止まっている間に、後から届くことがあります）。届いたら、同じ `sim_ms`、`paused` `true` で、値の違う通知がもう 1 回届きます（止まっている間、サーバーが読み直すのは、watch の `period_ms` と約 0.25 秒の長いほうの間隔です。間隔の長い watch では、その分だけ後になります）。入っているかは、participant ごとの `sample_ms` でわかります。
  - `paused` が `false`: 動いている、終わっている、読む間に進んだ、のどれかで、値は最後の値です。`sim_ms` は、値と一緒に読んだ仮想時間で、値はそれより古くありません（値のほうが新しいことはあります）。まだ読んでいなければ、サーバーが最後に知った仮想時間です（わからなければ `null`）。
  - この約束の前提と限り（Control Server のもの）: ECU が値をステップの中から送っているかぎりです（`ecu_main` で作った ECU はそうします。破れると、ランチャーが警告 `control-link-late-values` を出し、`true` でも値が欠けていることがあります）。「ステップの値」は、その時刻より前に ECU が読んだ最後の値です。実時間で動く participant（シミュレーションの時刻で動かないもの）の値には約束がなく、`paused` はその値を見ずに決まります。切れた participant の値は、最後の値のままです（`variables` の `connected` で見ます）。
  - 1 つの watch の participant を別々の呼び出しで読んだとき（間隔の違う watch が同じ participant を見ているとき）は、どの participant も同じ時刻で止まっていると読めていて、その後のどの読み取りもその時刻で止まっていたときだけ `true` です。
  - `api/time` で再開かステップ送りを送ると、サーバーは、それまでに読んだ値を「止まっている時刻の値」と扱うのをやめ、watch の次の間隔で読み直した結果で決め直します。通知が届くのは、その間隔で、前に送ったものと変わったときです（ランチャーがまだ命令を受け取っていなければ、同じ時刻で止まったままと読めるので、`false` の通知は届きません）。
  - ほかのクライアントが Control Server に直接再開やステップ送りを頼んだときは、サーバーが次に読むか、時刻の知らせ（Control Server から約 0.1 秒ごと）で、止まっていた時刻より進んだ・終わったとわかるまで、止まっていた時刻の値が最後の通知のままです。わかった後は、値を読めなくても、watch の次の間隔で `false` の通知が届きます。
  - watch している participant の値が、Control Server の 1 つの応答の上限（4 MiB）に入らないときは、入らなかった呼び出しの participant を半分ずつに分けて読みます（サーバーのログに警告を出します）。分けて読んでも、どの呼び出しも同じ時刻で止まっていると答えれば `true` です。1 つの participant の値だけで上限を超えるときは、その値を読みません（ログに警告。その participant が別のプロセスに替わるか、変数が変わるまで）。その participant を含む watch の `paused` は `false` のままです。
  - 値を読めないこと（Control Server が期限までに答えない、など）が 3 回続くと、変数の状態は `ready` のまま、`reason` に理由が入った `variables` の通知が届きます（値は最後に読めたもの）。読めるようになると、`reason` は `null` に戻ります。
  - 最初のステップの前で止まっている間（`start_paused`）は、`sim_ms` 0、`paused` `true` で、`peers` は空です。
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

## シナリオ

ランチャーの `run --scenario` と `check --scenario`（[docs/scenario.md](../../docs/scenario.md)）を、画面から使う口です。ランチャーの `features` に `scenario` があるときだけ使えます。例は `scenarios.json`、`check-scenario.json`、`ws-scenario.json`、`error-scenario-not-listed.json`、`error-scenario-running.json` です。

- `GET api/scenarios?file=<構成ファイル>`: 構成ファイルの隣のディレクトリ `scenarios` の `.yaml` と `.yml`（名前の順。200 個まで）。`directory` はそのディレクトリ（root からの相対）、`found` はあるか、`scenarios` は `[{path, name, size}]`（`path` は root からの相対）。`file` は `api/check` と同じ検査を通ります。挙げないもの: `.` で始まる名前、サブディレクトリ、通常のファイルでないもの（シンボリックリンクを含む）、`:` や `\` を含む名前、255 文字を超える名前。`scenarios` か、その上のディレクトリ（root の下）がシンボリックリンクなら、`found` は `false` です。
- `api/check` と `api/run` の `scenario`: 省くか `null` なら、なし。値は、その構成ファイルの `api/scenarios` の `path` のどれかだけで（サーバーが一覧を作り直して確かめます）、ほかは 400 `scenario-not-listed` です。サーバーは、それをランチャーに `--scenario` で渡します（ランチャーは root で動くので、root からの相対のまま。`-` で始まるものは `./` を付けます）。`api/check` の応答の `scenario` は、渡したシナリオです。
- シナリオを回す実行は、止めた状態では始められません（`start_paused: true` は 400 `invalid-field`）。ランチャーの `features` に `scenario-control` があれば、変数を読めます（`--control` を付けます。書き込みはランチャーも断ります）。なければ、`--control` を付けず、変数の状態は `off` で、`reason` がそう言います。どちらでも、シナリオの実行中の `api/write` は、サーバーが 409 `scenario-running` で断ります。時刻の命令（`api/time`）と停止は、ふつうの実行と同じです。
- 事象 `scenario_step`（手順が始まった `running` と、決まったとき）は、ランチャーが書いたまま届きます（`index` が 1 以上の整数、`status` が文字列でないものは `bad-event`）。手順の所見（`scenario-failed` など）は、シナリオの `file` と `line` を持つ `finding` です。
- 要約の `scenario`: `{file, name, step_count, steps, steps_omitted, status, over_at_ms}`。`file` は渡したシナリオ、`name` と `step_count` は照合の結果の `scenario` から、`steps` は届いた手順（`index` の順。手順ごとに最後の状態。`steps[]` の欄は事象と同じ。`text` と `message` は 1000 文字で切る。1000 手順まで、超えた数は `steps_omitted`）、`status` と `over_at_ms` は結果の `scenario` から（結果が来るまで `null`）。結果が来たら、`steps` は結果の手順（`not_run` を含む）になります。途中から開いた画面も、これで手順の表を作れます。
- シナリオで止まった実行の結果の `simulation` は `ended` です（結果の `scenario` があることで、participant が止めた `ended` と区別します）。

## 操作の記録（`api/recording`）

GUI サーバーは、実行ごとに、自分を通した書き込み（`api/write`。つないでいるどの画面からのものも、`api/write` を使う道具からのものも）のうち、反映されたもの（participant ごとの `operation` の `applied` と `applied_at_ms`）を覚えます。`GET api/recording` は、それをシナリオの YAML にして返します（いまの実行か、終わった直後の実行。次の実行を始めるまで）。**サーバーはファイルを書きません**。画面がブラウザの保存として渡し、置き場所は利用者が決めます。例は `recording.json` です。

`{"recording": {"session_id", "file", "state", "available", "reason", "writes", "omitted", "steps", "yaml"}}`

- `available`: この実行で覚えているか（変数のない実行と、シナリオの実行では `false`。`reason` が理由）。`yaml` が `null` のときは、`reason` がその理由（まだ反映された書き込みがない、など）です。
- `writes` は YAML に入れた値の数、`steps` は手順（`set`）の数、`omitted` は入れなかった値の数です。入れなかった理由は YAML の先頭のコメントに書きます（上限の 2000 個を超えた、participant の名前に `.` がある（シナリオでは最初の `.` までが participant の名前なので、名指せない））。
- YAML の先頭のコメントは、何を記録したか、シナリオが最後の手順の時刻で終わること（ランチャーはそこでシミュレーションを止めます。記録した実行がそれより先まで走っていれば、その時刻も）、最後の手順の後も走らせたいときに足す手順の例（`expect` と `hold_for_ms`）を言います。
- YAML は `vehicle_sil_scenario: 1`、`name`、`steps` で、手順は `at_ms`（反映したステップの仮想時刻）と `set` だけです。同じ時刻に反映された値は 1 つの `set` にまとめ、同じ変数の 2 つ目はその時刻の最後の値にします。変数は `<participant>.<変数名>`、列挙は項目の名前（いつも引用符つき。名前が 1 つに決まらなければ値）です。
- 書き方: 変数の名前は、英字か `_` で始まり、英数字、`_`、`-`、`.` だけで `.` を含むときだけ引用符なし、ほかは二重引用符で、印字できる ASCII 以外の文字（改行、タブ、BOM、制御文字、ASCII でない文字）は `\uXXXX`（と `\UXXXXXXXX`）でエスケープします（`\xNN` は使いません。ランチャーの YAML の読み方は、`\xNN` を文字 U+00NN でなく 1 バイトとして読むため）。実数はいつも `.` を含めて書きます（`1.0`、`1.0e+20`。YAML 1.1 と 1.2 のどちらでも数として読める）。有限でない数は書きません。
- 入れないもの: 時刻の操作（一時停止、ステップ送り）、反映されなかった書き込み（`late`、`rejected` など）、GUI サーバーを通さずに Control Server に直接書いたもの、期待する値（`expect`）。書き込んだ画面や道具の区別は持ちません（1 つの実行の全部の書き込みが 1 つのシナリオになります）。書き出した後で、エディターで足します。

## ダッシュボードのファイル

`GET api/dashboard?file=<構成ファイル>` は、構成ファイルの隣の `<名前（拡張子なし）>.dashboard.yaml` を読み、形を確かめて JSON で返します（書き方は [docs/gui.md](../../docs/gui.md) の「ダッシュボードのファイル」、JSON Schema は [dashboard.schema.json](dashboard.schema.json)）。実行がなくても読めます。

- `path`: ダッシュボードのファイル（root からの相対）。`found`: あるか。ないのは誤りではありません（画面は変数を自動で並べます）。
- `errors`: 誤りの並び（`line` と `column` は 1 から、`at` はデータの中の場所 `sections[0].items[2].widget`、`message` は直し方を含む英語の 1 文）。1 つでもあれば `dashboard` は `null` です。最大 50 個。
- `dashboard`: `{title, panels, sections}`。`panels` は `[{module, url}]`（`url` は画面の base からの相対で、モジュールを読み込む先。パスの部分ごとにパーセントエンコードしてある）。`sections` は `[{title, collapsed, items}]`、`items` はどれか 1 つの形: `{variable: {peer, name}, widget, label, options}`（`widget` と `label` は省けば `null`）、`{panel, label, options, variables: [{peer, name}]}`、`{auto}`。読む側は、知らない形の項目を読み飛ばします。
- 変数があるか、部品が変数に合うかは、サーバーは確かめません（画面が、いまの変数の一覧で確かめます）。
- 読み方の上限: 256 KiB、入れ子 16 段、値 50000 個、まとまり 200 個、項目は全部で 2000 個、パネルのモジュール 16 個、1 つの値（文字列、数）1024 文字。`widget` は名前の形（小文字の英字で始まる）だけを確かめ、どの部品があるかは画面が確かめます。YAML の別名（`*name`）、2 つ目の文書、YAML の型のタグ（`!!python/...` など）は誤りです。シンボリックリンクで root の外を指すファイルは読みません。

**パネルのモジュール**: `GET panels/<key>/<root からの相対パス>` が、持ち込みのパネルの ES モジュールを `text/javascript`（`X-Content-Type-Options: nosniff`、ほかの応答と同じ CSP、`Cache-Control: no-store`）で返します。`<key>` は起動ごとの乱数で、`api/dashboard` の `url` にだけ入っています（ブラウザはモジュールの import にヘッダーを付けられないので、トークンの代わりに URL に入れます。トークンとは別の値で、知っていてもパネルのモジュールを読めるだけです）。返すのは、root の下の `panels` という名前のディレクトリ（どの場所のものでも。`panels` とその上のディレクトリがシンボリックリンクでないもの）の中の `.js` / `.mjs` の通常のファイル（1 MiB まで。リンクをたどった先も、その `panels` の中）だけで、`.` で始まる名前、`..`、`\`、`:`、NUL を含むパスと、`<key>` の違う要求は 404 です。`Host` と `Origin` の検査はほかと同じです。

## 上限

サーバーが覚える量には上限があり、超えた分は数えて知らせます。log は source ごとに直近 2000 行と 1 MiB（64 source まで。それより後の source は共有の `*`）、全体で 16 MiB。64 KiB を超える 1 行の log は覚えません（中継はします）。`check` と `result` は 4 MiB まで（超えたら `check` は `null`、`result` は入れ子でない短い欄だけ。例は `ws-kept-limit.json`）、`findings` は 1000 個と 1 MiB、`participants` は 256 人で 1 人 4 KiB、`phase`、`time`、`registry`、`command` は 1 つ 4 KiB。4 KiB を超えても、大事な欄は必ず入れます（participant の `name`、`state`、`pid`、`exit_code`、`signal`、`reason`、`stop_requested`、`control_link`、`phase` の段階の名前、`time` の `sim_ms`、`speed`、`paused`、`late_steps`、`max_late_ms`、`slipped_ms`、`late_ms`、`registry` の `uri`、`command` の `seq`、`command`、`accepted`、`ms`。これだけで 4 KiB を超えるときは入れません）。`problems` は、ランチャーが書いたもの（標準エラーを除く）が 100 個、サーバー自身の知らせ（`kept-limit`、`no-result`、`output-left-open`、`stop-not-delivered`、`stop-escalated`、`launcher-silent`）が別に 50 個、標準エラーが別に最初の 20 行と最後の 30 行です。知らない事象は中継だけして覚えません（つなぎ直した画面には届きません）。 ECU の変数: 一覧は participant 256 人、変数は全部で 10000 個まで（超えた分は `peers_omitted`、`variables_omitted` に数える。名前が 256 文字を超えるもの、列挙の項目が 256 個を超えるもの、知らない型のものも入れない）。単位、説明、列挙の項目の名前は 256 文字、理由の文は 500 文字で切ります。値は participant ごとに最後の 1 組だけ。操作は 256 件まで覚え（確定したものから忘れる）、要約には、覚えている未確定の操作の全部と最後の 32 件。watch は接続ごとに 16 個、名前で挙げる変数は 1 つに 2000 個、間隔は 20 ms から 60 秒。`api/write` は 1 回に 256 個、順番を待つ要求は 32 個まで（超えたら 503 `write-busy`）、Control Server の答えを待つのは 5 秒まで。画面が送る WebSocket のメッセージは 256 KiB まで。値を読むのにかける時間は、長くても全体の半分で、読み取りは 1 つずつです（1 回の呼び出しで、間隔の来た watch の participant をまとめて読みます。応答の上限に入らず分けたときだけ、1 回の読み取りで複数の呼び出しを同時に出します。読むのが遅いときは、頼まれたより遅くなります）。Control Server が、つないでから 10 秒たっても今の状態を送らなければ、つなぎ直します。
