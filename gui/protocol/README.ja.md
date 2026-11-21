# gui/protocol

[English](README.md)

GUI サーバー（`gui/server`）の HTTP と WebSocket の API の、公開する仕様です。リクエストとレスポンスの形、WebSocket のメッセージ、エラーの `code`、上限を載せています。サンプルの画面（`gui/web`）も、クライアントのライブラリ（`gui/client`）を通して、この API だけを使います。自分の画面やツールも、このドキュメントと例に従えば同じことができます。

| 知りたいこと | 節 |
|---|---|
| フィールドやイベントが増えたときの扱い | 「バージョンと互換性」「読む側のルール」 |
| トークンの送り方、`Host` と `Origin` の検査 | 「認証と Origin」 |
| 照合、開始、停止、時間のコマンドのリクエスト | 「HTTP」 |
| 拒否されたリクエストの `code` | 「エラーの `code`」 |
| 実行の経過を受信する方法 | 「WebSocket（`ws`）」「要約（`session`）」 |
| ECU の変数の一覧、値、書き込み | 「ECU の変数」 |
| シナリオ、操作の記録、ダッシュボードのファイル | それぞれの節 |
| サーバーが覚える量、リクエストの大きさ | 「上限」 |

## バージョンと互換性

API のバージョンは `vehicle_sil_gui`（今は 3）です。サーバーは `GET api/app` のレスポンス、WebSocket の `hello`、`--json` で起動したときの 1 行に書きます。

| 変更 | バージョン | 読む側がすること |
|---|---|---|
| フィールド、`type`、`code`、イベント、リクエスト、認証のメッセージの任意のフィールドを追加する | 上げない | 知らないものを無視するか、そのまま通す |
| フィールドやリクエストを消す、型や意味を変える、送る順のルールを変える | `vehicle_sil_gui` を上げる | 知らないバージョンの `api/app` と `hello` を読まずに、バージョンが違うと通知する |

- 無視するか通すものは、知らないフィールド、知らない `type`、知らないイベント（`event`）、知らない `code` と、値の集合が増えうるフィールド（`phase`、`state`、`result` など）の知らない値です。
- バージョンが違うときは、待っても直らないので再接続しません。ライブラリは `api/app` で `ProtocolError`、`hello` で `session.incompatible` を立てて止まります。
- **受信し直すもの**: WebSocket のメッセージが JSON として読めない、または `hello` と `session` の要約の中心のフィールド（`id`、`state`、`launcher`、`stop`）が読めないときは、何が抜けたかわからないので、再接続して `hello` から受信し直します。要約を正とします。続けて起きるたびに、再接続するまでの間隔を延ばします。
- **頼んだときだけ変わる形**: 前の形のままでは重いものは、クライアントが認証のメッセージで頼んだときだけ形を変えます（`session_without_check`）。頼まないクライアントには、いままでの形で送ります。
- ランチャーのイベント（`event` の中身）は、ランチャーの `run --events` の形そのままです。そのバージョン（`hello` のイベントの `vehicle_sil_events`）はランチャーが決めます（[docs/simulation-events.md](../../docs/ja/simulation-events.md) の「プログラムから使う」）。
- ランチャーのオブジェクト（要約と `api/check` のチェックと実行の結果、指摘、ランチャーのイベント）は、キーをすべて中継します。クライアントのライブラリは、画面に要るキーに型を付け、ランチャーのオブジェクト全体を `raw` に残します（[gui/client/README.md](../client/README.ja.md#ランチャーのキーraw) の「ランチャーのキー（`raw`）」）。
- エラーの `code` の名前は変えません（追加することはあります）。`message` は人向けの英語の 1 文で、文言は変わることがあります。プログラムは `code` で分けます。

## 認証と Origin

| 検査 | 受け付けるもの |
|---|---|
| 待ち受けるアドレス | `127.0.0.1` だけ |
| `Host` | `127.0.0.1:<port>` だけ（開発のときは `--dev-origin` のホストも） |
| トークン（HTTP） | `Authorization: Bearer <token>` |
| トークン（WebSocket） | 最初のメッセージ（`auth`） |
| `Origin`（`POST` と WebSocket のハンドシェイク） | サーバー自身（`http://127.0.0.1:<port>`。開発のときは `--dev-origin`）のものだけ |
| CORS | 許可しない。別の origin のページからは使えない |

- トークンは起動のたびに作られ、サーバーが表示した URL の `#token=` の後ろにだけあります。
- サーバーが配信する画面（サンプルの画面と、`--web` で配信する自分の画面）は、開いた URL の `#token=` を読んで、すぐに URL から消します（クライアントのライブラリの `takeToken`）。
- トークンを URL の query やログに載せません（query のトークンは受け付けません）。
- `Origin` は、ブラウザが自分で付けます。ブラウザの外（Node、Python など）から使うときは、サーバー自身の origin を `Origin` に書きます。例は、リポジトリの [examples/gui_client/run.mjs](../../examples/gui_client/run.mjs) です（ビルド済みのツールのアーカイブには入っていません）。

## 読む側のルール

読む側は、知らないフィールド、知らない `type`、知らないイベント（`event`）、知らない `code` を無視して通します。要約とイベントは部分ごとに読み、読めない部分だけを読み飛ばします。

サーバーは、ランチャーのイベントを、ランチャーが書いた形のまま送ります。

- `heartbeat` もそのまま通します。画面は読み飛ばします。ランチャーが動いているかはサーバーが見て、要約の `launcher.silent_s` で通知します。
- participant の知らない `state`（`failed` など、ランチャーが追加するもの）も、そのまま通して要約に重ねます。
- サーバーが形を確認するのは、イベントの一部のフィールド（下の `bad-event`）だけです。ほかのフィールドは、なくても型が違っても、そのまま通して要約に重ねます（`ws-loose-fields.json`）。

要約とイベントの関係は、次のとおりです。

- **要約を正とします**。実行の要約（下の「要約（`session`）」）に、画面を作り直すのに必要なものを全部入れます。画面は `hello` と `session` を受信したら、手元の状態をその要約で置き換えます。
- イベントは、ランチャーが書くたびに `event` で送ります。画面は `hello` の後に受信したイベントを要約に反映して進めます。
- 再接続したとき（`hello` の後）に再送するのは、log の行（`from_seq` 以後で、サーバーが覚えている分）だけです。段階や participant のイベントは、要約に入っているので再送しません。
- サーバーが自分で持つ状態は、ランチャーから受信したものと、サーバーがランチャーにしたこと（起動、停止の依頼）だけです。

## HTTP

どの `api/` にも、次の条件があります。

- `Authorization: Bearer <token>` が必要です（token は、サーバーが表示した URL の `#token=` の後ろ）。
- `Host` は `127.0.0.1:<port>`（開発のときは `--dev-origin` のホストも）だけを受け付けます。
- `POST` は `Origin` がサーバー自身（開発のときは `--dev-origin`）のものだけを受け付けます。本文があれば `Content-Type: application/json` が必要です。
- レスポンスは JSON で、失敗は `{"error": {"code", "message", "detail"}}` です。`code` はプログラムが読む名前で、下の「エラーの `code`」の表にあります。`message` は人向けの英語の 1 文で、直し方を含みます。

### エンドポイント

| リクエスト | レスポンス | 例 |
|---|---|---|
| `GET api/app` | サーバーとランチャーの情報、変数を読み書きできるか、上限 | `app.json` |
| `GET api/configs` | root の下の `.yaml` / `.yml` の一覧 | `configs.json` |
| `GET api/config?file=<configuration file>` | ファイルの文と、そのバイト列の SHA-256: `{file, text, stamp}`（下の「構成ファイルの文」） | `config.json` |
| `POST api/check` `{file, vehicle_sil_run_request, disable, enable, manual, scenario, variant, params, strict, connect, time_policy, speed, start_paused}` | `{file, disable, manual, scenario, exit_code, check}` | `check-*.json` |
| `POST api/check` `{file, text, variant, strict}` | ファイルの保存していない文の照合。プロセスを起動しない。レスポンスは上と同じ | `check-text.json` |
| `POST api/describe` `{file, variant}` | `{file, variant, exit_code, check}`: 保存したファイルを照合し、実行ファイルの説明を取り直す（文の照合のため） | `describe.json` |
| `POST api/config` `{file, text, stamp}` | `{file, stamp}`: 文をそのファイルとして保存する。既定では、実行されるコマンドを変えない編集だけ（下の「文を保存する（`POST api/config`）」） | `save-config.json`、`error-commands-changed.json` |
| `POST api/run` `{file, vehicle_sil_run_request, disable, enable, manual, scenario, variant, params, strict, connect, time_policy, speed, start_paused}` | `{instance, session}`（始めた実行の要約）。または、所見つきの 422 `check-failed`（下の「照合と開始」） | `run.json`、`run-connect.json`、`error-check-failed.json` |
| `POST api/connections` `{file, network}` | `{file, network, exit_code, connections}`: そのネットワークを、このコンピューターでどこにつなげるか（下の「ネットワークを外につなぐ」） | `connections.json` |
| `POST api/stop` `{force}` | `{session}`（今の要約。実行がなければ `null`） | `stop*.json` |
| `POST api/stop-session` `{session, instance, force}` | 対象の `{session}`、または 409 `session-changed` | — |
| `POST api/time` `{command, ms, at_ms, value, id, session, instance}` | `{session_id, command}`（ランチャーに送ったコマンド。`id` があればそれも） | `time.json` |
| `POST api/write` `{session, writes, at_ms, late}` | `{session_id, operation}`（操作の番号。結果は WebSocket の `operation`） | `write.json` |
| `GET api/session` | `{instance, session, allowed}`（`session` は実行がなければ `null`。`allowed` は下の「サーバーが受け付けること（`allowed`）」） | `session-*.json` |
| `GET api/dashboard?file=<configuration file>` | 設定ファイルの隣のダッシュボードのファイル: `{file, path, found, errors, dashboard}` | `dashboard*.json` |
| `GET api/scenarios?file=<configuration file>` | 設定ファイルの隣の `scenarios` のシナリオ: `{file, directory, found, scenarios, truncated}` | `scenarios.json` |
| `GET api/recording` | いまの（最後の）実行の、反映された書き込みをシナリオの YAML にしたもの: `{recording}`（実行がなければ `null`） | `recording.json` |
| `GET api/timeline/<session の id>` | シナリオの実行の時間の軸のファイル。ランチャーが書いたそのまま（オブジェクトで包まない） | — |

- `api/app` のレスポンスは、サーバーのバージョン、`instance`（起動ごとに変わる）、root、`initial_file`、ランチャー（`usable`、`problems`、`version --json` の内容）、変数を読み書きできるか（`variables`: `available`、できない理由 `reason`）、上限（`limits`）です。`limits` には `speed_min`、`speed_max`、`step_ms_max`、変数の `watches`、`watch_variables`、`watch_period_ms_min`、`watch_period_ms_max`、`writes_per_request`、`write_at_ms_max`、`write_timeout_s` などがあります。
- `api/configs` の各ファイルは、`path`（root からの相対で `/` 区切り）、`modified`、`size`、`simulation`（先頭に `vehicle_sil_simulation` があるか）を持ちます。一覧の全体に `truncated` があります。
- `api/check` の `check` は `check --json` の結果そのままです。照合が失敗しても 200 です。
- `api/write` は下の「ECU の変数」、`api/scenarios` と `scenario` は下の「シナリオ」、`api/recording` は下の「操作の記録（`api/recording`）」、`api/timeline` は下の「シナリオの実行の時間の軸（`api/timeline`）」、`api/dashboard` は下の「ダッシュボードのファイル」にあります。`api/dashboard` の `file` は `api/check` と同じ検査を通ります（`invalid-path`、`outside-root`、`not-a-configuration`、`file-not-found`）。
- サーバーが終了を始めたら、`api/run`、`api/check`、`api/describe`、`POST api/config`、`api/connections`、`api/write` は 503 `shutting-down` です。

### `disable` と `manual`

| 書いたもの | レスポンス |
|---|---|
| そのファイルの照合の結果にない participant 名 | 400 `unknown-participant` |
| 1 つの配列に同じ名前を 2 度 | 400 `invalid-field` |

- 同じ名前を `disable` と `manual` の両方に書いたものと、ファイルにない名前の `enable` は、ランチャーが断ります。照合の所見（`api/check`）か 422 `check-failed`（`api/run`）に出ます。

`manual` に書いた participant は、ランチャーに要求ファイルで渡り、ランチャーは起動せずに参加を待ちます（[docs/simulation-config.md](../../docs/ja/simulation-config.md) の「participant を自分で起動する」）。

- 設定ファイルの `manual: true` は、照合の結果の `participants[].manual` に出ます。
- 起動するコマンドは、その participant のイベント（`state` が `manual`）の `command`、`working_directory`、`command_line` で届き、要約の `participants` に残ります（例は `check-manual.json`、`ws-manual.json`）。
- registry がループバックのアドレスでない設定では、起動するときに設定する環境変数が、同じイベントの `environment`（`{"VEHICLE_SIL_LISTEN_ON_NETWORK": "1"}`）で届き、同じく残ります。サーバーは中身を見ずに通します。

### `speed` と `start_paused`

`speed`（`"max"`、数）と `start_paused`（`true`）は、選ぶときだけ書きます。選ばないときはフィールドを省きます。`null` は、どのフィールドでも断ります（400 `invalid-field`）。

- `speed` の数の範囲は `limits.speed_min` から `limits.speed_max` までです（ランチャーと同じ）。
- `speed` を省くと、設定ファイルの `speed` です。

### ネットワークを外につなぐ（`connect`、`time_policy`、`api/connections`）

`can` と `ethernet` のネットワークは、ランチャーのブリッジを通して実機のバスにつなげます（[docs/simulation-run.md](../../docs/ja/simulation-run.md#バスを外につなぐ--connect) の「バスを外につなぐ（`--connect`）」）。画面は 3 段で行います。

1. **一覧を取る**: `POST api/connections` `{"file": "simulation.yaml", "network": "ETH_EXT"}` は、ランチャーの `connections --json --network ETH_EXT` を走らせ、`{file, network, exit_code, connections}` を返します。`connections` はランチャーの JSON そのままです（docs/simulation-run.md の「`connections --json`」）。`networks[]` はこの OS の方式（backend）ごとに 1 つずつで、`list.targets[]` に `value`、`label`、`detail`、`usable`、`reason` があります。一覧は、利用者がそのネットワークを開いたときだけ頼みます。そのネットワークのブリッジだけが起動するので、SIL Kit だけを使うコンピューターに Npcap の確認は出ません。`network` は、そのファイルのネットワーク（照合の `networks[].name`）です。ネットワークはどの variant でも同じなので、variant なしで一覧を取ります。ファイルにない名前は、ランチャーの使い方の誤りを 400 `unknown-network` に読み替えます。メッセージにランチャーの文が入り、`detail` は `{network, findings}`（ランチャーの所見）です。一覧は照合と同じ順番を待ちます（`api/check` と同じく 503 `check-busy`）。例: `connections.json`。
2. **選ぶ**: `api/check` と `api/run` に `connect` `{"<network>": "<target>"}` を、一覧の `value` で送ります。`"none"` はネットワークを SIL Kit だけに置き、一覧は要りません。
3. **照合して始める**: 照合は、各ネットワークのつなぎ先（`networks[].outside`）と、実行が実時間を守るか（`time_lock`）を言います。`time_policy`（たとえば `"free"`）はその固定を外します。ネットワークはつながったままで、照合に注記 `outside-not-real-time` が出ます。方針が何を意味するか、一時停止、ステップ、速さの変更を受けるか（要約の `allowed`）は、ランチャーが決めます。サーバーは `connect` と `time_policy` を要求ファイルで渡すだけで、時間の規則を持ちません。例: `check-connect.json`、`run-connect.json`。

**守り（`target-not-listed`）**: ファイルにあるネットワークの `"none"` 以外の先は、そのファイルのそのネットワークの直近の一覧に、ちょうど同じ文字列で `usable: true` として載っていて、その一覧の後にファイルが変わっていないときだけ渡します。`api/check` は照合の後に照合と同じ順番の中で、`api/run` はランチャーを起こす直前に照らします。そうでなければ 400 `target-not-listed`（`detail` は `{network, target}`。メッセージに一覧の使える値の先頭を挙げます）で、何も始めません。ネットワークの一覧を取り直し、そこから選びます。こうして、ブラウザーからの値は、ランチャーがこのコンピューターで一覧に載せたときだけ、ブリッジのデバイスに届きます。照らすのは文字列そのものです。ランチャーは `packet:enp3s0` と `enp3s0` を同じ先として受けますが、ここでは一覧の `enp3s0` と別のものなので、一覧の `value` のとおりに書きます。あるネットワークの新しい一覧を頼むと、前の一覧はその場でなくなり、答えのなかった一覧（502、504、サーバーの終わり）は一覧を残しません。例: `error-target-not-listed.json`。

- `connect` にあってファイルにないネットワークは、何もつなぎません。サーバーはランチャーに任せ、ランチャーは台の要求ファイルのときと同じく、注記 `connect-network-unused` を出して外します（コマンドラインの `--connect` では、そのようなネットワークは誤りです）。run の check から起動までの間にファイルが変わったときは、そのネットワークがファイルに足されているかもしれないので、何かをつなぐ run を `target-not-listed` で断ります。

- サーバーは、実行をまたいで選んだ先を覚えません。覚えたい画面は自分で覚えます。
- ランチャーの `outside` イベントと、`state` イベントの `outside[]` は、そのまま渡します。

### 照合と開始（`api/check`、`api/run`）

- **開始**: 同時に送られた `api/run` は、1 つだけが始めます。ほかは、実行が始まりかけている間は 409 `starting`、動いている間は 409 `already-running` です。レスポンスの要約を、画面はその場で手元の状態に反映します（後から届く `session` も同じ内容です）。
- **照合のあとの開始**: `api/run` は、まず実行を成す内容（実行と同じ要求ファイル）を、`api/check` と同じ順番で照合します。照合が通らなければ（`result` が `passed` でなければ）実行は始めず、422 `check-failed` を返します。`detail` は `{result, findings}` で、`result` は照合の `result`（`failed`、`unreadable`、`usage`）、`findings` は同じリクエストで `api/check` が返す `findings`（同じオブジェクト）です。警告や注記があるだけで通る照合は、実行を始めます。所見を直してから、もう一度 `api/run` を送ります。何も始める前に断るので、止めるものはありません。例: `error-check-failed.json`。
- **意図と要求ファイル**: 本文は、`file`（このサーバーのフィールド。root からの相対の構成ファイル）と、ランチャーの要求ファイル（`schema/run-request.schema.json`。[docs/simulation-run.md](../../docs/ja/simulation-run.md#要求ファイル--request) の「要求ファイル（`--request`）」）のキーです。名前と値の形は同じです。`variant`（文字列）、`params`（パラメーター名から文字列、数、真偽への object）、`scenario`、`disable`、`enable`、`manual`（participant 名の配列）、`strict`（真偽）、`connect`（ネットワーク名から先への object）と `time_policy`（文字列）（上の「ネットワークを外につなぐ」）、`speed` と `start_paused`（`api/check` でも。ランチャーの照合が、時間とシナリオの規則に使います）です。`vehicle_sil_run_request` は `1` なら、`$schema` は文字列なら送れます（後者は捨てます）。キーの名前と値の形は要求ファイルと同じで、`file` が加わります。`scenario` は `api/scenarios` の一覧の名前で、構成ファイルのディレクトリからのパスではありません。サーバーは、各値の形（本文は 64 KiB まで。`api/check` は 6 MiB + 64 KiB まで）、パス（`file`、`api/scenarios` の一覧と照らす `scenario`）だけを調べ、意図を自分の要求ファイル（この利用者だけが読める。照合か実行が終わったら消す）に書いて、ランチャーに `--request <そのファイル>` を渡します。値がそのファイルで何を意味するか（その variant があるか、`disable` と `manual` を併せてよいか、シナリオにその `params` があるか）は、ランチャーが言います。所見（`api/check`）か 422 `check-failed`（`api/run`）に出ます。要求ファイルについての所見は、そのファイルを「the request」と呼びます（一時ファイルは、読むときにはもうありません）。`scenario` のない `params` は断りません。どの要求ファイルでも、ランチャーは `params` をシナリオを動かすときだけ使い、それ以外では無視します。`disable` と `manual` に使える名前は、その variant の照合の結果のものです。
- **選ばないことは、キーを書かないこと。** `null` は断ります（400 `invalid-field`）。実行をする側のキー（`control`、`timeline`、`log_dir` など）は知らないフィールドです（400 `invalid-field`）。サーバーが決めます。送る `scenario` は、`api/scenarios` が返す `path`（root からの相対）です。サーバーは、要求ファイルが読む基準である構成ファイルのディレクトリからの相対に直して書きます。`request` の feature がないランチャーは使えません（`api/app` の `launcher.problems` に出ます）。
- **照合**: 同時の `api/check` は拒否せず、順に 1 つずつ実行します。待っているリクエストが多すぎるか、待ちが照合の時間の上限（`api/app` の `limits.check_timeout_s`）を過ぎたら 503 `check-busy` です。`api/run` も同じ順番で照合します。

### 構成ファイルの文（`api/config`、`api/check` の `text`、`api/describe`）

構成ファイルを編集し、保存する前に照合する画面のためのものです。

- **読む**: `GET api/config?file=<configuration file>` は `{file, text, stamp}` を返します。`text` はファイルのバイト列を UTF-8 として読んだそのままです。BOM は先頭に残り、改行（CRLF、改行のない最後の行）も変えません。`stamp` はそのバイト列の SHA-256（小文字の 16 進数 64 桁）です。`file` は `api/check` と同じ検査を通り、さらに `api/configs` が言う構成ファイルでなければなりません。`.` で始まる部分のあるパス（リンクをたどった先も）と、先頭の 4096 バイトに `vehicle_sil_simulation` のないファイル（UTF-16 のファイルを含む）は 400 `not-a-configuration` で、根の下のほかの YAML のファイルを画面のために読むことはありません。1 MiB を超えるファイルは 413 `config-too-large`、UTF-8 の文でないファイル（NUL のバイト、UTF-8 でないバイト列）と読めないファイルは 422 `config-unreadable` です。例: `config.json`。
- **文を照合する**: `text`（文字列）のある `api/check` は、その文を `file` の中身として照合します。文の中の相対パスは `file` のディレクトリから、所見は `file` の名前で出ます。`file` は、`api/config` と同じく、root の下にある構成ファイルです。`text` があるとき、本文が受けるのは `file`、`variant`、`strict`（と `$schema`、`vehicle_sil_run_request`）だけです。ほかのフィールドは 400 `invalid-field` です。文の照合は構成を照合するもので、実行の選択を照合するものではないからです。レスポンスの形は `text` がないときと同じです（`disable` と `manual` は `[]`、`scenario` は `null`）。`text` は UTF-8 で 1 MiB までです（413 `text-too-large`）。例: `check-text.json`。
- **プロセスは起動しない**: ランチャーは `--text-from-stdin` と `--descriptions` で文を照合します（[docs/simulation-check.md](../../docs/ja/simulation-check.md#保存した説明--write-descriptions--descriptions) の「保存した説明」）。各 participant の `--describe` の出力は、サーバーが保存したファイルの照合から取っておいた説明から取ります。participant の `command` と実行ファイルがそのときのままなら使い、そうでない participant は `not_described` です。サーバーは説明を `file` と `variant` ごとに自分の一時ディレクトリに持ちます（終わるときに消します。どのレスポンスにも名前は出ません）。説明がないとき、または説明を作った後に保存したファイルが変わったときは、先に保存したファイルを（その `variant` だけで）照合します。この照合は、実行の画面の照合と同じく、各実行ファイルを `--describe` で起動します。2 つの照合は、照合の順番の 1 回分で行います。その照合がファイルやその variant を読めないとき（`unreadable`、`usage`。文にだけある variant など）と、エラーで終わったとき（`check-timeout`、`launcher-output`、`launcher-start`）は、説明なしで文を照合し、ファイルが変わるか `api/describe` が頼むまで、文のために保存したファイルを照合し直しません（何が起きたかは `api/describe` の答えで分かります）。保存した説明のない participant は `not_described` で、全員がそうでも照合は `passed` のままです。どれを比べなかったかは `participants[].status` で分かり、`api/describe` で説明を取り直します（文にだけある participant は、先にファイルを保存します）。
- **説明を取り直す**: `POST api/describe {file, variant}`（`file` は `api/config` と同じ）は保存したファイルを照合し、説明を保存し直します（実行ファイルを作り直した後など）。レスポンスは、その照合の `{file, variant, exit_code, check}` です（`variant` は、指定がなければ `null`）。例: `describe.json`。
- **サーバーが保存したファイルについて覚えること**: `disable` と `manual` に使える名前と、`api/connections` の一覧は、保存したファイルのものです。文の照合は、これらを使わず、変えません。`text` のない `api/check` と `api/run` は説明を保存しません。
- どちらも、`api/check` と同じ照合の順番を通ります（`check-busy`、`check-timeout`）。

### 文を保存する（`POST api/config`）

`POST api/config {file, text, stamp}` は、`text` を `file` の中身として書きます。編集を保存してよいかは、ランチャーの決まりです。サーバーは、保存したファイルを相手に `check --compare-commands` で文を照合させ（[docs/simulation-check.md](../../docs/ja/simulation-check.md#実行されるコマンドを比べる--compare-commands) の「実行されるコマンドを比べる」）、その照合の `commands` と `result` だけを読みます。サーバー自身は YAML を比べません。既定で保存するのは、`commands` が `"same"` の編集だけです。ネットワーク、ネットワークの写像、`external` など、ランチャーが比べないキーの編集です。サーバーの選択肢で変わります。`--no-save` では何も保存しません。`--allow-command-changes` では、実行されるコマンドを変える編集と新しいファイルも保存します（そのとき、トークンを持つ者は、サーバーを動かすユーザーとして任意のコマンドを実行できます。[docs/gui.md](../../docs/ja/gui.md#セキュリティ) の「セキュリティ」）。どれが効いているかは `allowed.save` が言います（下の「サーバーが受け付けること（`allowed`）」）。

本文のフィールドはこの 3 つだけです（ほかのフィールドは 400 `invalid-field`）。`text` は UTF-8 で 1 MiB まで（413 `text-too-large`。本文は `api/check` と同じ大きさまで）。`stamp` は `GET api/config` がそのファイルに返した `stamp`、新しいファイルなら `null` です。サーバーは次の順に調べ、最初に断るところで止まります。

1. `--no-save` なら 403 `save-disabled`。
2. 場所。`stamp` があれば、`file` は、`api/config` と同じく、いまある構成ファイルです（400 `not-a-configuration`、404 `file-not-found` など）。`stamp` が `null` で `--allow-command-changes` がなければ、403 `commands-changed` です（新しいファイルには比べる保存したファイルがない）。`--allow-command-changes` のとき、新しいファイルは `.yaml` か `.yml` で終わり、そのディレクトリーがあること（404 `file-not-found`）。すでにあるファイルは 409 `config-changed` です。いまあるファイルも新しいファイルも、`api/configs` が構成ファイルを一覧に出す所にあること。リンクをたどった後も root の下（400 `outside-root`）、リンクのディレクトリーを通らない（ファイルがリンクなのはよい）、`.` で始まる部分がない、root から 6 階層以内、送った名前でもリンクをたどった後でも、ダッシュボードのファイル（`*.dashboard.yaml`）でも `scenarios` という名前のディレクトリーの中でもない（400 `not-a-configuration`）。`text` は NUL を含まず、先頭の 4096 バイトに `vehicle_sil_simulation` があること（422 `config-unreadable`）。サーバーが書くのは、`api/configs` が一覧に出し、`api/config` が読み直せる構成ファイルだけです。
3. 印。ファイルのいまのバイト列の SHA-256 が `stamp` と違えば 409 `config-changed`。読み直し、その文に編集をし直して、新しい印で保存します。
4. ランチャーが、文を `file` の中身として、`--text-from-stdin` と保存した説明で（プロセスは起動しません）、`--compare-commands <file>` つきで（新しいファイルではなし）照合します。`result` が `unreadable` か `usage` の文は、選択肢によらず 422 `config-unreadable` です。`--allow-command-changes` がなければ、`"same"` でない `commands`（`"changed"`、`"not_compared"`、`null`）は 403 `commands-changed` です。比べられないものは保存しません。メッセージにランチャーの所見（`commands-changed`、`commands-not-compared`）が入り、`detail` はその照合の `{result, commands, findings}` です。文のほかの所の誤り（`result` が `failed`）では、保存は止まりません。
5. ファイルをもう一度解き（シンボリックリンクがいまは別のファイルを指すなら 409 `config-changed`。手順 4 のランチャーは、その解いたファイルと比べます）、もう一度印を比べてから、文をそのまま UTF-8 のバイト列で（BOM、CRLF、改行のない最後の行は送られたまま）同じディレクトリーの新しいファイル（名前は `.vehicle-sil-gui-save-` で始まり、一覧に出ない）に書き、それを `file` の場所に置き換えます。いまあるファイルはモードを保ちます。シンボリックリンクは同じファイルを指したままで、そのファイルが新しい中身になります。置き換えられないファイル（Windows でほかのプログラムが開いている、このユーザーには読み取り専用、ディレクトリーに書けない）は 409 `config-busy` で、新しいファイルは消します。新しいファイルは、手順 2 の後にほかのプログラムが作ったファイルを置き換えません（409 `config-changed`）。ハードリンクで置き、ハードリンクのないファイルシステムでは、先に空のファイルを作り、あればそこで失敗します。POSIX では、置き換えの後にディレクトリーを同期します。

レスポンスは `{file, stamp}` で、保存した中身の印です。保存は照合の順番を 1 回使います（照合と同じく 503 `check-busy`）。サーバーが古い中身について覚えたものは捨てます。説明は次の文の照合の前に取り直すので、保存したファイルの実行ファイルがそのとき説明を出します（`--allow-command-changes` では、新しいコマンドがそこで `--describe` つきで動きます）。`connect` には `api/connections` の一覧を取り直します。2 回目の印の比べは、照合と置き換えの間にほかのプログラムがファイルを変えられる時間を狭めますが、なくしはしません。例: `save-config.json`、`error-commands-changed.json`。

### 停止（`api/stop`）

GUI Client は `api/session` と `api/run` の成功応答に `instance` を必要とします。応答にない場合は操作を中断し、同じリリースの GUI サーバーを使うよう案内します。開始要求はサーバーに届いている場合があるため、再試行の前に現在の実行を確認してください。

`api/stop-session` は `session` と `instance` を原子的に照合し、一致しなければ 409 `session-changed` を返して何もしません。`force` の意味は `api/stop` と同じです。GUI Client は対象を特定してからこの口を使い、開始の応答を失った場合や旧サーバーでは停止を送れなかったことを報告します。現在の実行を確認してから再度操作してください。

本文は `{"force": boolean}` です。省略は `false` で、本文なしで `Content-Type` もないリクエストも `false` です。`force` 以外の欄（または null）は、ほかの口と同じく 400 `invalid-field` で断り、何も止めません。

| `force` | すること | すでに頼んであるとき |
|---|---|---|
| `false` | 順に止める（ランチャーの標準入力に `{"command":"stop"}`） | 何もしない |
| `true` | 待たずに止める（まだ頼んでいなければ `stop` を 2 回、頼んであればもう 1 回） | 何もしない |

- どちらも、実行がない、または終わっているときは 409 にせず、今の要約を返します（何度呼んでも同じ結果）。
- 実行が始まりかけている間（`api/run` がレスポンスを返す前）の停止は、ランチャーが起動したところで渡します。レスポンスの要約は前の実行のもので、始まった実行の要約の `stop` に出ます。
- 止めた経過は、要約の `stop` と、ランチャーの `command` / `phase` のイベントで見えます。
- 標準入力を読まないランチャーは、これでは止まりません（サーバーを止めると、シグナルで止めます）。

### 時間のコマンド（`api/time`）

本文は次のいずれかのコマンドです。表の下で説明するフィールドも追加できます。

| 本文 | 値の範囲 |
|---|---|
| `{"command": "pause"}` | |
| `{"command": "resume"}` | |
| `{"command": "step", "ms": <integer>}` | 1 から `limits.step_ms_max` |
| `{"command": "run_until", "at_ms": <整数>}` | 0 から `limits.step_ms_max` |
| `{"command": "speed", "value": <"max" or a number>}` | `speed` と同じ範囲 |

どの本体にも `"id"`（0 から 2^53 までの整数）を付けられます。サーバーはそれをコマンドの行に入れてランチャーに渡し、ランチャーはその行に答える `command` イベントの `id` に返します（`id` のない行は `null`）。画面は、自分のコマンドへの答えを、ほかの画面やランチャーの標準入力のコマンドへの答えと見分けるのに使います。サーバーは `id` を読みません。レスポンスのコマンドにも `id` が入ります。そのような数でない `id` は 400 `invalid-field`です。

特定の実行にコマンドを送るには、`session`（正の整数）と `instance`（`api/run`、`api/session` の応答か WebSocket の `hello` にあるサーバーの識別子）を両方指定します。サーバーはランチャーに書く直前にこの組を照合します。一致しない場合、実行がない場合、次の実行を開始中の場合は 409 `session-changed` を返します。2 つの値はランチャーへのコマンドや応答の `command` に含めません。片方だけの指定、不正な値、未知のフィールドは 400 `invalid-field` です。両方を省略すると、サーバーが処理した時点の実行にコマンドを送ります。

サーバーは、動いている実行のランチャーの標準入力に 1 行で書き、レスポンスを待たずに返します。止まっているランチャーでリクエストが固まらないようにするためです。

- 受け付けたかどうかと理由は、ランチャーの `command` のイベント（`accepted`、`message`、断った理由のコード `reason`、`id`、`step` の実際に進む `ms` とその後に止まる時刻 `at_ms`、`run_until` の頼んだ時刻をタイムステップに切り上げた `at_ms`）で届きます。コマンドが反映されたことは `time` のイベントで届きます。どちらも、要約の `last_command` と `time` に残ります。
- 対象の指定がないか照合が通った場合に、ランチャーがコマンドを受け取れなければ 409 `not-running` です。実行の終了や開始中、ランチャーの入力が閉じた場合、最後の `state` が `not-running` の場合が該当します。`allowed` にあるほかの理由はすでに変わっていることがあるため、ランチャーがコマンドに答えます。
- ランチャーが入力を読まないままのときも、リクエストは長くて約 2 秒で返ります（コマンドはランチャーの入力に残ります）。
- コマンドの意味（一時停止はステップの境目で反映される、ステップ実行は一時停止の間だけ、など）はランチャーのルールです（[docs/simulation-events.md](../../docs/ja/simulation-events.md) の「標準入力のコマンド」）。

## エラーの `code`

画面が見るコードです。名前は変えません（追加することはあります）。

| HTTP | `code` | いつ |
|---|---|---|
| 400 | `invalid-json` | 本文が JSON のオブジェクトでない |
| 400 | `invalid-field` | フィールドの型か範囲が違う、または `api/check` か `api/run` の本文に、そのリクエストが受けないフィールドがある（対象は表の下） |
| 400 | `invalid-path` | `file` が root からの相対パスの文字列でない（空、長すぎる、`\`、NUL、`:`、対のないサロゲート） |
| 400 | `outside-root` | 絶対パス、`..`、root の外へのシンボリックリンク |
| 400 | `not-a-configuration` | `.yaml` / `.yml` の通常のファイルでない。`api/config`、`api/check` の `text`、`api/describe` では、`.` で始まる部分のあるパスと、先頭の 4096 バイトに `vehicle_sil_simulation` のないファイルも |
| 400 | `unknown-participant` | `disable` か `manual` の名前が照合の結果にない |
| 400 | `unknown-network` | `api/connections`: ファイルにそのネットワークがない（ランチャーの使い方の誤り。`detail` に `{network, findings}`） |
| 400 | `target-not-listed` | ファイルにあるネットワークの `connect` の先（`none` 以外）が、そのファイルのそのネットワークの直近の一覧で使えるものでない、またはその後にファイルが変わった（`detail` に `{network, target}`。上の「ネットワークを外につなぐ」）。例: `error-target-not-listed.json` |
| 400 | `scenario-not-listed` | `scenario` が、`api/scenarios` がその設定ファイルに挙げるシナリオでない（`scenarios` がない、リンク、ほかの場所のファイル） |
| 400 | `unsupported` | ランチャーが持たない機能を使った（対象は表の下） |
| 400 | `unknown-variable` | `api/write`: その participant か変数が、実行の変数の一覧にない（`detail` に `{peer, name}`） |
| 400 | `not-writable` | `api/write`: 出力の変数（`detail` に `{peer, name}`） |
| 400 | `write-refused` | `api/write`: ランチャーの Control Server が拒否した（文言に理由） |
| 401 | `unauthorized` | トークンがないか違う（`WWW-Authenticate: Bearer` つき） |
| 403 | `forbidden-host` | `Host` が `127.0.0.1:<port>` でない |
| 403 | `forbidden-origin` | `Origin` がサーバー自身でない（`POST` と WebSocket は `Origin` が必要） |
| 403 | `cors-not-allowed` | `OPTIONS`（CORS は許可しない） |
| 403 | `save-disabled` | `POST api/config`: サーバーを `--no-save` で起動した |
| 403 | `commands-changed` | `POST api/config`: 編集で実行されるコマンドが変わる、比べられない、または新しいファイルで、サーバーを `--allow-command-changes` なしで起動した（`detail` はランチャーの照合の `{result, commands, findings}`。新しいファイルでは `null`）。例: `error-commands-changed.json` |
| 404 | `file-not-found` | `file` が root の下にない |
| 404 | `not-found` | そういう API や画面のファイルはない |
| 404 | `no-timeline` | `api/timeline/<session の id>`: その実行の時間の軸のファイルを、サーバーが持っていない（「シナリオの実行の時間の軸（`api/timeline`）」） |
| 405 | `method-not-allowed` | 画面のファイルに `GET` 以外 |
| 409 | `already-running` | `api/run`: 実行が動いている |
| 409 | `starting` | `api/run`: 別の `api/run` が実行を始めている途中（ランチャーをまだ登録していない） |
| 422 | `check-failed` | `api/run`: 実行を成す内容の照合が通らなかったので、実行を始めなかった（`detail` は `{result, findings}`。上の「照合と開始」）。例: `error-check-failed.json` |
| 409 | `not-running` | `api/time`、`api/write`: 動いている実行がない |
| 409 | `scenario-running` | `api/write`: 動いている実行がシナリオを実行している（シナリオの結果を変えないため、書かない）。例 `error-scenario-running.json` |
| 409 | `session-changed` | `api/stop-session` と対象を指定した `api/time` では、サーバーの識別子か実行が変わった。`api/write` では、指定した実行 ID が動いている実行と違う。操作は送られていない。例 `error-session-changed.json` |
| 409 | `config-changed` | `POST api/config`: ファイルの中身がもう `stamp` でない、または新しいファイル（`stamp` が `null`）がすでにある |
| 409 | `config-busy` | `POST api/config`: ファイルを置き換えられなかった（理由はメッセージに） |
| 409 | `variables-not-ready` | `api/write`: 変数の機能の状態が `ready` でない（文言に、要約の `variables.reason`） |
| 413 | `too-large` | 本文が 64 KiB を超える（`api/check` と `POST api/config` は 6 MiB + 64 KiB） |
| 413 | `text-too-large` | `api/check`、`POST api/config`: `text` が UTF-8 で 1 MiB を超える |
| 413 | `config-too-large` | `api/config`: ファイルが 1 MiB を超える |
| 415 | `unsupported-media-type` | 本文があるのに `Content-Type: application/json` でない |
| 422 | `config-unreadable` | `api/config`: ファイルが UTF-8 の文でない（NUL のバイト、UTF-8 でないバイト列）か、読めない（`api/check` の `text` と `api/describe` でも、読めないとき）。`POST api/config`: ランチャーが文を構成として読めない（`detail` は `{result, commands, findings}`）、または文の先頭の 4096 バイトに `vehicle_sil_simulation` がない。例: `error-config-unreadable.json` |
| 500 | `internal` | サーバーの不具合（例はない。起きたら標準エラーを見る） |
| 502 | `launcher-start` | ランチャーを起動できない |
| 502 | `launcher-output` | `check --json` が照合の結果を、または `connections --json` が一覧を書かなかった |
| 502 | `write-failed` | `api/write`: 書き込みが Control Server に届かなかった（切断された） |
| 503 | `launcher-unusable` | ランチャーが使えない（`api/app` の `launcher.problems` に理由） |
| 503 | `check-busy` | 照合（と `api/connections` の一覧、保存）の待ちが多すぎるか、待ちが長すぎる |
| 503 | `write-busy` | `api/write`: 前の書き込みが Control Server のレスポンスを待っていて、順番が来なかった（送っていない） |
| 503 | `shutting-down` | サーバーが終了しようとしている |
| 503 | `too-many-connections` | WebSocket の数の上限（ハンドシェイクのレスポンス） |
| 500 | `request-file` | ランチャーに要求を渡す一時ファイルを作れなかった（空きがない、一時ディレクトリに書けない。`TMPDIR` を設定する） |
| 504 | `check-timeout` | 照合（または `api/connections` の一覧）が時間の上限までに終わらなかった（止めた） |
| 504 | `write-timeout` | `api/write`: Control Server が時間内（`limits.write_timeout_s`）に応答しなかった。受け取られたかもしれない（送り直さない） |

- `api/check` と `api/run` は同じフィールドを受けます: `file`、`$schema`、`vehicle_sil_run_request`、`scenario`、`variant`、`params`、`disable`、`enable`、`manual`、`strict`、`connect`、`time_policy`、`speed`、`start_paused`。`text` があるとき、`api/check` が受けるのは `file`、`$schema`、`vehicle_sil_run_request`、`variant`、`strict`、`text` だけです。`api/describe` は `file` と `variant` を受けます。`POST api/config` は `file`、`text`、`stamp` を受けます（`stamp` は必須で、`null` でもよい）。`api/connections` は `file` と `network` を受けます。ほかのフィールド（打ち間違い、実行をする側のキー）と `null` は 400 `invalid-field` で、メッセージに未知のフィールドをすべてと、そのリクエストが受けるフィールドの一覧が入ります。フィールドが気づかれずに無視されることはありません。
- `invalid-field` の対象: `speed`、`start_paused`、`variant`、`params`、`enable`、`strict`、`connect`、`time_policy`、`network`、`force`、`disable`、`manual`（同じ名前を 2 度も）、`api/time` の `command`、`ms`、`value`、`api/write` の `writes`、`at_ms`、`late` と、変数の型か範囲に合わない値です。変数の値のときは、`detail` に `{peer, name}` が入ります。
- `unsupported` の対象: `control-server` を持たないのに `api/write` を送った。（サーバーは起動時に `time-commands`、`stdin-run-until`、`state`、`speed`、`start-paused`、`scenario`、`manual-participants` を必須として確かめるので、`api/time` にこの場合はありません。）

例のファイルは次のとおりです。ほかのコードは、GUI サーバーのテストの中で確認します。

| ファイル | 内容 |
|---|---|
| `error-already-running.json`、`error-check-failed.json`、`error-commands-changed.json`、`error-config-unreadable.json`、`error-not-writable.json`、`error-scenario-not-listed.json`、`error-scenario-running.json`、`error-session-changed.json`、`error-target-not-listed.json` | HTTP のエラー |
| `error-invalid-watch.json` | WebSocket の `error` |
| `stop.json`、`stop-force.json`、`stop-ended.json` | 停止のレスポンス |
| `time.json` | 時間のコマンドのレスポンス |

## WebSocket（`ws`）

### 認証のメッセージ

接続したら最初に `{"type": "auth", "token": "...", "session": <id>, "from_seq": <n>, "session_without_check": true}` を送ります。

| フィールド | 内容 |
|---|---|
| `token` | サーバーが表示した URL のトークン |
| `session`、`from_seq` | 再接続で、持っている分を伝えるときだけ送る。0 以上の整数でなければ、ないものとして扱う |
| `session_without_check` | 任意。`true` なら、この接続への `session` の通知の要約から `check` のフィールドを省く |

`session_without_check` を頼んでも、`hello` の要約には `check` が入り、照合の結果は `check` のイベントで届くので、手元の値を使い続けます。照合の結果は大きく、実行の間に変わらないためです。例は `ws-without-check.json` です。

### 閉じるコードと接続数の上限

| 起きたこと | サーバーがすること |
|---|---|
| トークンが違う | `error`（`code: unauthorized`）の後に、閉じるコード 4401（理由の文は `unauthorized`） |
| 5 秒以内に `auth` が届かない | `error` を送らずに、閉じるコード 4401（理由の文は `auth-timeout`） |
| サーバーが終了する | 閉じるコード 1001 |
| 認証した接続が 32 本あるときのハンドシェイク | 503 `too-many-connections` |
| ハンドシェイクの後、認証までの間に 32 本が埋まった | 閉じるコード 1013（理由の文は `too-many-connections`） |
| 認証前の接続が 16 本を超えた | 最も古い認証前の接続を、閉じるコード 1013（理由の文は `too-many-unauthenticated`）で閉じる |

- 認証前の接続の 16 本は、認証した接続の 32 本とは別に数えます。
- 1013 で閉じられた画面は、再接続します。
- ハンドシェイクの段階での拒否（`Host`、`Origin`、`too-many-connections`）は、ブラウザの JavaScript からは理由が見えません（1006 になります）。画面は、ハンドシェイクに失敗したら `GET api/app` を 1 回呼んで理由を確認します。

### 認証の後に届くメッセージ

認証の後、サーバーは次の順に送ります。

1. `hello`: `instance`、`session`（要約。なければ `null`）、`allowed`（実行があれば `session.allowed` と同じ。なければ、サーバーが受け付けること）、`replay`（`from_seq`: 再送の始まり。`null` は最初から）。画面は状態をこの要約で作り直す。
2. `dropped`（あれば）: `from_seq` 以後の log の行のうち、サーバーが覚えていないので送れない数（source ごと）。
3. `event`: 覚えている log の行のうち `from_seq` 以後のものを、`seq` の順に。
4. `synced`: `{session, last_seq}`。再送の終わり。
5. 以後は、起きるたびに `event`、`session`、`allowed`、`error`、`dropped`、`resync`、`variables`、`values`、`operation`、`availability`。

`session` の `id` が `hello` の要約と違う、または `from_seq` が今のセッションのものでないときは、最初からの再送になります（`replay.from_seq` が `null`）。

認証の後に画面が送れるのは、変数の `watch` と `unwatch` です（下の「ECU の変数」）。JSON のオブジェクトで `type` が文字列でないものは `error`（`code: invalid-message`）、知らない `type` は読み飛ばします。どちらでも接続は続きます。

### メッセージの種類

| `type` | 内容 | 例 |
|---|---|---|
| `hello` | 上の 1 | `ws-*.json` の最初 |
| `event` | `{session, event}`。ランチャーのイベントそのまま（サーバーが形を確認して通したもの） | `ws-pass.json`、`ws-log-dropped.json`、`ws-time.json`（時間のコマンド）、`ws-late.json`（遅れ） |
| `synced` | `{session, last_seq}` | `ws-pass.json` |
| `session` | `{session}`（要約） | `ws-pass.json`、`ws-without-check.json` |
| `allowed` | `{session, allowed}`。サーバーが受け付けることが変わり、それを運ぶ `session` が続かない（下の「サーバーが受け付けること（`allowed`）」）。`session` は今の実行の id、なければ `null` | `ws-allowed.json` |
| `error` | `{session, code, message, detail, problem_seq?}`。サーバーが気づいたこと（下の「`error` の `code`」）。要約の `problems` にも残る | `ws-bad-lines.json` |
| `dropped` | `{session, reason, log, at_least, events}`。この画面に送らなかったものの数 | `ws-dropped.json`、`ws-slow-client.json` |
| `resync` | `{reason, message}`。画面の受信が遅く、落とせない通知まであふれた。すぐ後に `hello` から送り直す | `ws-resync.json` |
| `variables` | `{session, variables}`。その実行の変数の機能の状態か一覧（要約の `variables` と同じ形） | `ws-variables.json`、`ws-variables-failed.json` |
| `values` | `{id, session, sim_ms, paused, peers}`。watch している変数の最後の値（下の「ECU の変数」） | `ws-variables.json` |
| `operation` | `{session, seq, state, done, peers}`。書き込みの結果が変わった（下の「ECU の変数」）。全部の接続へ送り、落とさない | `ws-variables.json` |
| `availability` | `{session, rev, peers}`。ECU が変数を使っているかが変わった（下の「ECU の変数」）。全部の接続へ送る | `ws-availability.json`（下の「利用状態」） |

`session` を送るのは、次のときです。`session_without_check` を頼んだ接続には、`check` のフィールドがありません。

- 実行を始めたとき、止めるよう頼んだとき。
- 実行が終わったとき（`launcher.running` が `false`、`state` が `ended`。ランチャーの最後のイベントの後）。
- ランチャーが何も書かなくなったとき（`launcher.silent_s` が変わったとき）と、また書き始めたとき。

`variables` を送るのは、実行が始まった直後（その `session` の通知のすぐ後）と、変わったときです。 `snapshot_rev` は session 要約だけの順序で、ランチャーのイベントや `allowed` の順序とは別です。変数要約の `rev` と利用状態の差分の `rev` は別の共通系列です。後の完全な変数要約には現在の利用状態が入り、差分は指定した participant だけを置き換えます。古い要約でも replay、ログ、コマンドの応答、確定済み書き込み結果は捨てません。旧サーバーにはこれらのフィールドがないため、HTTP と WebSocket の要約間の順序は最善を尽くす扱いです。旧クライアントは追加フィールドを無視できます。

`dropped` のフィールドは次のとおりです（`ws-slow-client.json` の数は例）。画面は、その source のログのその位置に「ここで N 行が落ちた」を表示します。

| フィールド | 内容 |
|---|---|
| `log` | source ごとの log の行の数。送らなかった `log_dropped` の数も、その source に加える |
| `at_least` | その数が下限でしかない source（もっと落ちているかもしれない） |
| `events` | ほかのイベントの数 |
| `reason` | `not-kept` はサーバーが覚えていない（再送の前）。`slow-client` は画面の受信が遅く、送る分から落とした |

source の名前は要約の `log` と同じです（65 個目からの source は `*`）。

### 受信が遅い接続

受信が遅い画面には、次の順に対処します。

1. log の行、`log_dropped`、知らないイベントを落とす（`dropped`）。
2. `time` と `state` は、それぞれ最後の 1 つにまとめ、`heartbeat` は数えずに落とす。
3. それでも追いつかないときだけ `resync`。

`values` は、送るときに、そのときの最後の値で作ります（待ち行列には watch ごとに 1 つ分の場所だけがあり、送れない間の値はためません）。`variables`、`operation`、`availability` は落としません。変数の利用状態を毎ステップ切り替える ECU でも、`availability` の通知は短い時間に 1 通（1 秒に 20 通ほど）までなので、それも読めない画面は `resync` になります。

### `error` の `code`

サーバーの通知です。

| `code` | 意味 |
|---|---|
| `unauthorized` | 認証のエラー |
| `invalid-message` | 画面が送ったものが、JSON のオブジェクトで `type` を持つものでない |
| `invalid-watch`、`too-many-watches` | `watch` の形か範囲が違う、接続の watch が上限を超えた |
| `launcher-output` | ランチャーがイベントでない行を書いた |
| `bad-event` | 知っているイベントのフィールドの型が違うか、サーバーがそのイベントを中継できなかった |
| `line-too-long` | 1 行が上限（16 MiB）を超えた。捨てる |
| `seq-gap`、`seq-order` | `seq` が飛んだ、戻った |
| `no-hello` | 最初のイベントが `hello` でない |
| `events-format` | `hello` の `vehicle_sil_events` が、このサーバーの読むバージョンと違う |
| `launcher-stderr` | ランチャーが標準エラーに書いた行 |
| `kept-limit` | 覚える量の上限を超えたので、要約に一部しか入れなかった |
| `no-result` | ランチャーが `result` を書かずに終わった（`detail.exit_code`） |
| `output-left-open` | ランチャーは終わったが、ランチャーが起動したプロセスが出力を開いたままなので、残りを読まずに終えた |
| `stop-not-delivered` | 停止のコマンドをランチャーの標準入力に書けなかった |
| `stop-escalated` | サーバーが終了するとき、ランチャーが時間内に止まらず、シグナルで止めるよう頼んだ |
| `launcher-silent` | ランチャーのプロセスが動いているのに、結果の前に `--silent-after`（既定 5 秒）より長く 1 行も書かない |

- `unauthorized`: `session` は `null` です。この後 4401 で閉じます。
- `invalid-message`: `session` は `null` です。その接続にだけ送り、接続は続きます。
- `invalid-watch`、`too-many-watches`: `session` は `null` で、`detail.id` に watch の id が入ります。その接続にだけ送り、接続は続きます。
- `launcher-output`: JSON のオブジェクトでない、`event` がない、`seq` が 0 から 2^53 までの整数でない行です。`1e999` のような有限でない数を含む行も、ここに入ります。
- `bad-event`: `t_ms` が 0 から 2^53 までの整数でない、`log_dropped` の `count` と `sources` の値が数でない、`state` の `rev` が 0 から 2^53 までの整数でない、または `allowed` がオブジェクトでない、`command` の `id` が `null` でも 0 から 2^53 までの整数でもない、を含みます。そのイベントは捨てます（中継も要約への反映もしません）。
- `launcher-stderr`: 最初の 20 行はすぐに送ります。それより後は最後の 30 行だけを、実行が終わったときに送ります。間で落とした行があれば、その前に 1 つ送ります（`detail.skipped`: 落とした行の数）。
- `kept-limit`: `detail.what` は `check`、`result`、`findings`、`participants`、`state`、または 4 KiB を超えたイベントの種類（`phase`、`time`、`registry`、`command`、`participant`）です。4 KiB を超えたイベントは、必ず入れるフィールド（下の「上限」）を残し、ほかのフィールドは入る分だけ入れて、入らないフィールドの名前を `omitted_fields` に書きます。通知は種類ごとに 1 回です。
- `launcher-silent`: ランチャーはハートビート `heartbeat` を 1 秒ごとに書くので、止まっているかもしれません。書かなくなるたびに 1 回送ります（`detail.seconds`）。続く間は要約の `launcher.silent_s` が増え、また書いたら `null` に戻ります。

## 要約（`session`）

| フィールド | 内容 |
|---|---|
| `id` | 実行の番号（サーバーの起動ごとに 1 から） |
| `snapshot_rev` | 同じサーバーインスタンスと実行の中で増える、session 要約の取得順 |
| `findings[].event_seq` | 保持された実行時 finding のランチャーイベント番号。同じインスタンスと実行の中で一意で、要約とライブ通知の照合に使う。旧サーバーにはなく、その場合の照合は最善を尽くす扱い |
| `file`、`disable`、`manual`、`speed`、`start_paused` | 始めたときの指定 |
| `started_at`、`ended_at` | 時刻（UTC、ISO 8601）。`ended_at` は終わるまで `null` |
| `state` | `running` か `ended` |
| `launcher` | `{pid, running, exit_code, signal, silent_s}` |
| `stop` | サーバーがランチャーに頼んだ停止: `none`、`requested`（順に止める）、`forced`（待たずに止める） |
| `phase` | 最後の `phase` イベントの段階（まだなければ `null`） |
| `last_seq`、`last_t_ms` | 最後に受信したイベントの `seq` と `t_ms` |
| `time` | 最後の `time` イベント（そのまま。なければ `null`） |
| `check` | `check` イベントの `check`（照合の結果を丸ごと。まだなければ `null`） |
| `stop_at_ms` | 照合結果が保存上限を超えたときや、イベントの行が大きすぎて読み飛ばされたときの、照合済みの停止時刻。行を読み飛ばした場合は、起動前に同じリクエストを照合した値を使う。`null` は上限なし。照合結果を保存できるときや値が不明なときは省略 |
| `registry` | 最後の `registry` イベントから `event`、`seq`、`t_ms` を除いたもの（なければ `null`） |
| `participants` | participant ごとの今の状態（初めて現れた順） |
| `participants_omitted` | 上限（256 人）を超えたので覚えていない participant のイベントの数 |
| `findings`、`findings_omitted` | `finding` イベント（`event`、`seq`、`t_ms` を除く）と、上限を超えて覚えていない数 |
| `last_command` | 最後の `command` イベントから `event`、`t_ms` を除いたもの（なければ `null`） |
| `launcher_state` | ランチャーの最後の `state` イベントから `event`、`t_ms` を除いたもの（ランチャーがいま受け付けること。まだなければ `null`）。覚えるのは最後の 1 つだけ |
| `allowed` | このサーバーがいま受け付けること: `{start, stop, force, save}`（下の「サーバーが受け付けること（`allowed`）」） |
| `result` | `result` イベントの `result`（なければ `null`） |
| `variables` | ECU の変数の機能の状態、participant ごとの変数、最近の操作（下の「ECU の変数」） |
| `log` | source ごとの `{kept, dropped}`: サーバーが覚えている行の数と、落とした行の数 |
| `problems`、`problems_omitted` | サーバーの通知（`error` と同じ `{code, message, detail, problem_seq}`）を起きた順に並べたものと、上限を超えて覚えていない数 |
| `problem_seq` | この実行で発生したサーバー診断の最大番号。省略された診断も含む。実行に属する `error` と要約の同じ診断は同じ番号を持ち、重複を除ける。旧サーバーにはない |
| `scenario` | シナリオの実行（`api/run` の `scenario`）の経過。シナリオのない実行では `null`（下の「シナリオ」） |

フィールドごとの補足です。

- **始めたときの指定**: `manual` は `api/run` で指定したものだけです（設定ファイルの `manual: true` は `check` にあります）。`speed` の `null` は設定ファイルのままで、いまの速度は `launcher_state.speed` です。
- **`state`**: `ended` は、ランチャーが終わり、その出力を読み終えた（または `output-left-open` で打ち切った）状態です。その間（長くて 2 秒）は、`api/session` で `launcher.running: false` と `running` が見えることがあります。
- **`launcher`**: `running` はランチャーのプロセスが動いているかで、止められるかどうかはこれで決めます。終わったら `exit_code` が入ります（シグナルで終わったら `null` で、`signal` にその番号。POSIX だけ）。`silent_s` は、動いているランチャーが何も書かない秒数です（`--silent-after` を超えたときだけ、その倍数ごとに更新。書けば `null`）。
- **`time`**: 仮想時間 `sim_ms`、実際の速度 `actual_speed`、遅れの数 `late_steps`、`max_late_ms`、`slipped_ms`、`late_ms` です。途中から開いた画面も、これで今の時刻がわかります。一時停止しているか、その時刻、指定の速度は、`launcher_state` にあります（`paused`、`paused_at_ms`、`speed`）。一時停止している間、`time` は止まる前のものでありえます（ランチャーが次に見るまで `sim_ms` が前の時刻、`actual_speed` と `late_ms` が止まる前の値）。
- **`check`**: participant の `required`、`enabled`、`variables` を含みます。`session_without_check` を頼んだ接続への `session` の通知には、このフィールドがありません（`hello` と HTTP のレスポンスにはあります）。
- **`last_command`**: `seq`、`command`、`source`、`accepted`、`message`、`reason`、`id`、受け付けた `step` の `ms` と `at_ms`、受け付けた `run_until` の `at_ms` です。`seq` で、新しいレスポンスか、前と同じレスポンスかがわかります。`state` より前のランチャーは `reason` と `id` を書きません。
- **`launcher_state`**: ランチャーが書いた `state` イベントそのまま（[docs/ja/simulation-events.md](../../docs/ja/simulation-events.md) の `state`）に、その `seq` を付けたものです: `rev`、`phase`、`paused`、`paused_at_ms`（一時停止している時刻）、`speed`、`stop`、`allowed`（ランチャーが受け付けること: `pause`、`resume`、`step`、`speed`、`run_until`、`stop`、`write`。それぞれ `{ok, reason, message, caution}`）など。サーバーは変えません。`rev` は変わるたびに上がるので、画面は新しいものと、同じものがもう一度届いたのとを見分けられます。これは、ランチャーが書いた時点の答えです。`ok` は、ランチャーがそのコマンドを受け付けることを言い、何かが変わることは言いません。`state` の後のイベント（`command`、`time`）のほうが新しいことがあります。`state` のイベントは `event` の通知としても届きますが、要約が覚えるのは最後の 1 つだけなので、後から開いた画面は要約から読み、前のものは再送されません。機能 `state` のないランチャーは何も書かず、このフィールドは `null` です。
- **`omitted_fields`**: `time`、`registry`、`last_command`、`participants` の各要素は、大きすぎて入れなかったフィールドがあれば、その名前の配列 `omitted_fields` を持ちます。participant では、後のイベントでそのフィールドが届けば消えます（例は `ws-large-fields.json`）。
- **`variables`**: `hello` の要約と HTTP のレスポンスにあります。**`session` の通知の要約には、このフィールドがありません**。大きく、めったに変わらないので、変わったときに `variables` の通知で送ります。同じ実行の `session` の通知では、手元の値を使い続けます。別の実行の `session` の通知の直後には、その実行の `variables` の通知が届きます。
- **`log`**: 落とした行は、サーバーが覚えずに落とした行と、ランチャーが読む側の遅さで書けなかった行です（後者は `log_dropped` の `sources`）。`log_dropped` のイベントは、名前の挙がった source の log の、来た位置に覚えます。再接続した画面にも同じ位置に届き、1 つのイベントは 1 回だけ送ります。画面はそこに「ここで N 行が落ちた」を表示します。

`participants` の各要素は、`participant` イベントのフィールド（`event`、`seq`、`t_ms` を除く）を名前ごとに重ねたものです（`name`、`state`、`pid`、`exit_code`、`signal` など）。

- `failed` の `reason` は、後で `ended` が来ても残ります。
- ランチャーの `control_link` イベント（変数を読み書きする API の接続の状態）は、その participant の `control_link` のフィールドに、`name` を除いたフィールドのオブジェクト（`{"state", "variables"}` など。`state` の値はランチャーのまま）として重ねます（例は `ws-control-trouble.json`）。
- `required` と `enabled` は `check` を見ます。
- `stop_requested` のイベントが来た participant には、サーバーが `stop_requested: true` を付け、後の状態でも残します。止めるよう求めた後の終了を、失敗と区別するためです。

ランチャーが `result` を出さずに終わったときは、`launcher.running: false` と `exit_code`、`state: ended`、通知の `no-result` になります。

## サーバーが受け付けること（`allowed`）

`allowed` は `{rev, start, stop, force, save}` です。後の 4 つはそれぞれ `{ok, reason, message}` で（`save` には `caution` もあります）、`ok` はサーバーがいまそのリクエストを受け付けるか、`reason` は理由のコード（`ok` のときは `null`）、`message` は人に見せる英語の 1 文（`ok` のときは `null`）です。`session`（要約）と `allowed` のメッセージに入り、実行がないときにも読めるように、`hello` と `api/session` の答えの最上位にも入ります。画面は、`ok` でボタンを使えるようにし、`message` を出せば、自分の表を持たずに済みます。それでも送られた `api/run` は、`start` の理由と同じコードのエラーを返します（409 `already-running`、`starting`、503 `shutting-down`、`launcher-unusable`）。`api/stop` はエラーを返さず、いつも要約つきの 200 を返します（`stop` や `force` に理由があるときは何もしません）。そのため `stop` と `force` の理由は `allowed` にだけあります。それでも送られた `POST api/config` は、403 `save-disabled` か 503 `shutting-down` を返します。

| フィールド | `reason` | いつ |
|---|---|---|
| `start` | `shutting-down` | サーバーが終わろうとしている |
| | `already-running` | 実行が動いている（出力を最後まで読み終えていない間も）。`api/run` がまだ答えていないが、登録したばかりの実行も含む |
| | `starting` | 実行を始めている途中（`api/run` がまだ答えていない）で、動いている実行がない |
| | `launcher-unusable` | 見つけたランチャーが使えない（`api/app` の `launcher.problems`）。最後に調べた結果のとおり |
| `stop` | `shutting-down` | サーバーが終わろうとしていて、実行はサーバー自身が止める |
| | `not-running` | ランチャーが動いている実行がない |
| | `already-stopping` | すでに停止を頼んだ（`stop` が `requested` か `forced`） |
| `force` | `shutting-down`、`not-running` | `stop` と同じ |
| | `already-forced` | すでに強制の停止にした |
| `save` | `shutting-down` | サーバーが終わろうとしている |
| | `save-disabled` | サーバーを `--no-save` で起動した |

- `save` の `caution` は `null` か、保存はするが先に知っておくことがあるときの `{code, message}` です（ランチャーの `allowed` の `caution` と同じ形）。コードは `command-changes-allowed` だけです。サーバーを `--allow-command-changes` で起動したので、実行されるコマンドを変える編集と新しいファイルも保存します。画面は、保存のボタンの近くにメッセージを出します。1 つの編集を保存するかは、リクエスト（`POST api/config`）ごとに決め、`allowed` では決めません。

- 当てはまる理由のうち、表の順で最初の 1 つだけを返します。実行を始めている途中は、`stop` と `force` は `ok` です。リクエストは待ち、ランチャーが始まったら効きます（「停止（`api/stop`）」）。
- ランチャーが受け付けること（pause、step、speed、書き込み）はここにはありません。ランチャーが書いたままの `launcher_state` の `allowed` です。そこでの `ok` は、ランチャーがそのコマンドを受け付けることを言い、何かが変わることは言いません（一時停止中の `pause` も `ok`）。
- **`rev`** はサーバー全体で 1 つの番号です。`allowed` の言うことが変わるたびに 1 上がり、`allowed` を運ぶどの場所（`hello`、`api/session`、要約、`allowed` のメッセージ）でも同じです。複数の場所から `allowed` を受ける画面は、`rev` の大きいほうを採ります。新しい実行が始まっても 0 に戻りません。サーバーを起動し直すと 1 から始まるので、`rev` を比べるのは同じ `instance`（`hello` の `instance`）の中だけです。`hello` を受けたら、その `allowed` に置き換えます。
- **いつ送るか**: `allowed` は、`session` を送るたびに、そのときの値で付きます。変わったのに、それを運ぶ `session` が続かないときは、代わりに `allowed` のメッセージを送ります: 開始が始まった（`starting`）、開始に失敗して `start` が `ok` に戻った、サーバーが終わり始めた（`shutting-down`）。`session` がその変化を運ぶとき（実行を登録した、停止を頼んだ、実行が終わった）は、`allowed` のメッセージは送りません。`allowed` のメッセージは小さく、遅い接続でも落としません。このメッセージの種類を知らない読み手は無視し（「版と互換」）、`hello`、`api/session`、各要約の `allowed` は引き続き受けます。
- 実行がなければ要約がないので、`hello`、`api/session`、`allowed` のメッセージの `allowed` がすべてです。
- コードは増えることがあります。コードの意味は変わりません。

## ECU の変数

ランチャーが Control Server を持つとき（`api/app` の `variables.available`）、GUI サーバーは実行ごとにその Control Server に接続し、ECU が宣言した変数を読み書きします。持たないランチャー、gRPC のない GUI サーバー、`--no-variables` では、`variables.state` が `off` で、理由が `reason` にあります。例は `ws-variables.json`（一覧、watch、始まる前の書き込み、ステップ実行、終わり）と `ws-variables-failed.json`（接続できない）です。

### 状態と一覧（要約の `variables`、`variables` の通知）

`{"rev", "state", "reason", "peers", "peers_omitted", "variables_omitted", "operations", "operations_from_seq", "operations_open_omitted"}`

| フィールド | 内容 |
|---|---|
| `rev` | 同じサーバーインスタンスと実行の中で、変数の完全な要約と利用状態の差分に共通する取得順 |
| `state` | 変数の機能の状態（下の表）。値は増えることがある |
| `reason` | `off`、`connecting`、`failed` の理由（人向けの英語の文。なければ `null`） |
| `peers` | `[{"name", "connected", "variables": [...]}]`。Control Server が知っている participant（変数のないものも） |
| `peers[].variables[]` | `{"name", "type", "role", "writable", "unit", "min", "max", "step", "description", "enum", "availability"}` |
| `peers_omitted`、`variables_omitted` | 上限を超えたので入れていない participant と変数の数 |
| `operations` | 操作（下の `operation` から `type` と `session` を除いたもの。サーバーが初めて見た順） |
| `operations_from_seq` | サーバーが受け取る操作のうち、この番号（`seq`）以上のものは、全部 `operations` に入っている（0 なら、サーバーが見た操作の全部）。シナリオ自身の書き込みの操作は、サーバーに届かない（下の `operation`） |
| `operations_open_omitted` | サーバーが覚えきれなかった未確定の操作の数（ふつうは 0） |

| `state` | 意味 |
|---|---|
| `off` | この実行に変数はない |
| `connecting` | Control Server に接続しようとしている（切断されて再接続している間も） |
| `ready` | 読み書きできる |
| `failed` | 接続できなかった、拒否された |
| `ended` | 実行が終わった。最後に読めた値が残っている |

- `reason` は、`ended` では、実行の終わりより前に接続が切断されていたとき（値が終わりのものより古いかもしれない）だけ入ります。
- `peers[].connected` は、その ECU がいま接続しているかです。
- `type` は `bool`、`integer`、`real`、`enum` です。
- `role` は `input`（実行中も書ける）、`parameter`（最初のステップの前だけ書ける）、`output`（読むだけ）です。`writable` は `role` が `output` でないことです。
- `min`、`max`、`step`（編集の刻み幅の目安）は数か `null` です。
- `enum` は、`type` が `enum` のとき `[{"name", "value"}]`、ほかは `null` です。
- `availability` は、ECU がいまその変数を使っているかです。`unknown`、`active`、`inactive` のどれかです（下の「利用状態」）。
- `operations` に入るのは、サーバーが覚えている未確定の操作の全部と、最後の 32 件です。途中から開いた画面と、再接続した画面のためです。
- participant が接続するたびに一覧が変わるので、`variables` の通知は、短い間の変化をまとめて送ります。

結果を待っている操作があるクライアントは、再接続した後の `hello`（か `api/session`）の要約で、次のように見分けます。

| `operations` にあるか | 番号 | 状態 |
|---|---|---|
| ある | | その状態。未確定なら、後で `operation` の通知が届く |
| ない | `operations_from_seq` 以上 | まだ届いていないだけ |
| ない | `operations_from_seq` より小さい | 切断されている間に確定していて、結果（`operation` の通知）はもう届かない |

最後の行は、`operations_open_omitted` が 0 のときです。1 以上のときは、確定したかどうかもわかりません。ライブラリは、この最後の場合を `no-answer` にします。

### 利用状態（変数の `availability`、`availability` の通知）

ECU は、動いている間に、いま変数のそれぞれを使っているかを言えます。たとえば、最初のステップで設定を読んだ後です（[docs/control-api.md](../../docs/ja/control-api.md#変数の利用状態) の「変数の利用状態」）。画面は、使っていない変数を、薄くするなどして別に見せられます。利用状態は知らせるだけのものです。変数の書き込み、読み取り、watch は、利用状態によらず同じです。

| `availability` | 意味 |
|---|---|
| `unknown` | ECU がまだ決めていない（ふつうは最初の数ステップ）。いつもどおりに表示する |
| `active` | 使っている。利用状態を報告しない変数は、どれも `active` |
| `inactive` | ECU のいまの構成では使っていない |

- 要約の変数の `availability` は、いまの状態です。再接続した後の `hello` の要約には、そのときの状態が入っています。
- `availability` の通知は `{"type": "availability", "session", "rev", "peers": {"<participant>": {"<変数>": "<状態>", ...}}}` です。状態が変わった participant ごとに、利用状態を報告する変数の全部の状態が入り、その participant について前に届いたものを置き換えます。短い間の変化はまとめて送り、変数を watch しているかによらず、全部の接続に送ります。
- その変数が載った `variables` の通知（か `hello`）の後に届きます。上限で要約から外した participant と変数は入りません。
- participant がシミュレーションから抜けても、Control Server との接続が切れても、状態は変わりません。ECU が最後に言ったままです（接続のことは `peers[].connected` と `state` が言います）。participant の新しいプロセスは、変数が宣言した状態から始まります。

### 値（`watch`、`unwatch`、`values`）

画面は、認証の後に次を送ります。

- `{"type": "watch", "id": <integer from 0 to 2^53>, "variables": "all" or [{"peer", "name"}, ...], "period_ms": <integer>}`: 変数の値の受信を始めます。
- `{"type": "unwatch", "id": <id>}`: やめます（ない `id` は何もしません）。

| `watch` のフィールド | 内容 |
|---|---|
| `id` | その接続の中で watch を指す番号。同じ `id` をもう一度送ると置き換える |
| `variables` | `"all"` か、名前で挙げた変数。名前で挙げられるのは `limits.watch_variables`（2000）個まで |
| `period_ms` | `limits.watch_period_ms_min`（20）から `limits.watch_period_ms_max` まで（省くと下限） |

- 接続ごとの watch は `limits.watches`（16）個までです。超えたものと形の違うものは、`error`（`invalid-watch`、`too-many-watches`）で、接続は続きます。
- **1 通は 256 KiB までで、これを超える 1 通を送ると、サーバーは `error` を返さずに接続を閉じます（閉じるコード 1009）。** 長い名前を多く挙げるときは、送る前に大きさを確認し、超えるなら `"all"` を頼むか、watch を分けます（ライブラリは `"all"` に替えます）。同じものを送り直すと、また閉じられます。
- 接続が切断されると、その接続の watch は全部なくなります（再接続したら送り直します）。
- watch は実行をまたいで続きます。実行がないとき、変数の状態が `ready` と `ended` のどちらでもないとき、いまの実行にない変数（その分だけ）には、何も届きません。いまの実行にない名前を挙げてもエラーではありません（実行の前に watch できます）。

サーバーは `{"type": "values", "id", "session", "sim_ms", "paused", "peers": {"<peer>": {"sample_ms", "values": {"<name>": <value>}}}}` を送ります。

**いつ届くか**

- watch の直後に 1 回、その後は `period_ms` ごとに、**前に送ったものから変わったときだけ**届きます（値を読んだステップが進んだ、`sim_ms` か `paused` が変わった）。止まっている間は、値が変わらなければ届きません。
- 実行が始まったとき、変数の状態か一覧が変わったとき（`variables` の通知）、実行が終わったときは、`period_ms` を待たずに送ります。
- ある実行の `values` は、その実行の `variables` の通知（か `hello`）より後に届きます。
- 実行が終わると、状態は `ended` になり、最後に読めた値が 1 回届きます（`paused` は `false`）。終わった後に始めた watch にも 1 回届きます。

**値の形**

- `sample_ms` は、その participant の値を読んだステップの仮想時間です。
- 値は、bool は真偽、整数と列挙は数、実数は数で、有限でない実数（NaN、無限大）は `null` です。2^53 を超える整数は、JSON の数として正確でないことがあります。
- まだ 1 回も値を読んでいない participant は `peers` にありません。
- 値は「最後の値」です。全部の値の列ではありません（速いシミュレーションでは、間のステップの値は届きません）。サーバーは、watch されている participant の値だけを、頼まれた頻度でランチャーから読みます。履歴はサーバーが持たないので、必要な画面が自分で持ちます。

**`paused` と `sim_ms`**

`paused` と `sim_ms` と値は、**1 回の読み取りのもの**です。サーバーは、watch している participant の値と仮想時間を、Control Server の 1 回の呼び出し（`GetSnapshot`。[docs/control-api.md](../../docs/ja/control-api.md) の「時刻と値をまとめて読む」）で読み、`paused` はそのレスポンスのフラグ（`consistent`）から決めます。

| `paused` | シミュレーション | 値 | `sim_ms` |
|---|---|---|---|
| `true` | `sim_ms` で止まっている | その時刻の値。画面は「止まっている時刻の値」として見せられる | 止まっている時刻 |
| `false` | 動いている、終わっている、読む間に進んだ、のどれか | 最後の値 | 値と一緒に読んだ仮想時間 |

- `true` のとき、`sim_ms` より前に始まったステップの値は全部入っていて、`sim_ms` より後のステップの値は 1 つも入っていません。
- `false` のとき、値は `sim_ms` より古くありません（値のほうが新しいことはあります）。まだ読んでいなければ、`sim_ms` はサーバーが最後に知った仮想時間です（わからなければ `null`）。
- 最初のステップの前で止まっている間（`start_paused`）は、`sim_ms` 0、`paused` `true` で、`peers` は空です。

ちょうど `sim_ms` に始まるステップの値は、入っているかどうか決まりません（止まっている間に、後から届くことがあります）。届いたら、同じ `sim_ms`、`paused` `true` で、値の違う通知がもう 1 回届きます。止まっている間、サーバーが読み直すのは、watch の `period_ms` と約 0.25 秒の長いほうの間隔です。間隔の長い watch では、その分だけ後になります。入っているかは、participant ごとの `sample_ms` でわかります。

`paused` が `true` になる条件と、`false` に変わる時点は次のとおりです。

- **前提（Control Server のもの）**: ECU が値をステップの中から送っているかぎりです。`ecu_main` で作った ECU はそうします。この前提が崩れると、ランチャーが警告 `control-link-late-values` を出し、`true` でも値が欠けていることがあります。
- 「ステップの値」は、その時刻より前に ECU が読んだ最後の値です。
- 実時間で動く participant（シミュレーションの時刻で動かないもの）の値には保証がなく、`paused` はその値を見ずに決まります。
- 切断された participant の値は、最後の値のままです（`variables` の `connected` で見ます）。
- **別々の呼び出しで読んだとき**: 1 つの watch の participant を別々の呼び出しで読んだとき（間隔の違う watch が同じ participant を見ているとき）は、どの participant も同じ時刻で止まっていると読めていて、その後のどの読み取りもその時刻で止まっていたときだけ `true` です。
- **`api/time` で再開かステップ実行を送ったとき**: サーバーは、それまでに読んだ値を「止まっている時刻の値」と扱うのをやめ、watch の次の間隔で読み直した結果で決め直します。通知が届くのは、その間隔で、前に送ったものと変わったときです。ランチャーがまだコマンドを受け取っていなければ、同じ時刻で止まったままと読めるので、`false` の通知は届きません。
- **ほかのクライアントが Control Server に直接再開やステップ実行を頼んだとき**: サーバーが次に読むか、時刻の通知（Control Server から約 0.1 秒ごと）で、止まっていた時刻より進んだ・終わったとわかるまで、止まっていた時刻の値が最後の通知のままです。わかった後は、値を読めなくても、watch の次の間隔で `false` の通知が届きます。

読み取りがうまくいかないときは、次のようになります。

- **レスポンスの上限に入らない**: watch している participant の値が、Control Server の 1 つのレスポンスの上限（4 MiB）に入らないときは、入らなかった呼び出しの participant を半分ずつに分けて読みます（サーバーのログに警告を出します）。分けて読んでも、どの呼び出しも同じ時刻で止まっていると応答すれば `true` です。
- **1 つの participant で上限を超える**: 1 つの participant の値だけで上限を超えるときは、その値を読みません（ログに警告。その participant が別のプロセスに替わるか、変数が変わるまで）。その participant を含む watch の `paused` は `false` のままです。
- **読めないことが続く**: 値を読めないこと（Control Server が期限までに応答しない、など）が 3 回続くと、変数の状態は `ready` のまま、`reason` に理由が入った `variables` の通知が届きます（値は最後に読めたもの）。読めるようになると、`reason` は `null` に戻ります。

### 書き込み（`api/write`、`operation`）

`POST api/write` `{"session": <run id>, "writes": {"<peer>": {"<name>": <value>}}, "at_ms": <integer>, "late": "reject" | "apply"}` → `{"session_id", "operation"}`

| フィールド | 内容 |
|---|---|
| `session`（必須） | 書き込みを向ける実行の id（要約の `id`） |
| `writes` | 変数の型に合わせた値。1 回のリクエストは `limits.writes_per_request`（256）個まで |
| `at_ms` | 反映する仮想時間（0 から `limits.write_at_ms_max`）。省くと次のステップ |
| `late` | 届いたときにその時刻のステップがもう実行されていたときの扱い: `reject`（既定）か `apply` |

- **`session`**: 動いている実行の id と違えば、409 `session-changed` で拒否し、何も書きません。ある実行を見ながら送った書き込みが、その実行が終わって次が始まった後に届いても、次の実行に書かれないようにするためです。拒否されたら、いまの実行の変数を見てから書き直します。
- **`writes` の値**: `bool` は `true` / `false`、`integer` は整数（整数値の実数も可）、`real` は有限の数、`enum` は項目の値（数）か項目の名前（文字列）です。`min` と `max` があれば、その範囲（両端を含む）にします。
- サーバーは、変数の一覧と照合して、合わないものを 400 で拒否します（`invalid-field`、`unknown-variable`、`not-writable`）。1 つでも合わなければ、全部を送りません。
- **`at_ms`**: その時刻以後に始まる最初のステップで反映されます。
- **`late`**: `reject` は、反映せず、結果が `late` になります。`apply` は、次のステップで反映し、結果の `late` が `true` になります。
- 1 回のリクエストの書き込みは、participant ごとに、同じステップでまとめて反映されます。リクエストは届いた順にランチャーへ渡します（1 つずつ）。
- レスポンスは、ランチャーが操作を受け取ったところで返ります（番号だけ）。反映されたかは、`operation` の通知で届きます。レスポンスより先に `operation` が届くことがあります。

止まっている間の書き込みは、次のようになります。

- 最初のステップの前で止まっている間（`start_paused`）に書いたものは、時刻 0 のステップで反映されます（`parameter` も書けます）。
- 一時停止してから書くときは、`at_ms` を止まっている時刻より後にします。その時刻のステップは、もう実行した ECU があるためです（ランチャーのルール: [docs/control-api.md](../../docs/ja/control-api.md)）。
- 止まっている時刻そのもの（か、それより前）を指定した書き込みは、いったん `queued` になり、ECU の次のステップで `late` に確定します（`late: "apply"` なら、そのステップで反映されます）。

`{"type": "operation", "session", "seq", "state", "done", "peers": {"<peer>": {"state", "applied_at_ms", "late", "reason", "applied_writes", "failed_write"}}}`

participant ごとの `state` は次のとおりです。

| `state` | 確定か | 意味 |
|---|---|---|
| `sent` | 途中 | ランチャーが ECU へ送った |
| `queued` | 途中 | ECU が受け取った |
| `disconnected` | 途中 | |
| `applied` | 確定 | `applied_at_ms` のステップで反映した |
| `late` | 確定 | |
| `rejected` | 確定 | ECU の setter が失敗した |
| `cancelled` | 確定 | 反映の前にシミュレーションが終わった |
| `invalid` | 確定 | ECU が拒否した: 最初のステップの後の `parameter` など（`reason` に理由） |
| `busy` | 確定 | |
| `unknown` | 確定 | 反映されたかわからない |

- `applied_writes` と `failed_write` は、Control Server の `TargetResult` の値をそのまま渡したものです。participant の書き込みのうち先頭から反映された数と、`reason` が言っている書き込みの位置（0 から）です。participant の書き込みは、`api/write` のリクエストのうちその participant への書き込みを、書いた順に並べたものです。わからないとき、言われないときは `null` です（[docs/ja/control-api.md](../../docs/ja/control-api.md#書き込みが反映される時点) の「書き込みが反映される時点」）。
- 全体の `state`: 途中は `sent`（まだ受け取っていない ECU がある）か `queued` です。確定（`done` が `true`）したら、全部の participant が同じならその状態、違えば `mixed` です。確定した操作は変わりません。
- 状態が変わるたびに、全部の接続へ送ります（ほかのクライアントが Control Server に直接書いた操作も届きます）。シナリオ自身の書き込みの操作は届きません。GUI サーバーが Control Server から受け取らないからです。
- 受信の遅い接続でも、`operation` は落としません。ただし、その接続の待ち行列があふれて `resync`（`hello` からの送り直し）になったときは、要約の `operations`（未確定の操作と、最後の 32 件）にあるものだけが届き直します。それより前に確定した操作の結果は、その接続には届きません（どれがそうかは、要約の `operations_from_seq` でわかります）。
- 実行が終わるまでに確定しなかった操作は、サーバーが `unknown`（`reason`: `the run ended before the result came`）で確定させて送ります。
- 未確定の操作が 256 件を超えて、サーバーが古いものを覚えていられなかったとき（要約の `operations_open_omitted`）も、その操作の変化が届けば `operation` で送ります。確定しないまま実行が終われば、終わりに `unknown` を送ります（そのときは `peers` が空です）。
- 実行が終わるのと同時にランチャーが受け取った書き込みは、レスポンスで番号を返し、`unknown` で確定させて送ります。
- GUI サーバーと Control Server の接続が切断されている間に確定した操作と、Control Server が GUI サーバーへ送りきれなかった変化は、再接続したとき（送りきれなかったとわかったとき）に読み直して送ります。

## シナリオ

ランチャーの `run --scenario` と `check --scenario`（[docs/scenario.md](../../docs/ja/scenario.md)）を、画面から使う API です。ランチャーに feature `request` が要ります（このサーバーのどのリクエストも同じです）。例は `scenarios.json`、`check-scenario.json`、`ws-scenario.json`、`error-scenario-not-listed.json`、`error-scenario-running.json` です。

### シナリオの一覧（`api/scenarios`）

`GET api/scenarios?file=<configuration file>` は、設定ファイルの隣のディレクトリ `scenarios` の `.yaml` と `.yml` を返します（名前の順。200 個まで）。`file` は `api/check` と同じ検査を通ります。

| フィールド | 内容 |
|---|---|
| `directory` | そのディレクトリ（root からの相対） |
| `found` | ディレクトリがあるか |
| `scenarios` | `[{path, name, size}]`（`path` は root からの相対） |

- 一覧に入れないもの: `.` で始まる名前、サブディレクトリ、通常のファイルでないもの（シンボリックリンクを含む）、`:` や `\` を含む名前、255 文字を超える名前。
- `scenarios` か、その上のディレクトリ（root の下）がシンボリックリンクなら、`found` は `false` です。

### 照合と実行の `scenario`

`api/check` と `api/run` の `scenario` は、省くか `null` なら、シナリオなしです。

- 値は、その設定ファイルの `api/scenarios` の `path` のどれかだけです（サーバーが一覧を作り直して確認します）。ほかは 400 `scenario-not-listed` です。
- サーバーは、それを要求ファイルの `scenario` に、構成ファイルのディレクトリからの相対で書きます（要求ファイルの相対の `scenario` は、ランチャーがそこから読みます）。`simulation.yaml` でも `sub/simulation.yaml` でも `scenarios/throttle.yaml` です。
- `api/check` のレスポンスの `scenario` は、渡したシナリオです。

シナリオを実行している間にできることは、次のとおりです。

| 操作 | 扱い |
|---|---|
| 止めた状態で始める（`start_paused: true`） | ランチャーが断る: 422 `check-failed`（`api/run`）、`usage` の結果（`api/check`） |
| 変数を読む | ランチャーの `features` に `control-server` があれば読める |
| `api/write` | 409 `scenario-running` で拒否する |
| 時間のコマンド（`api/time`）と停止 | ふつうの実行と同じ |

- `control-server` があれば、サーバーは `--control` を付けます（書き込みはランチャーも拒否します）。なければ、`--control` を付けず、変数の状態は `off` で、`reason` に理由が入ります。
- 時間のコマンドと停止は、ランチャーの標準入力に書きます。Control Server がシナリオの実行中にクライアントに対して拒否する `Pause`、`Step`、`SetSpeed` などとは別で、ランチャーは受け付けます。

### シナリオのイベントと要約

- イベント `scenario_step`（手順が始まった `running` と、結果が決まったとき）は、ランチャーが書いたまま届きます（`index` が 1 以上の整数、`status` が文字列でないものは `bad-event`）。
- 手順の指摘（`scenario-failed` など）は、シナリオの `file` と `line` を持つ `finding` です。
- シナリオで止まった実行の結果の `simulation` は `ended` です（結果の `scenario` があることで、participant が止めた `ended` と区別します）。

要約の `scenario` は `{file, name, step_count, steps, steps_omitted, status, over_at_ms, timeline_url}` です。途中から開いた画面も、これで手順の表を作れます。

| フィールド | 内容 |
|---|---|
| `file` | 渡したシナリオ |
| `name`、`step_count` | 照合の結果の `scenario` から |
| `steps` | 届いた手順（`index` の順。手順ごとに最後の状態） |
| `steps_omitted` | 1000 手順を超えた数 |
| `status`、`over_at_ms` | 結果の `scenario` から（結果が来るまで `null`） |
| `timeline_url` | サーバーがその実行の時間の軸のファイルを配る所（`api/timeline/<session の id>`）。実行が終わるまでと、ファイルのない実行（下）では `null` |

- `steps[]` のフィールドはイベントと同じです。`text` と `message` は 1000 文字で切ります。
- 結果が来たら、`steps` は結果の手順（`not_run` を含む）になります。

### シナリオの実行の時間の軸（`api/timeline`）

ランチャーの `features` に `timeline` があると、サーバーはシナリオつきの実行のすべてに `--timeline <ファイル>` を付けます。ファイルはサーバーの一時ディレクトリに置きます。実行の結果の `result.scenario.timeline` と、ランチャーの `log` の行 `timeline: <ファイル>` が、そのファイルを指します（[docs/ja/simulation-run.md](../../docs/ja/simulation-run.md#シナリオの実行を時間の軸で記録する--timeline) の「シナリオの実行を時間の軸で記録する」）。`timeline` のないランチャーには `--timeline` を付けず、その実行には時間の軸がありません。

- 実行が終わったら、ランチャーがファイルを全部書けていれば、サーバーはファイルを残します。ファイルが空でなく、結果に指摘 `timeline-write` がないことです。このとき、要約の `scenario.timeline_url` が `api/timeline/<session の id>` になります。
- `GET api/timeline/<session の id>` は、ファイルを `application/json` で、`Content-Disposition: inline; filename="<シナリオの名前>.timeline.json"` を付けて返します。ほかの `api/` と同じく、トークンが要ります。残したファイルのない実行（シナリオがない、`timeline` のないランチャー、まだ動いている、ファイルを全部書けなかった、サーバーが出していない id、消えたファイル）は 404 `no-timeline` です。
- サーバーは、ファイルを書いた最後の 4 つの実行のファイルを残します。5 つ目を残すときに、いちばん古いものを消します。サーバーが止まるときに、全部を消します。
- ファイルを図にするページは、トークンなしで `timeline/` に配ります（`timeline/`、`timeline/timeline.html`、`timeline/page.js`、`timeline/vehicle-sil-timeline.js`。最後のものは要素 `<vehicle-sil-timeline>` で、サーバーの画面も読めます）。`Host` の確かめとヘッダーは、画面のファイルと同じです。ページは、画面と同じ所からトークンを読み（URL の `#token=`、なければタブの sessionStorage の、`takeToken` と同じ鍵）、`Authorization: Bearer` で送ります。開くときは `timeline/?src=/api/timeline/<session の id>` です。

## 操作の記録（`api/recording`）

GUI サーバーは、実行ごとに、自分を通した書き込み（`api/write`）のうち、反映されたもの（participant ごとの `operation` の `applied` と `applied_at_ms`）を覚えます。接続しているどの画面からの書き込みも、`api/write` を使うツールからの書き込みも対象です。`GET api/recording` は、それをシナリオの YAML にして返します（いまの実行か、終わった直後の実行。次の実行を始めるまで）。**サーバーはファイルを書きません**。画面がブラウザの保存として渡し、置き場所はブラウザで決めます。例は `recording.json` です。

`{"recording": {"session_id", "file", "state", "available", "reason", "writes", "omitted", "steps", "yaml"}}`

| フィールド | 内容 |
|---|---|
| `available` | この実行で覚えているか（変数のない実行と、シナリオの実行では `false`。`reason` が理由） |
| `reason` | `available` が `false` の理由か、`yaml` が `null` の理由（まだ反映された書き込みがない、など） |
| `writes` | YAML に入れた値の数 |
| `steps` | 手順（`set`）の数 |
| `omitted` | 入れなかった値の数 |
| `yaml` | シナリオの YAML（なければ `null`） |

- 入れなかった理由は YAML の先頭のコメントに書きます。理由は、上限の 2000 個を超えた、participant の名前に `.` がある、のどちらかです。シナリオでは最初の `.` までが participant の名前なので、`.` を含む名前の participant は指定できません。
- YAML の先頭のコメントには、何を記録したか、シナリオが最後の手順の時刻で終わること、最後の手順の後も実行を続けたいときに追加する手順の例（`expect` と `hold_for_ms`）を書きます。ランチャーは最後の手順の時刻でシミュレーションを止めます。記録した実行がそれより先まで進んでいれば、その時刻も書きます。
- YAML は `vehicle_sil_scenario: 1`、`name`、`steps` で、手順は `at_ms`（反映したステップの仮想時間）と `set` だけです。同じ時刻に反映された値は 1 つの `set` にまとめ、同じ変数の 2 つ目はその時刻の最後の値にします。
- 変数は `<participant>.<variable name>`、列挙は項目の名前（いつも引用符つき。名前が 1 つに決まらなければ値）です。

YAML の書き方は次のとおりです。

- 変数の名前は、英字か `_` で始まり、英数字、`_`、`-`、`.` だけで `.` を含むときだけ引用符なし、ほかは二重引用符です。
- 印字できる ASCII 以外の文字（改行、タブ、BOM、制御文字、ASCII でない文字）は `\uXXXX`（と `\UXXXXXXXX`）でエスケープします。`\xNN` は使いません。ランチャーの YAML の読み方は、`\xNN` を文字 U+00NN でなく 1 バイトとして読むためです。
- 実数はいつも `.` を含めて書きます（`1.0`、`1.0e+20`。YAML 1.1 と 1.2 のどちらでも数として読める）。有限でない数は書きません。

YAML に入れないものは次のとおりです。必要なものは、書き出した後でエディターで追加します。

- 時間の操作（一時停止、ステップ実行）。
- 反映されなかった書き込み（`late`、`rejected` など）。
- GUI サーバーを通さずに Control Server に直接書いたもの。
- 期待する値（`expect`）。
- 書き込んだ画面やツールの区別（1 つの実行の全部の書き込みが 1 つのシナリオになります）。

## ダッシュボードのファイル

`GET api/dashboard?file=<configuration file>` は、設定ファイルの隣の `<name without extension>.dashboard.yaml` を読み、形を確認して JSON で返します（書き方は [docs/gui.md](../../docs/ja/gui.md) の「ダッシュボードのファイル」、JSON Schema は [dashboard.schema.json](dashboard.schema.json)）。実行がなくても読めます。

| フィールド | 内容 |
|---|---|
| `path` | ダッシュボードのファイル（root からの相対） |
| `found` | ファイルがあるか。ないのはエラーではない（画面は変数を自動で並べる） |
| `errors` | エラーの並び（最大 50 個）。1 つでもあれば `dashboard` は `null` |
| `dashboard` | `{title, panels, sections}` |

- `errors[]`: `line` と `column` は 1 から、`at` はデータの中の場所（`sections[0].items[2].widget`）、`message` は直し方を含む英語の 1 文です。
- `panels` は `[{module, url}]` です。`url` は画面の base からの相対で、モジュールを読み込む先です（パスの部分ごとにパーセントエンコードしてあります）。
- `sections` は `[{title, collapsed, items}]` です。
- `items` は次のどれか 1 つの形です。読む側は、知らない形の項目を読み飛ばします。
  - `{variable: {peer, name}, widget, label, options}`（`widget` と `label` は省けば `null`）
  - `{panel, label, options, variables: [{peer, name}]}`
  - `{auto}`
- 変数があるか、コンポーネントが変数に合うかは、サーバーは確認しません（画面が、いまの変数の一覧で確認します）。
- `widget` は名前の形（小文字の英字で始まる）だけを確認し、どのコンポーネントがあるかは画面が確認します。
- YAML の別名（`*name`）、2 つ目の文書、YAML の型のタグ（`!!python/...` など）はエラーです。シンボリックリンクで root の外を指すファイルは読みません。

| 読み取りの対象 | 上限 |
|---|---|
| ファイルの大きさ | 256 KiB |
| 入れ子 | 16 段 |
| 値 | 50000 個 |
| セクション | 200 個 |
| 項目（全部で） | 2000 個 |
| パネルのモジュール | 16 個 |
| 1 つの値（文字列、数） | 1024 文字 |

### パネルのモジュール

`GET panels/<key>/<path relative to the root>` が、カスタムパネルの ES モジュールを `text/javascript`（`X-Content-Type-Options: nosniff`、ほかのレスポンスと同じ CSP、`Cache-Control: no-store`）で返します。

- `<key>` は起動ごとの乱数で、`api/dashboard` の `url` にだけ入っています。ブラウザはモジュールの import にヘッダーを付けられないので、トークンの代わりに URL に入れます。トークンとは別の値で、知っていてもパネルのモジュールを読めるだけです。
- 返すのは、root の下の `panels` という名前のディレクトリの中の `.js` / `.mjs` の通常のファイルだけです（1 MiB まで。リンクをたどった先も、その `panels` の中）。`panels` は root の下のどの場所のものでもかまいませんが、`panels` とその上のディレクトリがシンボリックリンクでないものに限ります。
- `.` で始まる名前、`..`、`\`、`:`、NUL を含むパスと、`<key>` の違うリクエストは 404 です。
- `Host` と `Origin` の検査はほかと同じです。

## 上限

サーバーが覚える量には上限があり、超えた分は数えて通知します。

### リクエストの本文と文

| 対象 | 上限 | 超えたとき |
|---|---|---|
| リクエストの本文 | 64 KiB。`api/check` と `POST api/config` は 6 MiB + 64 KiB（1 MiB の文を JSON がどうエスケープしても入る分。`\u0001` は 6 バイト） | 413 `too-large` |
| `api/check` と `POST api/config` の `text` | UTF-8 で 1 MiB | 413 `text-too-large` |
| `api/config` が読むファイル | 1 MiB | 413 `config-too-large` |

### ログ

| 対象 | 上限 | 超えたとき |
|---|---|---|
| source ごとの log | 直近 2000 行と 1 MiB | 古い行を落として数える |
| source の数 | 64 | それより後の source は共有の `*` |
| log の全体 | 16 MiB | |
| 1 行の log | 64 KiB | 覚えない（中継はする） |

### 要約

| 対象 | 上限 | 超えたとき |
|---|---|---|
| `check` | 4 MiB | `null` |
| `result` | 4 MiB | 入れ子でない短いフィールドだけ（例は `ws-kept-limit.json`） |
| `launcher_state` | 64 KiB | 前に覚えた 1 つを残し、サーバーが 1 回知らせる（`kept-limit`） |
| `findings` | 1000 個と 1 MiB | `findings_omitted` に数える |
| `participants` | 256 人で、1 人 4 KiB | `participants_omitted` に数える |
| `phase`、`time`、`registry`、`command` | 1 つ 4 KiB | 必ず入れるフィールドを残す |
| 知らないイベント | 覚えない | 中継だけする（再接続した画面には届かない） |

4 KiB を超えても必ず入れるフィールドは、次のとおりです。これだけで 4 KiB を超えるときは入れません。

| 対象 | 必ず入れるフィールド |
|---|---|
| participant | `name`、`state`、`pid`、`exit_code`、`signal`、`reason`、`stop_requested`、`control_link` |
| `phase` | 段階の名前 |
| `time` | `sim_ms`、`late_steps`、`max_late_ms`、`slipped_ms`、`late_ms` |
| `registry` | `uri` |
| `command` | `seq`、`command`、`accepted`、`ms`、`at_ms`、`reason`、`id` |

### `problems`

| 対象 | 上限 |
|---|---|
| ランチャーが書いたもの（標準エラーを除く） | 100 個 |
| サーバー自身の通知（`kept-limit`、`no-result`、`output-left-open`、`stop-not-delivered`、`stop-escalated`、`launcher-silent`） | 別に 50 個 |
| 標準エラー | 別に最初の 20 行と最後の 30 行 |

### ECU の変数

| 対象 | 上限 | 超えたとき |
|---|---|---|
| 一覧の participant | 256 人 | `peers_omitted` に数える |
| 一覧の変数（全部で） | 10000 個 | `variables_omitted` に数える |
| 変数の名前 | 256 文字 | 一覧に入れない |
| 列挙の項目 | 256 個 | 一覧に入れない |
| 知らない型の変数 | | 一覧に入れない |
| 単位、説明、列挙の項目の名前 | 256 文字 | 切る |
| 理由の文 | 500 文字 | 切る |
| 値 | participant ごとに最後の 1 組だけ | |
| 利用状態 | 変数ごとに最後の状態だけ。50 ms の間の変化を 1 つの `availability` の通知に | |
| 覚える操作 | 256 件 | 確定したものから忘れる |
| 要約に入れる操作 | 覚えている未確定の操作の全部と最後の 32 件 | |

### watch と書き込み、WebSocket

| 対象 | 上限 | 超えたとき |
|---|---|---|
| 接続ごとの watch | 16 個 | |
| 1 つの watch に名前で挙げる変数 | 2000 個 | |
| watch の間隔 | 20 ms から 60 秒 | |
| `api/write` の 1 回の値 | 256 個 | |
| 順番を待つ `api/write` | 32 個 | 503 `write-busy` |
| Control Server のレスポンスを待つ時間 | 5 秒 | |
| 画面が送る WebSocket のメッセージ | 256 KiB | |

### 値の読み取りにかける時間

- 値を読むのにかける時間は、長くても全体の半分で、読み取りは 1 つずつです。読むのが遅いときは、頼まれたより遅くなります。
- 1 回の呼び出しで、間隔の来た watch の participant をまとめて読みます。レスポンスの上限に入らず分けたときだけ、1 回の読み取りで複数の呼び出しを同時に出します。
- Control Server が、接続してから 10 秒たっても今の状態を送らなければ、再接続します。

## `examples/` の JSON

`examples/` の JSON は、サーバーのテスト（`gui/server/tests/test_protocol_examples.py`）が、本物のランチャーの出力の記録を再生するランチャー（`gui/server/tests/replay_launcher.py`）との実際のやりとりから作り、毎回照合します。

- ファイルに残すのは、形ごとに 1〜2 個と、ドキュメントや画面とライブラリのテストが読むものです。同じ形のほかの場合（ほかのエラーのコード、ほかの終わり方の実行）は、テストの中で、エラーのコードややりとりの終わり方を確認します。
- テストが作らないファイルが `examples/` に残っていてもテストは失敗します。
- 形を変えたら `UPDATE_PROTOCOL_EXAMPLES=1 scripts/verify-gui.py --skip-web` で書き直し、ライブラリの型と読み取り（`gui/client/src/protocol/`）と、その単体テスト（同じファイルを読む）を合わせます。
- プロセス ID、時刻、インスタンス、この PC のパス、実行ごとに変わる数は、決まった値に置き換えてあります（`t_ms` は `seq × 10`）。

## 関連するドキュメント

| したいこと | 読むもの |
|---|---|
| 自分の画面を作る | [自分の画面を作る](../../docs/ja/howto/gui-screen.md) |
| TypeScript から使う（接続、再接続、要約へのイベントの反映、時間のコマンド、ECU の変数の watch、書き込みと結果） | クライアントのライブラリ [gui/client](../client/README.ja.md) |
| 画面を使わない最小のクライアントを見る | リポジトリの [examples/gui_client/run.mjs](../../examples/gui_client/run.mjs)（素の JavaScript、依存なし、Node 22 以上） |
| ECU の変数を読み書きする例を見る | リポジトリの [examples/gui_client/variables.mjs](../../examples/gui_client/variables.mjs)（ライブラリを使う） |

`run.mjs` は、照合して実行を始め、イベントを読み、終わりを待ちます。`scripts/verify-gui.py` が、記録を再生するランチャーを相手に動かして確認します。`examples/gui_client/` は、ビルド済みのツールのアーカイブには入っていません。
