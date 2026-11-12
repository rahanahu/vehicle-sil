# gui/protocol

GUI サーバー（`gui/server`）と画面（`gui/web`）の間でやりとりする形の正本です。`examples/` の JSON は、サーバーの試験（`gui/server/tests/test_protocol_examples.py`）が偽のランチャーとの実際のやりとりから作り、毎回照合します。試験が作らないファイルが `examples/` に残っていても試験は落ちます。形を変えたら `UPDATE_PROTOCOL_EXAMPLES=1 uv run pytest gui/server/tests` で書き直し、画面の型（`gui/web/src/app/core/api.ts`）と、その単体試験（同じファイルを読む）を合わせます。プロセス ID、時刻、インスタンス、この PC のパス、決まらない数は、決まった値に置き換えてあります（`t_ms` は `seq × 10`）。

読む側は、知らない欄、知らない `type`、知らない事象（`event`）、知らない `code` を無視して通します。サーバーは欄を足すことがありますが、ここにある欄を消したり意味を変えたりするときは `vehicle_sil_gui`（今は 1）を上げます。ランチャーの事象（`event` の中身）は、ランチャーの `run --events` の形そのままです（[docs/simulation-config.md](../../docs/simulation-config.md) の「機械から使う」）。`heartbeat` もそのまま通します（画面は読み飛ばします。ランチャーが動いているかはサーバーが見て、要約の `launcher.silent_s` で知らせます）。participant の知らない `state`（`failed` など、ランチャーが足すもの）も、そのまま通して要約に重ねます。サーバーが形を確かめるのは、事象の一部の欄（下の `bad-event`）だけです。ほかの欄は、なくても型が違っても、そのまま通して要約に重ねます（`ws-loose-fields.json`）。画面は要約と事象を部分ごとに読み、読めない部分だけを読み飛ばします。

## 考え方

- **要約が正**。実行の要約（下の「要約」）に、画面を作り直すのに要るものを全部入れます。画面は `hello` と `session` を受けたら、手元の状態をその要約で置き換えます。
- 事象は届くたびに `event` で送ります。画面は `hello` の後に届いた事象を要約に当てて進めます。
- つなぎ直したとき（`hello` の後）に再送するのは、log の行（`from_seq` 以後で、サーバーが覚えている分）だけです。段階や participant の事象は再送しません（要約に入っているので）。
- サーバーが自分で持つ状態は、ランチャーから届いたものと、サーバーがランチャーにしたこと（起動、停止の依頼）だけです。

## HTTP

どの `api/` にも `Authorization: Bearer <token>` が要ります（token は、サーバーが表示した URL の `#token=` の後ろ）。`Host` は `127.0.0.1:<port>`（開発のときは `--dev-origin` のホストも）だけを受け付けます。`POST` は `Origin` がサーバー自身（開発のときは `--dev-origin`）のものだけを受け付け、本文があれば `Content-Type: application/json` が要ります。応答は JSON で、失敗は `{"error": {"code", "message", "detail"}}` です（`code` は機械が読む名前で、下の表にあります。`message` は人向けの英語の 1 文で、直し方を含みます）。

| 要求 | 応答 | 例 |
|---|---|---|
| `GET api/app` | サーバーの版、`instance`（起動ごとに変わる）、root、`initial_file`、ランチャー（`usable`、`problems`、`version --json` の内容）、上限 | `app.json` |
| `GET api/configs` | root の下の `.yaml` / `.yml`（`path` は root からの相対で `/` 区切り、`modified`、`size`、`simulation`: 先頭に `vehicle_sil_simulation` があるか）、`truncated` | `configs.json` |
| `POST api/check` `{file, disable}` | `{file, disable, exit_code, check}`。`check` は `check --json` の結果そのまま。照合が失敗しても 200 | `check-*.json` |
| `POST api/run` `{file, disable, speed, start_paused}` | `{session}`（始めた実行の要約） | `run.json` |
| `POST api/stop` `{force}` | `{session}`（今の要約。実行がなければ `null`） | `stop*.json` |
| `GET api/session` | `{session}`（なければ `null`） | `session-*.json` |

- `disable` に書けるのは、そのファイルの照合の結果にある participant 名だけです（`unknown-participant`）。
- `speed`（`null`、`"max"`、数）と `start_paused`（真偽）は、`api/app` の `launcher.version.features` に `speed`、`start-paused` があるときだけ値を送ります。ないときは欄を省くか `null` / `false` を送ります（ほかの値は 400 `unsupported`）。
- **開始**: 同時に送られた `api/run` は、1 つだけが始めます。ほかは、実行が始まりかけているか動いている間、409 `already-running` です。応答の要約を、画面はその場で当てます（後から届く `session` も同じ内容です）。
- **照合**: 同時の `api/check` は断らず、順に 1 つずつ実行します。待っている要求が多すぎるか、待ちが照合の時間の上限（`api/app` の `limits.check_timeout_s`）を過ぎたら 503 `check-busy` です。`api/run` も、participant 名を確かめるために同じ順番で照合することがあります。
- **停止**: 本文は `{"force": 真偽}`（省略は `false`。本文なしで `Content-Type` もない要求も `false`）。`force: false` は「順に止める」（ランチャーの標準入力に `{"command":"stop"}`）で、すでに頼んであれば何もしません。`force: true` は「待たずに止める」（まだ頼んでいなければ `stop` を 2 回、頼んであればもう 1 回）で、すでに頼んであれば何もしません。どちらも、実行がない、または終わっているときは 409 にせず、今の要約を返します（何度呼んでも同じ結果）。実行が始まりかけている間（`api/run` が応答する前）の停止は、ランチャーが起動したところで届けます（応答の要約は前の実行のもの。始まった実行の要約の `stop` に出ます）。止めた経過は、要約の `stop` と、ランチャーの `command` / `phase` の事象で見えます。標準入力を読まないランチャーは、これでは止まりません（サーバーを止めると、シグナルで止めます）。
- サーバーが終わり始めたら、`api/run` と `api/check` は 503 `shutting-down` です。

## 誤りの `code`

画面が見るコードです。名前は変えません（足すことはあります）。

| HTTP | `code` | いつ |
|---|---|---|
| 400 | `invalid-json` | 本文が JSON のオブジェクトでない |
| 400 | `invalid-field` | 欄の型か範囲が違う（`speed`、`start_paused`、`force`、`disable`） |
| 400 | `invalid-path` | `file` が root からの相対パスの文字列でない（空、長すぎる、`\`、NUL、`:`、対のないサロゲート） |
| 400 | `outside-root` | 絶対パス、`..`、root の外へのシンボリックリンク |
| 400 | `not-a-configuration` | `.yaml` / `.yml` の通常のファイルでない |
| 400 | `unknown-participant` | `disable` の名前が照合の結果にない |
| 400 | `unsupported` | ランチャーが `speed` / `start-paused` を持たないのに値を送った |
| 401 | `unauthorized` | トークンがないか違う（`WWW-Authenticate: Bearer` つき） |
| 403 | `forbidden-host` | `Host` が `127.0.0.1:<port>` でない |
| 403 | `forbidden-origin` | `Origin` がサーバー自身でない（`POST` と WebSocket は `Origin` が要る） |
| 403 | `cors-not-allowed` | `OPTIONS`（CORS は許さない） |
| 404 | `file-not-found` | `file` が root の下にない |
| 404 | `not-found` | そういう API や画面のファイルはない |
| 405 | `method-not-allowed` | 画面のファイルに `GET` 以外 |
| 409 | `already-running` | 実行が始まりかけているか動いている |
| 413 | `too-large` | 本文が 64 KiB を超える |
| 415 | `unsupported-media-type` | 本文があるのに `Content-Type: application/json` でない |
| 500 | `internal` | サーバーの不具合（例はない。起きたら標準エラーを見る） |
| 502 | `launcher-start` | ランチャーを起動できない |
| 502 | `launcher-output` | `check --json` が照合の結果を書かなかった |
| 503 | `launcher-unusable` | ランチャーが使えない（`api/app` の `launcher.problems` に理由） |
| 503 | `check-busy` | 照合の待ちが多すぎるか、待ちが長すぎる |
| 503 | `shutting-down` | サーバーが終わろうとしている |
| 503 | `too-many-connections` | WebSocket の数の上限（握手の応答） |
| 504 | `check-timeout` | 照合が時間の上限までに終わらなかった（止めた） |

例は `error-<code>.json` です（`internal` を除く）。`stop.json`、`stop-force.json`、`stop-ended.json` は停止の応答です。

## WebSocket（`ws`）

つないだら最初に `{"type": "auth", "token": "...", "session": <id>, "from_seq": <n>}` を送ります（`session` と `from_seq` は、再接続で持っている分を言うときだけ。0 以上の整数でなければ、ないものとして扱います）。トークンが違えば `error`（`code: unauthorized`）の後に、5 秒以内に `auth` が届かなければそのまま、閉じるコード 4401 で閉じます（理由の文は `unauthorized` か `auth-timeout`）。サーバーが終わるときは 1001 で閉じます。握手の段の拒否（`Host`、`Origin`、`too-many-connections`）は、ブラウザの JavaScript からは理由が見えません（1006 になります）。画面は、握手に失敗したら `GET api/app` を 1 回呼んで理由を知ります。認証した接続は 32 本まで（それを超える握手は 503 `too-many-connections`。握手の後、認証までの間に埋まったときは、閉じるコード 1013、理由の文 `too-many-connections`）。認証前の接続は別に 16 本までで、あふれたら最も古い認証前の接続を、閉じるコード 1013（理由の文は `too-many-unauthenticated`）で閉じて場所を空けます。1013 で閉じられた画面は、つなぎ直します。

認証の後に届くものは、この順です。

1. `hello`: `instance`、`session`（要約。なければ `null`）、`replay`（`from_seq`: 再送の始まり。`null` は最初から）。画面は状態をこの要約で作り直す。
2. `dropped`（あれば）: `from_seq` 以後の log の行のうち、サーバーが覚えていないので送れない数（source ごと）。
3. `event`: 覚えている log の行のうち `from_seq` 以後のものを、`seq` の順に。
4. `synced`: `{session, last_seq}`。再送の終わり。
5. 以後は、届くたびに `event`、`session`、`error`、`dropped`、`resync`。

`session` の `id` が `hello` の要約と違う、または `from_seq` が今のセッションのものでないときは、最初からの再送になります（`replay.from_seq` が `null`）。

| `type` | 内容 | 例 |
|---|---|---|
| `hello` | 上の 1 | `ws-*.json` の最初 |
| `event` | `{session, event}`。ランチャーの事象そのまま（サーバーが形を確かめて通したもの） | `ws-pass.json`、`ws-log-dropped.json` |
| `synced` | `{session, last_seq}` | `ws-pass.json` |
| `session` | `{session}`（要約）。実行を始めたとき、止めるよう頼んだとき、実行が終わったとき（`launcher.running` が `false`、`state` が `ended`。ランチャーの最後の事象の後）、ランチャーが黙ったとき（`launcher.silent_s` が変わったとき）と、また書き始めたとき | `ws-pass.json` |
| `error` | `{session, code, message, detail}`。サーバーが気づいたこと（下の表）。要約の `problems` にも残る | `ws-bad-lines.json` |
| `dropped` | `{session, reason, log, at_least, events}`（`ws-slow-client.json` の数は例）。この画面に送らなかったものの数。`log` は source ごとの log の行の数（送らなかった `log_dropped` の数も、その source に足す）、`at_least` はその数が下限でしかない source（もっと落ちているかもしれない）、`events` はほかの事象の数。source の名前は要約の `log` と同じです（65 個目からの source は `*`）。`reason` が `not-kept` はサーバーが覚えていない（再送の前）、`slow-client` は画面の受け取りが遅く、送る分から落とした。画面は、その source のログのその位置に「ここで N 行が落ちた」を出す | `ws-dropped.json`、`ws-slow-client.json` |
| `resync` | `{reason, message}`。画面の受け取りが遅く、落とせない知らせまであふれた。すぐ後に `hello` から送り直す | `ws-resync.json` |

受け取りが遅い画面には、まず log の行、`log_dropped`、知らない事象を落とし（`dropped`）、`time` は最後の 1 つにまとめ、`heartbeat` は数えずに落とします。それでも追いつかないときだけ `resync` です。

### `error` の `code`（サーバーの知らせ）

| `code` | 意味 |
|---|---|
| `unauthorized` | 認証の誤り（`session` は `null`。この後 4401 で閉じる） |
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
| `file`、`disable`、`speed`、`start_paused` | 始めたときの指定 |
| `started_at`、`ended_at` | 時刻（UTC、ISO 8601）。`ended_at` は終わるまで `null` |
| `state` | `running` か `ended`。`ended` は、ランチャーが終わり、その出力を読み終えた（または `output-left-open` で打ち切った）。その間（長くて 2 秒）は、`api/session` で `launcher.running: false` と `running` が見えることがある |
| `launcher` | `{pid, running, exit_code, signal, silent_s}`。`running` はランチャーのプロセスが動いているか。止められるかどうかはこれで決める。終わったら `exit_code`（シグナルで終わったら `null` で、`signal` にその番号。POSIX だけ）。`silent_s` は、動いているランチャーが何も書かない秒数（`--silent-after` を超えたときだけ、その倍数ごとに更新。書けば `null`） |
| `stop` | サーバーがランチャーに頼んだ停止: `none`、`requested`（順に止める）、`forced`（待たずに止める） |
| `phase` | 最後の `phase` 事象の段階（まだなければ `null`） |
| `last_seq`、`last_t_ms` | 最後に受け取った事象の `seq` と `t_ms` |
| `time` | 最後の `time` 事象（そのまま。なければ `null`）。大きすぎた欄があれば `omitted_fields` |
| `check` | `check` 事象の `check`（照合の結果を丸ごと。participant の `required`、`enabled`、`variables` を含む）。まだなければ `null` |
| `registry` | 最後の `registry` 事象から `event`、`seq`、`t_ms` を除いたもの（なければ `null`）。大きすぎた欄があれば `omitted_fields` |
| `participants` | participant ごとの今の状態。`participant` 事象の欄（`event`、`seq`、`t_ms` を除く）を名前ごとに重ねたもの（`name`、`state`、`pid`、`exit_code`、`signal` など。`failed` の `reason` は、後で `ended` が来ても残る）。初めて現れた順。`required` と `enabled` は `check` を見る。`stop_requested` の事象が来た participant には、サーバーが `stop_requested: true` を付け、後の状態でも残す（止めるよう求めた後の終わりを、失敗と区別するため）。大きすぎて入れなかった欄があれば、その名前の配列 `omitted_fields`（後の事象でその欄が届けば消える。例は `ws-large-fields.json`） |
| `participants_omitted` | 上限（256 人）を超えたので覚えていない participant の事象の数 |
| `findings`、`findings_omitted` | `finding` 事象（`event`、`seq`、`t_ms` を除く）と、上限を超えて覚えていない数 |
| `last_command` | 最後の `command` 事象から `event`、`seq`、`t_ms` を除いたもの（なければ `null`）。大きすぎた欄があれば `omitted_fields` |
| `result` | `result` 事象の `result`（なければ `null`） |
| `log` | source ごとの `{kept, dropped}`: サーバーが覚えている行の数と、落とした行の数（サーバーが覚えずに落とした行と、ランチャーが読む側の遅さで書けなかった行。後者は `log_dropped` の `sources`）。`log_dropped` の事象は、名前の挙がった source の log の、来た位置に覚え（つなぎ直した画面にも同じ位置に届く。1 つの事象は 1 回だけ送る）、画面はそこに「ここで N 行が落ちた」を出す |
| `problems`、`problems_omitted` | サーバーの知らせ（`error` と同じ `{code, message, detail}`）を起きた順に、と、上限を超えて覚えていない数 |

ランチャーが `result` を出さずに終わったときは、`launcher.running: false` と `exit_code`、`state: ended`、知らせの `no-result` になります。

## 上限

サーバーが覚える量には上限があり、超えた分は数えて知らせます。log は source ごとに直近 2000 行と 1 MiB（64 source まで。それより後の source は共有の `*`）、全体で 16 MiB。64 KiB を超える 1 行の log は覚えません（中継はします）。`check` と `result` は 4 MiB まで（超えたら `check` は `null`、`result` は入れ子でない短い欄だけ。例は `ws-kept-limit.json`）、`findings` は 1000 個と 1 MiB、`participants` は 256 人で 1 人 4 KiB、`phase`、`time`、`registry`、`command` は 1 つ 4 KiB。4 KiB を超えても、大事な欄は必ず入れます（participant の `name`、`state`、`pid`、`exit_code`、`signal`、`phase` の段階の名前、`time` の `sim_ms`、`registry` の `uri`、`command` の `command` と `accepted`。これだけで 4 KiB を超えるときは入れません）。`problems` は、ランチャーが書いたもの（標準エラーを除く）が 100 個、サーバー自身の知らせ（`kept-limit`、`no-result`、`output-left-open`、`stop-not-delivered`、`stop-escalated`、`launcher-silent`）が別に 50 個、標準エラーが別に最初の 20 行と最後の 30 行です。知らない事象は中継だけして覚えません（つなぎ直した画面には届きません）。
