# gui/protocol

GUI サーバー（`gui/server`）と画面（`gui/web`）の間でやりとりする形の正本です。`examples/` の JSON は、サーバーの試験（`gui/server/tests/test_protocol_examples.py`）が偽のランチャーとの実際のやりとりから作り、毎回照合します。形を変えたら `UPDATE_PROTOCOL_EXAMPLES=1 uv run pytest gui/server/tests` で書き直し、画面の型（`gui/web/src/app/core/api.ts`）と、その単体試験（同じファイルを読む）を合わせます。プロセス ID、時刻、インスタンス、この PC のパスは、決まった値に置き換えてあります（`t_ms` は `seq × 10`）。

読む側は、知らない欄、知らない `type`、知らない事象（`event`）を無視して通します。サーバーは欄を足すことがありますが、ここにある欄を消したり意味を変えたりするときは `vehicle_sil_gui`（今は 1）を上げます。ランチャーの事象（`event` の中身）は、ランチャーの `run --events` の形そのままです。

## HTTP

どの `api/` にも `Authorization: Bearer <token>` が要ります（token は、サーバーが表示した URL の `#token=` の後ろ）。`POST` は `Content-Type: application/json` で、`Origin` がサーバー自身（開発のときは `--dev-origin`）のものだけを受け付けます。応答は JSON で、失敗は `{"error": {"code", "message", "detail"}}` です（`code` は機械が読む名前、`message` は人向けの英語の 1 文で、直し方を含みます）。

| 要求 | 応答 | 例 |
|---|---|---|
| `GET api/app` | サーバーの版、`instance`（起動ごとに変わる）、root、`initial_file`、ランチャー（`usable`、`problems`、`version --json` の内容）、上限 | `app.json` |
| `GET api/configs` | root の下の `.yaml` / `.yml`（`path` は root からの相対で `/` 区切り、`modified`、`size`、`simulation`: 先頭に `vehicle_sil_simulation` があるか）、`truncated` | `configs.json` |
| `POST api/check` `{file, disable}` | `{file, disable, exit_code, check}`。`check` は `check --json` の結果そのまま。照合が失敗しても 200 | `check-*.json` |
| `POST api/run` `{file, disable, speed, start_paused}` | `{session}`（要約）。`speed` は `null`、`"max"`、数。動いている間は 409 | `run.json` |
| `POST api/stop` | `{session}`。2 回目は待たずに止める | `stop.json` |
| `GET api/session` | `{session}`（なければ `null`） | `session-*.json` |

`disable` に書けるのは、そのファイルの照合の結果にある participant 名だけです（`unknown-participant`）。`speed` と `start_paused` は、ランチャーの `features` に `speed`、`start-paused` があるときだけ使えます（ないときは `unsupported`）。主な誤り: 401 `unauthorized`、403 `forbidden-host` / `forbidden-origin`、400 `invalid-json` / `invalid-field` / `invalid-path` / `outside-root` / `not-a-configuration` / `unknown-participant` / `unsupported`、404 `file-not-found`、409 `session-running` / `not-running` / `check-busy`、415、502 `launcher-start` / `launcher-output`、503 `launcher-unusable`、504 `check-timeout`（例は `error-*.json`）。

## WebSocket（`ws`）

つないだら最初に `{"type": "auth", "token": "...", "session": <id>, "from_seq": <n>}` を送ります（`session` と `from_seq` は、再接続で持っている分を言うときだけ）。トークンが違えば `error` の後に 4401 で閉じます。サーバーから届くもの:

| `type` | 内容 |
|---|---|
| `hello` | 接続（と `resync`）のたびに最初に 1 回。`instance`、`session`（要約。なければ `null`）、`replay`（`from_seq`: どこから再送するか、`null` は最初から。`omitted.log`: 上限を超えて覚えていない log の行の数を source ごとに、`omitted.events`: ほかの事象の数）。受けたら画面の状態をこの要約で作り直す |
| `event` | `{session, event}`。ランチャーの事象そのまま。`hello` の後の再送も同じ形。再送は、覚えている事象（log は source ごとに直近の行、`time` は最後の 1 つ）を `seq` の順に |
| `synced` | 再送の終わり。`{session, last_seq}` |
| `session` | `{session}`（要約）。実行を始めたとき（新しい `id`）、止めるよう頼んだとき、ランチャーが終わったとき |
| `error` | `{session, code, message, detail}`。サーバーが気づいたこと: `launcher-output`（事象でない行）、`line-too-long`、`seq-gap`、`seq-order`、`no-hello`、`events-format`（`hello` の版の違い）、`launcher-stderr`、`no-result`（`result` なしで終わった）、`stop-escalated`。要約の `problems` にも残る |
| `resync` | 画面の受け取りが遅く、送る分を捨てた。すぐ後に `hello` から送り直す |

要約（`session`）: `id`、`file`、`disable`、`speed`、`start_paused`、`state`（`running`: ランチャーが動いている、`ended`）、`phase`（最後の `phase` 事象）、`stop_requests`、`started_at`、`ended_at`、`launcher_pid`、`exit_code`、`last_seq`、`sim_ms`、`participants`（`name`、`required`、`enabled`、`state`、`pid`、`exit_code`、`signal`）、`findings`（`finding` 事象）、`result`（`result` 事象の中身）、`problems`、`problems_omitted`。

例: `ws-pass.json`（通しの実行）、`ws-check-error.json`、`ws-participant-crash.json`、`ws-stop.json`、`ws-ignore-stop.json`、`ws-launcher-crash.json`、`ws-bad-lines.json`、`ws-replay.json`（実行の後につないだ）、`ws-resume.json`（`from_seq` つきの再接続）、`ws-unauthorized.json`。どれも `client`（画面が送ったもの）と `server`（届いたもの、順に）です。
