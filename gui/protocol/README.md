# gui/protocol

[日本語](README.ja.md)

The public specification of the HTTP and WebSocket API of the GUI server (`gui/server`): request and response shapes, WebSocket messages, error `code`s and limits. The sample screens (`gui/web`) use only this API, through the client library (`gui/client`). Your own screens and tools can do the same by following this document and the examples.

| To find | See |
|---|---|
| How new fields and events are handled | "Versioning and compatibility", "Rules for readers" |
| How to send the token; the `Host` and `Origin` checks | "Authentication and Origin" |
| Requests for check, start, stop and time commands | "HTTP" |
| The `code` of a rejected request | "Error codes (`code`)" |
| How to receive the progress of a run | "WebSocket (`ws`)", "Summary (`session`)" |
| The list, values and writes of ECU variables | "ECU variables" |
| Scenarios, recording actions, dashboard files | Their own sections |
| How much the server keeps; request sizes | "Limits" |

## Versioning and compatibility

The API version is `vehicle_sil_gui` (currently 4). The server writes it in the response of `GET api/app`, in the WebSocket `hello`, and in the line it prints when started with `--json`.

| Change | Version | What a reader does |
|---|---|---|
| Add a field, `type`, `code`, event, request, or optional field of the authentication message | Unchanged | Ignores or passes on what it does not know |
| Remove a field or request, change a type or meaning, change the rules on sending order | `vehicle_sil_gui` goes up | Does not read an `api/app` or `hello` of an unknown version, and reports the version mismatch |

- What a reader ignores or passes on: unknown fields, unknown `type`s, unknown events (`event`), unknown `code`s, and unknown values of fields whose set of values can grow (`phase`, `state`, `result` and others).
- On a version mismatch, waiting does not help, so the client does not reconnect. The library stops, with a `ProtocolError` for `api/app` and with `session.incompatible` for `hello`.
- **What is received again**: if a WebSocket message is not valid JSON, or the core fields of the `hello` or `session` summary (`id`, `state`, `launcher`, `stop`) cannot be read, the reader cannot tell what was lost. It reconnects and receives everything again from `hello`. The summary is the source of truth. Each time this happens again in a row, the wait before reconnecting grows.
- Launcher events (the contents of `event`) have the shape of the launcher's `serve [--once]` Events v4 stream (`vehicle_sil_events: 4` in `hello`; see [docs/simulation-events.md](../../docs/simulation-events.md)).
- The launcher's objects (the check and the run result in the summary and in `api/check`, findings, launcher events) are relayed with every key. The client library types the keys that screens need and keeps the launcher's whole object in `raw` (see "The launcher's keys (`raw`)" in [docs/gui-client-low-level.md](../../docs/gui-client-low-level.md#the-launchers-keys-raw)).
- Error `code` names do not change (new ones may be added). `message` is a human-readable sentence, not localized, and its wording may change. Programs branch on `code`.

## Authentication and Origin

| Check | What is accepted |
|---|---|
| Listen address | `127.0.0.1` only |
| `Host` | `127.0.0.1:<port>` only (during development, also the host of `--dev-origin`) |
| Token (HTTP) | `Authorization: Bearer <token>` (skipped only when the server is started with `--no-auth`) |
| Token (WebSocket) | The first message (`auth`) (skipped only when the server is started with `--no-auth`) |
| `Origin` (`POST` and the WebSocket handshake) | Only the server's own (`http://127.0.0.1:<port>`; during development, `--dev-origin`) |
| CORS | Not allowed. Pages from another origin cannot use the API |

- By default, the server creates a new token on every start. The token appears only after `#token=` in the URL that the server prints.
- With `--no-auth`, HTTP and WebSocket token authentication is disabled, printed/opened URLs omit `#token=...`, `--json` includes `"auth": false`, and a warning is written to stderr; `127.0.0.1`, `Host`, `Origin`, no-CORS, CSP and `--root` checks remain enforced.
- The screens the server serves (the sample screens and your own screen served with `--web`) read `#token=` from the URL they were opened with and remove it from the URL at once (`takeToken` in the client library).
- Never put the token in a URL query or a log (a token in the query is not accepted).
- The browser sets `Origin` itself. From outside a browser (Node, Python and others), set `Origin` to the server's own origin. For an example, see [examples/gui_client/run.mjs](../../examples/gui_client/run.mjs) in the repository (not included in the prebuilt tools archive).

## Rules for readers

A reader ignores and passes on unknown fields, unknown `type`s, unknown events (`event`) and unknown `code`s. It reads summaries and events part by part and skips only the parts it cannot read.

The server sends launcher events in the shape the launcher wrote them.

- `heartbeat` also passes through. Screens skip it. The server watches whether the launcher is alive and reports it in `launcher.silent_s` of the summary.
- An unknown participant `state` (such as `failed`, added by the launcher) also passes through and is merged into the summary.
- The server checks the shape of only some fields of an event (see `bad-event` below). Other fields pass through and are merged into the summary even if they are missing or of the wrong type (`ws-loose-fields.json`).

How the summary and events relate:

- **The summary is the source of truth.** The run summary ("Summary (`session`)" below) holds everything a screen needs to rebuild itself. When a screen receives `hello` or `session`, it replaces its local state with that summary.
- The server sends each event as `event` as soon as the launcher writes it. A screen applies the events it receives after `hello` to the summary.
- On reconnection (after `hello`), the server replays only log lines (after `after_stream_seq`, as far as it keeps them). Phase and participant events are not replayed; the summary holds them.
- The only state the server holds itself is what it received from the launcher and what it did to the launcher (start, stop requests).

## HTTP

Every `api/` request has these conditions:

- It needs `Authorization: Bearer <token>` unless the server was started with `--no-auth` (the token is the part after `#token=` in the URL the server printed).
- `Host` must be `127.0.0.1:<port>` (during development, also the host of `--dev-origin`).
- A `POST` is accepted only with the server's own `Origin` (during development, `--dev-origin`). A body needs `Content-Type: application/json`.
- Responses are JSON. A failure is `{"error": {"code", "message", "detail"}}`. `code` is the name programs read; see the table in "Error codes (`code`)" below. `message` is a human-readable sentence, not localized, that includes how to fix the problem.

### Endpoints

| Request | Response | Example |
|---|---|---|
| `GET api/app` | Information on the server and the launcher, whether variables can be read and written, limits | `app.json` |
| `GET api/configs` | The `.yaml` / `.yml` files under the root | `configs.json` |
| `GET api/config?file=<configuration file>` | The text of the file and the SHA-256 of its bytes: `{file, text, stamp}` ("The text of a configuration file" below) | `config.json` |
| `POST api/check` `{file, vehicle_sil_run_request, disable, enable, manual, record, record_after_fail_ms, scenario, case, after_scenario, variant, params, strict, connect, time_policy, speed, start_paused, stop_at_ms, late_tolerance_ms}` | `{file, disable, manual, scenario, exit_code, check}` | `check-*.json` |
| `POST api/check` `{file, text, variant, strict}` | The check of an unsaved text of the file, which starts no process; the same response as above | `check-text.json` |
| `POST api/describe` `{file, variant}` | `{file, variant, exit_code, check}`: a check of the saved file that describes its executables again, for the checks of its text | `describe.json` |
| `POST api/config` `{file, text, stamp}` | `{file, stamp}`: saves the text as the file, by default only an edit that keeps the commands that run ("Saving a text (`POST api/config`)" below) | `save-config.json`, `error-commands-changed.json` |
| `POST api/run` `{file, vehicle_sil_run_request, disable, enable, manual, record, record_after_fail_ms, scenario, case, after_scenario, variant, params, strict, connect, time_policy, speed, start_paused, stop_at_ms, late_tolerance_ms}` | `{instance, run_id}` (the identifier of the run it started; the summary arrives over WebSocket), or 422 `check-failed` with the findings ("Check and start" below) | `run.json`, `run-connect.json`, `error-check-failed.json` |
| `POST api/connections` `{file, network}` | `{file, network, exit_code, connections}`: where that network can be connected on this computer ("Connecting a network outside" below) | `connections.json` |
| `POST api/stop-session` `{run_id, instance, force}` | `{instance, run_id}` of the targeted run, or 409 `session-changed` | `stop*.json` |
| `POST api/time` `{command, ms, at_ms, value, id, run_id, instance}` | `{run_id, command}` (the command sent to the launcher, with its `id` if given) | `time.json` |
| `POST api/write` `{run_id, instance, writes, at_ms, late}` | `{run_id, operation}` once every target ECU has queued or settled the operation (final outcome arrives as `operation` over WebSocket) | `write.json` |
| `GET api/session` | `{instance, session, allowed, runtime}` (`session` is `null` if none; `allowed` as in "What the server accepts (`allowed`)" below) | `session-*.json` |
| `POST api/prepare` `{file, variant, disable, enable, manual}` | `{instance}`: prepares a persistent launcher when eligible (`runtime` updates arrive over WebSocket) | — |
| `GET api/dashboard?file=<configuration file>` | The dashboard file next to the configuration file: `{file, path, found, errors, dashboard}` | `dashboard*.json` |
| `GET api/scenarios?file=<configuration file>` | The scenarios in `scenarios` next to the configuration file: `{file, directory, found, scenarios, truncated}` | `scenarios.json` |
| `GET api/recording` | The applied writes of the current (last) run as scenario YAML: `{recording}` (`null` if there is no run) | `recording.json` |
| `GET api/timeline/<run_id>` | The timeline file of a scenario run, as the launcher wrote it (not wrapped in an object) | — |

- The `api/app` response holds the server version, `instance` (changes on every start), the root, `initial_file`, the launcher (`usable`, `problems`, the contents of `version --json`), whether variables can be read and written (`variables`: `available`, and `reason` if not), and the limits (`limits`). `limits` includes `speed_min`, `speed_max`, `step_ms_max`, and for variables `watches`, `watch_variables`, `watch_period_ms_min`, `watch_period_ms_max`, `writes_per_request`, `write_at_ms_max`, `write_timeout_s` and others.
- Each file in `api/configs` has `path` (relative to the root, separated by `/`), `modified`, `size` and `simulation` (whether it starts with `vehicle_sil_simulation`). The list as a whole has `truncated`.
- `check` in the `api/check` response is the result of `check --json` as is. The response is 200 even if the check fails.
- For `api/write`, see "ECU variables" below; for `api/scenarios` and `scenario`, "Scenarios"; for `api/recording`, "Recording actions (`api/recording`)"; for `api/timeline`, "Timeline of a scenario run (`api/timeline`)"; for `api/dashboard`, "Dashboard files". The `file` of `api/dashboard` goes through the same checks as for `api/check` (`invalid-path`, `outside-root`, `not-a-configuration`, `file-not-found`).
- Once the server has begun to shut down, `api/prepare`, `api/run`, `api/check`, `api/describe`, `POST api/config`, `api/connections`, `api/time` and `api/write` return 503 `shutting-down`.

### `disable` and `manual`

| What you send | Response |
|---|---|
| A participant name that is not in the check result of that file | 400 `unknown-participant` |
| The same name twice in one list | 400 `invalid-field` |

- A name in both `disable` and `manual`, and a name in `enable` that the file does not have, are for the launcher to refuse: they show as findings of the check (`api/check`) or as 422 `check-failed` (`api/run`).

A participant in `manual` is passed to the launcher in its request file; the launcher does not start it and waits for it to join (see "Starting a participant yourself (`manual`, `--manual`)" in [docs/simulation-config.md](../../docs/simulation-config.md#starting-a-participant-yourself-manual---manual)).

- `manual: true` in the configuration file appears in `participants[].manual` of the check result.
- The command to start the participant arrives in that participant's event (`state` is `manual`) as `command`, `working_directory` and `command_line`, and stays in `participants` of the summary (examples: `check-manual.json`, `ws-manual.json`).
- If the registry is not on a loopback address, the environment variables to set when starting arrive in `environment` of the same event (`{"VEHICLE_SIL_LISTEN_ON_NETWORK": "1"}`) and stay in the summary too. The server passes them on without looking at them.

### `speed` and `start_paused`

Send `speed` (`"max"` or a number) and `start_paused` (`true`) to choose them. Leave the field out to choose nothing: a `null` is refused (400 `invalid-field`), as for every field.

- A numeric `speed` must be from `limits.speed_min` to `limits.speed_max` (the same as the launcher).
- Without `speed`, the run goes as fast as possible unless the external real-time lock sets speed 1.

### Connecting a network outside (`connect`, `time_policy`, `api/connections`)

A `can` or `ethernet` network can be connected to a real bus through the launcher's bridge (see "Connecting a bus outside (`--connect`)" in [docs/simulation-run.md](../../docs/simulation-run.md#connecting-a-bus-outside---connect)). A screen does it in three steps:

1. **List**: `POST api/connections` `{"file": "simulation.yaml", "network": "ETH_EXT"}` runs `connections --json --network ETH_EXT` of the launcher and answers `{file, network, exit_code, connections}`. `connections` is the launcher's JSON as is ("`connections --json`" in docs/simulation-run.md): `networks[]` has one entry for each backend of this OS, and `list.targets[]` has `value`, `label`, `detail`, `usable` and `reason`. Ask for a network only when the user opens it: only that network's bridge is started, so a computer that runs SIL Kit only gets no Npcap prompt. `network` must be a network of the file (`networks[].name` of its check; every variant has the same networks, so the file is listed without one). For a name that the file lacks, the launcher's usage error becomes 400 `unknown-network`: the message quotes the launcher, and `detail` is `{network, findings}` (the launcher's findings). The list waits in the queue of the checks (503 `check-busy` as for `api/check`). Example: `connections.json`.
2. **Choose**: send `connect` `{"<network>": "<target>"}` in `api/check` and `api/run`, with a `value` of the list. `"none"` keeps the network on SIL Kit only and needs no list.
3. **Check and start**: the check says where each network goes (`networks[].outside`) and whether the run keeps real time (`time_lock`). `time_policy` (e.g. `"free"`) lifts that lock: the network is still connected, and the check gives the note `outside-not-real-time`. The launcher decides what a policy means and whether pause, step and speed are allowed (`allowed` of the summary). The server passes `connect` and `time_policy` on in the request file and has no rule of its own about the time. Examples: `check-connect.json`, `run-connect.json`.

**The guard (`target-not-listed`)**: for a network of the file, a target other than `"none"` is passed on only if the last list of that network of that file gave it with `usable: true`, written exactly so, and the file has not changed since that list. `api/check` compares after its check, in the queue of the checks; `api/run` compares right before it starts the launcher. Otherwise the answer is 400 `target-not-listed` (`detail` `{network, target}`; the message names the first usable values of the list) and nothing is started: list the network again and choose from it. So a value from the browser reaches a bridge's device only when the launcher has listed it on this computer. The comparison is of the exact text: `packet:enp3s0` is not the listed `enp3s0` here, although the launcher takes both as one target, so give the `value` as listed. Asking for a new list of a network ends the last one at once, and a list that gives no answer (502, 504, or the server's end) leaves none. Example: `error-target-not-listed.json`.

- A network in `connect` that the file lacks connects nothing: the server leaves it to the launcher, which leaves it out with the note `connect-network-unused`, as it does for a request file of a test bench (on the command line, `--connect` with such a network is an error). If the file changes between a run's check and its start, a run that connects anything is refused with `target-not-listed`, since the file may have that network by then.

- The server does not remember the choice from one run to the next: a screen keeps it if it wants to.
- The launcher's `outside` events and `outside[]` of its `state` event are passed on as they are.

### Check and start (`api/check`, `api/run`)

The optional integer `stop_at_ms` (1–10^12) gives the virtual stop time. The optional integer `late_tolerance_ms` (0–10^12) gives the wall-clock pacing tolerance. Both fields apply to Check and Start. The server returns HTTP 409 `launcher-unsupported` if either field is sent to a launcher without the `run-stop-at` feature. Check returns the resolved `time_step_ms` after Describe. The optional string `after_scenario` (`"stop"` or `"pause"`, default `"stop"`) chooses whether a passed scenario stops the run or pauses at the scenario end virtual time and hands over to manual control; `"pause"` requires `scenario` to be set and `stop_at_ms` to be omitted (400 `invalid-field` otherwise), and returns 409 `launcher-unsupported` when the launcher lacks `scenario-handover`. The optional non-empty string `case` selects a named case from a multi-case scenario; it returns 409 `launcher-unsupported` when the launcher lacks `scenario-cases`. When a multi-case scenario is checked without `case`, `POST api/check` still returns HTTP 200 with `check.result: "usage"` while preserving `participants`, `networks`, `hot_eligibility`, and `scenario` (`case: null`, `cases`, `case_details`, `params`, `param_meta`, `missing_params`).

- **Start**: of several `api/run` requests sent at the same time, only one starts a run. The others get 409 `starting` while a run is starting, and 409 `already-running` while a run is running. The response is `{instance, run_id}`, and the summary of the new run arrives over the WebSocket stream as `session`. **Ordering with WebSocket**: the server broadcasts the initial `session`, `variables`, and any buffered pre-start events over WebSocket *before* the `POST api/run` HTTP response resolves. External HTTP+WS clients must accept events (and even a fast `session` with `state: "ended"`) arriving before `POST api/run` returns (see [examples/gui_client/run.mjs](../../examples/gui_client/run.mjs) and [examples/gui_client/time-and-stop.mjs](../../examples/gui_client/time-and-stop.mjs)).
- **Start after a check**: `api/run` validates `disable`/`manual` participant names against the saved configuration and runs the launcher's own single `check` when starting. If the check does not pass (`result` is not `passed`), the run does not proceed and the response is 422 `check-failed`. Its `detail` is `{result, findings}`: `result` is the check's `result` (`failed`, `unreadable` or `usage`), and `findings` are the `findings` that `api/check` returns for the same request (the same objects). A check that passes with warnings or notes starts the run. Fix the findings, then send `api/run` again; the request is refused before any session becomes active, so there is nothing to stop. Example: `error-check-failed.json`.
- **The intent and the request file**: the body is `file` (this server's field: the configuration file, relative to the root) and the keys of the launcher's request file (`schema/run-request.schema.json`; "Request files (`--request`)" in [docs/simulation-run.md](../../docs/simulation-run.md#request-files---request)), with the same names and value forms: `variant` (a string), `params` (an object of parameter name to a string, a number or a boolean), `scenario`, `disable`, `enable` and `manual` (lists of participant names), `strict` (a boolean), `connect` (an object of network name to target) and `time_policy` (a string) ("Connecting a network outside" above), `speed` and `start_paused` (for `api/check` too: the launcher's check uses them for its rules about time and about a scenario). `vehicle_sil_run_request` may be sent as `1`, and `$schema` as a text (it is dropped). The names of the keys and the forms of the values are those of the request file; `file` is added, and `scenario` is a name from the listing of `api/scenarios`, not a path from the directory of the configuration file. The server checks the form of each value (the body is at most 64 KiB; of `api/check`, 6 MiB + 64 KiB) and the paths (`file`; `scenario` against the listing of `api/scenarios`), writes the intent into a request file of its own (readable by this user only; removed when the check or the run has ended) and gives the launcher `--request <that file>`. What the values mean for the file (is there that variant, can `disable` and `manual` be used together, does the scenario have those `params`) is the launcher's to say: it shows as a finding (`api/check`) or as 422 `check-failed` (`api/run`), and a finding that is about the request file names it "the request" (the temporary file is gone by the time you read it). `params` without `scenario` is not refused: as with any request file, the launcher uses `params` only when a scenario runs, and ignores them otherwise. The names that `disable` and `manual` may take are those of the check of that variant.
- **A choice that is not made is a key that is left out.** A `null` is refused (400 `invalid-field`). The keys of the launcher's side of a run (`control`, `timeline`, `log_dir` and others) are unknown fields (400 `invalid-field`): the server decides them. The `scenario` to send is the `path` that `api/scenarios` lists (relative to the root); the server writes it relative to the directory of the configuration file, which is where the request file reads it from. A launcher without the feature `request` cannot be used (`api/app` says so in `launcher.problems`).
- **Check**: `api/check` requests sent at the same time are not rejected; they run one at a time. If too many requests are waiting, or the wait exceeds the check time limit (`limits.check_timeout_s` in `api/app`), the response is 503 `check-busy`. `api/run` validates metadata through the launcher (`check --file-only --json` or resident `prepare`, when supported) and checks once inside the started launcher run using `limits.check_timeout_s`.

### The text of a configuration file (`api/config`, `text` of `api/check`, `api/describe`)

For a screen that edits a configuration file and checks it before it is saved.

- **Reading**: `GET api/config?file=<configuration file>` returns `{file, text, stamp}`. `text` is the bytes of the file read as UTF-8, as they are: a BOM stays at its start, and the line ends (CRLF, a last line without one) are not changed. `stamp` is the SHA-256 of those bytes (64 lowercase hexadecimal digits). `file` goes through the same checks as for `api/check`, and must be a configuration file as `api/configs` tells one: a path with a part that starts with `.` (also after following links), or a file without `vehicle_sil_simulation` in its first 4096 bytes (a UTF-16 file among them), is 400 `not-a-configuration`, so no other YAML file under the root is read for a screen. A file larger than 1 MiB is 413 `config-too-large`; a file that is not UTF-8 text (a NUL byte, bytes that are not UTF-8) or that cannot be read is 422 `config-unreadable`. Example: `config.json`.
- **Checking a text**: `api/check` with `text` (a string) checks that text as the content of `file`: its relative paths are from the directory of `file`, and the findings name `file`. `file` is a configuration file that exists under the root, as for `api/config`. With `text`, the body takes only `file`, `variant` and `strict` (and `$schema` and `vehicle_sil_run_request`); any other field is 400 `invalid-field`, because a check of a text checks the configuration, not the choices of a run. The response has the same shape as without `text` (`disable` and `manual` are `[]`, `scenario` is `null`). `text` is at most 1 MiB as UTF-8 (413 `text-too-large`). Example: `check-text.json`.
- **No process starts**: the launcher checks the text with `--text-from-stdin` and `--descriptions` ("Saved descriptions" in [docs/simulation-check.md](../../docs/simulation-check.md#saved-descriptions---write-descriptions---descriptions)). It takes each participant's `--describe` output from descriptions that the server saved from a check of the saved file, if the participant's `command` and its executable are as they were then; any other participant is `not_described`. The server keeps those descriptions per `file` and `variant` in a temporary directory of its own (removed when it stops; no answer names them). When it has none, or the saved file has changed since they were made, it first checks the saved file (with that `variant` only), which starts each executable with `--describe`, as the check of the Run screen does; both checks take one turn of the queue of the checks. If that check cannot read the file or its variant (`unreadable`, `usage`; a variant that only the text has, say), or ends in an error (`check-timeout`, `launcher-output`, `launcher-start`), the text is checked with no descriptions, and the saved file is not checked again for its texts until it changes or `api/describe` asks for it (whose answer shows what went wrong). A participant without saved descriptions is `not_described`, and the check still `passed` even when every participant is: `participants[].status` says which were not compared; `api/describe` describes them again (a participant that only the text has needs the file saved first).
- **Describing again**: `POST api/describe {file, variant}` (`file` as for `api/config`) checks the saved file and saves its descriptions again (after an executable was built again, say). The response is `{file, variant, exit_code, check}` with that check (`variant` is `null` when none was given). Example: `describe.json`.
- **What the server keeps of the saved file**: the names that `disable` and `manual` may take and the lists of `api/connections` are those of the saved file; a check of a text neither uses nor changes them. `api/check` without `text` and `api/run` save no descriptions.
- Both go through the queue of the checks, as `api/check` does (`check-busy`, `check-timeout`).

### Saving a text (`POST api/config`)

`POST api/config {file, text, stamp}` writes `text` as the content of `file`. Whether an edit may be saved is the launcher's rule: the server has it check the text with `check --compare-commands` against the saved file ("Comparing the commands that run" in [docs/simulation-check.md](../../docs/simulation-check.md#comparing-the-commands-that-run---compare-commands)) and reads only `commands` and `result` of that check; it compares no YAML itself. By default it saves only an edit whose `commands` is `"same"`: the networks, the mapping of the networks, `external` and the other keys that the launcher does not compare. The options of the server change that: with `--no-save` nothing is saved; with `--allow-command-changes`, an edit that changes the commands that run, and a new file, are saved too (then whoever has the token can run any command as the user of the server: see "Security" in [docs/gui.md](../../docs/gui.md#security)). `allowed.save` says which applies ("What the server accepts (`allowed`)" below).

The body has exactly these 3 fields (another field is 400 `invalid-field`). `text` is at most 1 MiB as UTF-8 (413 `text-too-large`; the body may be as large as that of `api/check`). `stamp` is the `stamp` that `GET api/config` gave for the file, or `null` for a new file. The server goes through these steps in order and stops at the first refusal:

1. With `--no-save`: 403 `save-disabled`.
2. Where. With a `stamp`, `file` is an existing configuration file, as for `api/config` (400 `not-a-configuration`, 404 `file-not-found`, ...). With `stamp` `null` and no `--allow-command-changes`: 403 `commands-changed` (a new file has no saved file to compare with). With `--allow-command-changes`, a new file must end in `.yaml` or `.yml`; its directory must exist (404 `file-not-found`); a file that is there already is 409 `config-changed`. Both an existing file and a new one must be where `api/configs` lists configuration files: under the root also after following links (400 `outside-root`), through no linked directory (a file that is a link is fine), with no part that starts with `.`, at most 6 directories below the root, not a dashboard file (`*.dashboard.yaml`) and not in a directory named `scenarios`, by the name sent or after following links (400 `not-a-configuration`). `text` must have no NUL character and must have `vehicle_sil_simulation` in its first 4096 bytes (422 `config-unreadable`). So the server writes only configuration files that `api/configs` lists and that `api/config` reads again.
3. The stamp: the SHA-256 of the file's bytes now differs from `stamp`: 409 `config-changed`. Read the file again, make the edit on that text and save with the new stamp.
4. The launcher checks the text as the content of `file` with `--text-from-stdin` and the saved descriptions, so no process starts, and with `--compare-commands <file>` (not for a new file). A text whose `result` is `unreadable` or `usage` is 422 `config-unreadable`, whatever the options. Without `--allow-command-changes`, `commands` other than `"same"` (`"changed"`, `"not_compared"`, `null`) is 403 `commands-changed`: what cannot be compared is not saved. The message has the launcher's finding (`commands-changed`, `commands-not-compared`), and `detail` holds `{result, commands, findings}` of that check. An error elsewhere in the text (`result` `failed`) does not stop a save.
5. The file is resolved once more (a symbolic link that now leads to another file is 409 `config-changed`; the launcher of step 4 compared with that resolved file) and the stamp is compared once more, then the server writes the text as UTF-8 bytes, as it is (a BOM, CRLF and a last line without a line end stay as sent), to a new file in the same directory whose name starts with `.vehicle-sil-gui-save-` (never listed), and puts that file in the place of `file`. An existing file keeps its mode; a symbolic link keeps pointing to the file, which gets the new content. A file that cannot be replaced (another program has it open on Windows, the file is read-only for this user, the directory cannot be written) is 409 `config-busy`, and the new file is removed. A new file never replaces a file that another program made after step 2 (409 `config-changed`): it is put in place with a hard link, or, on a file system without them, an empty file is made first and fails when one is there. On POSIX, the directory is synced after the replacement.

The response is `{file, stamp}`, with the stamp of the saved content. A save takes one turn of the queue of the checks (503 `check-busy` as for a check). What the server kept of the old content is dropped: its descriptions are made again before the next check of its text, so the executables of the saved file describe themselves then (with `--allow-command-changes`, a new command runs with `--describe` at that point), and a `connect` needs a new list of `api/connections`. The second comparison of the stamp narrows the time in which another program can change the file between the check and the replacement; it does not close it. Examples: `save-config.json`, `error-commands-changed.json`.

### Run targeting (`instance` and `run_id`)

Obtain `instance` from `GET api/app`, `GET api/session`, `POST api/run`, or WebSocket `hello`, and `run_id` from `POST api/run`, `GET api/session` (`session.id`), or WebSocket `hello` / `session` / `event`. The three run-scoped POST endpoints differ in how they check these fields:

| Endpoint | `run_id` and `instance` rule | On mismatch |
|---|---|---|
| `POST api/stop-session` | Both `run_id` and `instance` are **required** (`400 invalid-field` if either is missing or empty) | `409 session-changed` (nothing stopped) |
| `POST api/time` | **Both or neither**: Protocol v4 preserves omitting both unchanged for compatibility (targeting whichever run is current), but new clients, AI agents, and CI automation should always send both `instance` and `run_id`. Sending only one returns `400 invalid-field` | `409 session-changed` (nothing sent) |
| `POST api/write` | `run_id` is **required**; `instance` is **optional** (checked if present; empty/non-string returns `400 invalid-field`) | `409 session-changed` (nothing written) |

### Stop (`api/stop-session`)

The GUI Client requires an `instance` in successful `api/session` and `api/run` responses. If it is missing, the operation fails with a message to use the GUI server from the same release. A start request may already have reached the server. Check the current run before retrying.

To stop a run, use `POST api/stop-session` with `{"run_id": <string>, "instance": <server instance>, "force": <boolean>}` (`force` is optional; if omitted, it is `false`). `instance` is the identifier in the `api/run` or `api/session` answer or the WebSocket hello. If the instance differs, or another run is current or starting, it returns 409 `session-changed` and sends no stop. The check and the stop are made together, before another request can change the current run. A field other than `run_id`, `instance` and `force` (or a null) is refused with 400 `invalid-field` and nothing is stopped, as for the other endpoints.

| `force` | What it does | If a stop is already requested |
|---|---|---|
| `false` | Stops in order (`{"command":"stop"}` on the launcher's stdin) | Nothing |
| `true` | Stops without waiting (`stop` twice if no stop was requested yet, once more if one was) | Nothing |

- Returns `{instance, run_id}` instead of 409 when the targeted run has already ended (calling it again gives the same result).
- The GUI Client sends `api/stop-session` with the `run_id` and `instance` it knows. It holds a stop requested during its own start until the start answer identifies that run. If no run is known or the start answer is lost or unreadable, it reports that no stop was sent; the run may have started. Inspect the current session before asking to stop again. This prevents a delayed stop from affecting a later run started by another screen.
- The progress of the stop shows in `stop` of the summary and in the launcher's `command` / `phase` events.
- A launcher that does not read stdin does not stop this way (stopping the server stops it with a signal).

### Time commands (`api/time`)

Machine-readable contracts: [openapi.yaml](openapi.yaml) (all 17 `/api/*` HTTP endpoints, including `POST /api/time` with `run_id` ↔ `instance` `dependentRequired`, `TimeCommandAck`, and `ErrorResponse`) and [schemas/ws-command-event.schema.json](schemas/ws-command-event.schema.json) (asynchronous WebSocket `command` event), alongside [schemas/ws-server.schema.json](schemas/ws-server.schema.json), [schemas/ws-client.schema.json](schemas/ws-client.schema.json), and [schemas/cli-json.schema.json](schemas/cli-json.schema.json). All schemas are generated offline from `gui/server/src/vehicle_sil_gui/contracts.py` and `server.route_specs()` without starting a server (`python -m vehicle_sil_gui.openapi --write` / `--check`) and are bundled under `protocol/` in the prebuilt tools archive.

The body is one of the following commands, with the optional fields described below.

| Body | Range |
|---|---|
| `{"command": "pause"}` | |
| `{"command": "resume"}` | |
| `{"command": "step", "ms": <integer>}` | 1 to `limits.step_ms_max` |
| `{"command": "run_until", "at_ms": <integer>}` | 0 to `limits.step_ms_max` |
| `{"command": "speed", "value": <"max" or a number>}` | Same range as `speed` |

Each body may also have `"id"`, a whole number from 0 to 2^53. The server passes it to the launcher in the command line, and the launcher returns it as `id` in the `command` event that answers that line (`null` when the line had none). A screen uses it to tell the answer to its own command from the answers to the commands of other screens or of the launcher's stdin; the server does not read it. The response carries `{run_id, command}` with its `id`. An `id` that is not such a number gives 400 `invalid-field`.

To target a specific run, **include both `instance` and `run_id`** (non-empty strings from `api/run`, `api/session`, or WebSocket `hello`). Protocol v4 preserves omitting both `run_id` and `instance` unchanged for compatibility (targeting whichever run is current when the server handles the request), but **new clients, AI agents, and CI automation should always send both `instance` and `run_id`**. The server checks both fields together before writing to the launcher; a mismatch, an absent run, or a run being replaced returns 409 `session-changed`. Neither field is passed to the launcher or included in the response's `command`. **Sending only one of `run_id` or `instance` returns 400 `invalid-field`** (as does an empty, null, or unknown field).

The server writes the command as one line to the stdin of the running run's launcher and returns HTTP 200 immediately without waiting for the launcher's decision. **HTTP 200 acknowledges forwarding to launcher stdin only—not that the launcher accepted or applied the command.**

- Whether the command was accepted, and why not, arrives asynchronously in the launcher's WebSocket `event` message (`event: "command"` with `accepted`, `message`, `reason` (always `null` when `accepted` is `true`), `id`, optional `asked_run` on launcher-level refusals outside a run window, `ms` and `at_ms` for an accepted `step`, and `at_ms` for an accepted `run_until`). The `time` and `state` events show that the command took effect.
- **Pre-setup window**: immediately after `POST api/run` returns 200, `launcher_state` may still be `null` or in setup (`allowed.<cmd>.ok === false`, `reason: "not-set-up"`). Except when `launcher_state.allowed.<cmd>.reason` is `"not-running"` (which returns HTTP 409 `not-running` immediately), `POST api/time` returns HTTP 200 and the launcher queues commands until setup finishes, then emits `command` events. External callers should wait for `launcher_state.allowed.<cmd>.ok === true` (or `phase === "running"` / `holding === true`) before sending commands, or handle async `command` refusals.
- **Correlation termination and `last_command`**: if a run ends (`session.state === "ended"` or `!session.launcher.running`) right after HTTP 200, the launcher may exit after `result` without ever emitting a `command` event. Clients waiting on a WebSocket `command` event matching `event.id` must abort the wait when the run ends or on a timeout (see [examples/gui_client/time-and-stop.mjs](../../examples/gui_client/time-and-stop.mjs)). Prefer streaming WebSocket `command` events matched by `id` over polling `session.last_command`, because `last_command` keeps only the latest single command and overwrites earlier answers under concurrent commands.
- If the request has no target or its target matches, but the launcher cannot take a command, the response is 409 `not-running`. This includes a run that ended or is starting, closed launcher input, and a launcher state of `not-running`. The server lets the launcher answer for any other reason in `allowed`, since it may have changed.
- Even if the launcher never reads its input, the request returns within about 2 seconds (the command stays in the launcher's input).
- What the commands mean (a pause takes effect at a step boundary, stepping works only while paused and so on) is defined by the launcher (see "Stdin commands (`run --stdin-commands`)" in [docs/simulation-events.md](../../docs/simulation-events.md#stdin-commands-run---stdin-commands)).

## Error codes (`code`)

The codes that screens look at. The names do not change (new ones may be added).

| HTTP | `code` | When |
|---|---|---|
| 400 | `invalid-json` | The body is not a JSON object |
| 400 | `invalid-field` | A field has the wrong type or is out of range, or the body of `api/check` or `api/run` has a field that the request does not take (which fields: below the table) |
| 400 | `invalid-path` | `file` is not a path string relative to the root (empty, too long, `\`, NUL, `:`, unpaired surrogate) |
| 400 | `outside-root` | Absolute path, `..`, symbolic link out of the root |
| 400 | `not-a-configuration` | Not a regular `.yaml` / `.yml` file; for `api/config`, `text` of `api/check` and `api/describe`, also a path with a part that starts with `.`, or a file without `vehicle_sil_simulation` in its first 4096 bytes |
| 400 | `unknown-participant` | A name in `disable` or `manual` is not in the check result |
| 400 | `unknown-network` | `api/connections`: the file has no such network (the launcher's usage error; `detail` holds `{network, findings}`) |
| 400 | `target-not-listed` | A target of `connect` (other than `none`) of a network of the file is not usable in the last list of that network of that file, or the file has changed since (`detail` holds `{network, target}`; "Connecting a network outside" above). Example: `error-target-not-listed.json` |
| 400 | `scenario-not-listed` | `scenario` is not one of the scenarios that `api/scenarios` lists for that configuration file (no `scenarios`, a link, a file elsewhere) |
| 400 | `unsupported` | Uses a feature the launcher does not have (which: below the table) |
| 400 | `unknown-variable` | `api/write`: the participant or variable is not in the run's variable list (`detail` holds `{peer, name}`) |
| 400 | `not-writable` | `api/write`: an output variable (`detail` holds `{peer, name}`) |
| 400 | `write-refused` | `api/write`: the launcher's Control Server refused it (the message gives the reason) |
| 401 | `unauthorized` | Missing or wrong token (with `WWW-Authenticate: Bearer`) |
| 403 | `forbidden-host` | `Host` is not `127.0.0.1:<port>` |
| 403 | `forbidden-origin` | `Origin` is not the server's own (`POST` and WebSocket need `Origin`) |
| 403 | `cors-not-allowed` | `OPTIONS` (CORS is not allowed) |
| 403 | `save-disabled` | `POST api/config`: the server was started with `--no-save` |
| 403 | `commands-changed` | `POST api/config`: the edit changes the commands that run, they could not be compared, or the file is new, and the server was started without `--allow-command-changes` (`detail` holds `{result, commands, findings}` of the launcher's check, or `null` for a new file). Example: `error-commands-changed.json` |
| 404 | `file-not-found` | `file` is not under the root |
| 404 | `not-found` | No such API or screen file |
| 404 | `no-timeline` | `api/timeline/<run_id>`: the server has no timeline file of that run (see "Timeline of a scenario run (`api/timeline`)") |
| 405 | `method-not-allowed` | A method other than `GET` on a screen file |
| 409 | `already-running` | `api/run`: a run is running |
| 409 | `starting` | `api/run`: another `api/run` is starting a run (the launcher has not been registered yet) |
| 422 | `check-failed` | `api/run`: the check of what the run is made of did not pass, so no run was started (`detail` holds `{result, findings}`; "Check and start" above). Example: `error-check-failed.json` |
| 409 | `not-running` | `api/time`, `api/write`: no run is running |
| 409 | `launcher-unsupported` | `api/check`, `api/run`: `stop_at_ms` or `late_tolerance_ms` was sent to a launcher without `run-stop-at`, `after_scenario: "pause"` was sent to a launcher without `scenario-handover`, `case` was sent to a launcher without `scenario-cases`, non-empty `record` was sent to a launcher without `record-signals`, or `record_after_fail_ms` was sent to a launcher without `record-after-fail` |
| 409 | `scenario-running` | `api/write`: the running run is running a scenario before handover (not written, so that the scenario result does not change). Example: `error-scenario-running.json` |
| 409 | `session-changed` | `api/stop-session` and targeted `api/time`: the server instance or current run changed. `api/write`: the requested run ID differs from the running run. No operation was sent. Example: `error-session-changed.json` |
| 409 | `config-changed` | `POST api/config`: the file's content no longer has `stamp`, or a new file (`stamp` `null`) is there already |
| 409 | `config-busy` | `POST api/config`: the file could not be replaced (the message says why) |
| 409 | `variables-not-ready` | `api/write`: the variable feature is not `ready` (the message includes `variables.reason` of the summary) |
| 413 | `too-large` | The body exceeds 64 KiB (of `api/check` and `POST api/config`: 6 MiB + 64 KiB) |
| 413 | `text-too-large` | `api/check`, `POST api/config`: `text` exceeds 1 MiB as UTF-8 |
| 413 | `config-too-large` | `api/config`: the file exceeds 1 MiB |
| 415 | `unsupported-media-type` | There is a body, but not `Content-Type: application/json` |
| 422 | `config-unreadable` | `api/config`: the file is not UTF-8 text (a NUL byte, bytes that are not UTF-8) or cannot be read (also `text` of `api/check` and `api/describe`: cannot be read). `POST api/config`: the launcher cannot read the text as a configuration (`detail` holds `{result, commands, findings}`), or the text has no `vehicle_sil_simulation` in its first 4096 bytes. Example: `error-config-unreadable.json` |
| 500 | `internal` | A server bug (no example; if it happens, look at stderr) |
| 502 | `launcher-start` | The launcher cannot be started |
| 502 | `launcher-output` | `check --json` did not write a check result, or `connections --json` no list |
| 502 | `write-failed` | `api/write`: the write did not reach the Control Server, or the connection broke before every target ECU queued it (once an operation number was assigned, `detail` holds `{operation, state: "sent"}`) |
| 503 | `launcher-unusable` | The launcher cannot be used (the reason is in `launcher.problems` of `api/app`) |
| 503 | `check-busy` | Too many checks (and lists of `api/connections`, and saves) waiting, or waiting too long |
| 503 | `write-busy` | `api/write`: 32 writes are already in flight, or an earlier write held submission for longer than `limits.write_timeout_s`, so this write was not sent |
| 503 | `shutting-down` | The server is shutting down |
| 503 | `too-many-connections` | WebSocket connection limit (handshake response) |
| 500 | `request-file` | The temporary file that carries the request to the launcher could not be made (no space, or a temporary directory that cannot be written: set `TMPDIR`) |
| 504 | `check-timeout` | The check (or the list of `api/connections`) did not finish within the time limit (it was stopped) |
| 504 | `write-timeout` | `api/write`: the Control Server or target ECUs did not queue the write within `limits.write_timeout_s` (once an operation number was assigned, `detail` holds `{operation, state: "sent"}`). It may still take effect (do not resend) |

- `api/check` and `api/run` take the same fields: `file`, `$schema`, `vehicle_sil_run_request`, `scenario`, `case`, `after_scenario`, `record`, `record_after_fail_ms`, `variant`, `params`, `disable`, `enable`, `manual`, `strict`, `connect`, `time_policy`, `speed`, `start_paused`, `stop_at_ms` and `late_tolerance_ms`. With `text`, `api/check` takes only `file`, `$schema`, `vehicle_sil_run_request`, `variant`, `strict` and `text`. `api/describe` takes `file` and `variant`. `POST api/config` takes `file`, `text` and `stamp` (`stamp` is required, and may be `null`). `api/connections` takes `file` and `network`. Any other field (a misspelled one, or a key of the launcher's side) and any `null` give 400 `invalid-field`, and the message names every unknown field and the fields the request takes, so that a field is never ignored without notice.
- Fields covered by `invalid-field`: `speed`, `start_paused`, `stop_at_ms`, `late_tolerance_ms`, `after_scenario` (also `"pause"` without `scenario` or together with `stop_at_ms`), `record`, `record_after_fail_ms`, `variant`, `params`, `enable`, `strict`, `connect`, `time_policy`, `network`, `force`, `disable`, `manual` (also the same name twice), `command`, `ms`, `at_ms`, `value`, `id`, `run_id` and `instance` of `api/time` (including sending only one of `run_id` or `instance`), `writes`, `at_ms` and `late` of `api/write`, and values that do not fit the variable's type or range. For a variable value, `detail` holds `{peer, name}`.
- Case of `unsupported`: `api/write` sent when the launcher lacks `control-server`. (The server requires `time-commands`, `stdin-run-until`, `state`, `speed`, `start-paused`, `scenario` and `manual-participants` when it starts, so `api/time` has no such case.)

The example files. The GUI server's tests check the other codes.

| File | Contents |
|---|---|
| `error-already-running.json`, `error-check-failed.json`, `error-commands-changed.json`, `error-config-unreadable.json`, `error-not-writable.json`, `error-scenario-not-listed.json`, `error-scenario-running.json`, `error-session-changed.json`, `error-target-not-listed.json` | HTTP errors |
| `error-invalid-watch.json` | WebSocket `error` |
| `stop.json`, `stop-force.json`, `stop-ended.json` | Stop responses |
| `time.json` | Time command response |

## WebSocket (`ws`)

### Authentication message

After connecting, first send `{"type": "auth", "token": "...", "instance": "...", "run_id": "<id>", "after_stream_seq": <n>}`. When the server was started with `--no-auth`, it does not wait for `auth` and sends `hello` and `synced` immediately: any `auth` message (including reconnect delta-resume fields `instance`, `run_id`, and `after_stream_seq`) is ignored, and `hello.replay.after_stream_seq` is always `null` (full buffered replay).

| Field | Contents |
|---|---|
| `token` | The token from the URL the server printed |
| `instance`, `run_id`, `after_stream_seq` | Send these only when reconnecting to the same server instance and run, to tell what stream position you already have. A value that does not match the current instance and run counts as absent |

### Close codes and the connection limit

| What happened | What the server does |
|---|---|
| Wrong token | `error` (`code: unauthorized`), then close code 4401 (reason text `unauthorized`) |
| No `auth` within 5 seconds | Close code 4401 without an `error` (reason text `auth-timeout`) |
| The server shuts down | Close code 1001 |
| A handshake while 32 authenticated connections exist | 503 `too-many-connections` |
| The 32 slots filled up between the handshake and authentication | Close code 1013 (reason text `too-many-connections`) |
| More than 16 unauthenticated connections | Closes the oldest unauthenticated connection with close code 1013 (reason text `too-many-unauthenticated`) |

- The 16 unauthenticated connections are counted separately from the 32 authenticated ones.
- A screen closed with 1013 reconnects.
- JavaScript in a browser cannot see the reason for a rejection at the handshake (`Host`, `Origin`, `too-many-connections`); it sees 1006. When the handshake fails, a screen calls `GET api/app` once to find the reason.

### Messages after authentication

After authentication, the server sends, in this order:

1. `hello`: `instance`, `stream_seq`, `session` (the summary, or `null`), `allowed` (the same as `session.allowed` when there is a run; with no run, what the server accepts), `replay` (`after_stream_seq`: where the replay starts; `null` means from the beginning), `runtime` (`{mode, state, config, reason, message}`). The screen rebuilds its state from this summary.
2. `dropped` (if any): the number of log lines after `after_stream_seq` that the server cannot send because it no longer keeps them (per source).
3. `event`: the kept log lines after `after_stream_seq`, in `stream_seq` and `seq` order.
4. `synced`: `{stream_seq, run_id, last_seq}`. The end of the replay.
5. From then on, as things happen: `event`, `session`, `allowed`, `runtime`, `error`, `dropped`, `resync`, `variables`, `values`, `operation`, `availability`.

If `instance` or `run_id` differs from the current instance and the `id` of the `hello` summary, or `after_stream_seq` does not belong to the current session, the replay starts from the beginning (`replay.after_stream_seq` is `null`).

After authentication, a screen can send variable `watch` and `unwatch` messages ("ECU variables" below). A JSON object whose `type` is not a string gets an `error` (`code: invalid-message`); an unknown `type` is skipped. Either way, the connection stays open.

### Message types

| `type` | Contents | Example |
|---|---|---|
| `hello` | Item 1 above | The first message of `ws-*.json` |
| `event` | `{stream_seq, run_id, event}`. The launcher event as is (after the server checked its shape) | `ws-pass.json`, `ws-log-dropped.json`, `ws-time.json` (time commands), `ws-late.json` (lateness) |
| `synced` | `{stream_seq, run_id, last_seq}` | `ws-pass.json` |
| `session` | `{stream_seq, session}` (the summary) | `ws-pass.json` |
| `allowed` | `{stream_seq, run_id, allowed}`. What the server accepts changed, and no `session` follows to carry it ("What the server accepts (`allowed`)" below). `run_id` is the id of the current run, or `null` | `ws-allowed.json` |
| `runtime` | `{stream_seq, runtime}`. Persistent launcher state (`mode`, `state`, `config`, `reason`, `message`) changed | — |
| `error` | `{stream_seq, run_id, code, message, detail}`. Something the server noticed ("The `code` of `error` messages" below). Also kept in `problems` of the summary | `ws-bad-lines.json` |
| `dropped` | `{run_id, reason, log, at_least, events}`. Counts of what was not sent to this screen (outside `stream_seq`) | `ws-dropped.json`, `ws-slow-client.json` |
| `resync` | `{reason, message}`. The screen receives too slowly, and even notifications that must not be dropped overflowed. Right after this, the server sends everything again from `hello` | `ws-resync.json` |
| `variables` | `{stream_seq, run_id, variables}`. The state or list of that run's variable feature (same shape as `variables` in the summary) | `ws-variables.json`, `ws-variables-failed.json` |
| `values` | `{id, watch_id, run_id, sim_ms, paused, peers}`. The last values of the watched variables ("ECU variables" below; outside `stream_seq`) | `ws-variables.json` |
| `operation` | `{stream_seq, run_id, seq, state, done, peers}`. The result of a write changed ("ECU variables" below). Sent to every connection, never dropped | `ws-variables.json` |
| `availability` | `{stream_seq, run_id, peers}`. Whether the ECUs use their variables changed ("ECU variables" below). Sent to every connection | `ws-availability.json` ("Availability" below) |

The server sends `session` at these times:

- When a run starts, and when a stop is requested.
- When a run ends (`launcher.running` is `false` and `state` is `ended`; after the launcher's last event).
- When the launcher stops writing anything (when `launcher.silent_s` changes), and when it starts writing again.

The server sends `variables` right after a run starts (immediately after that `session` notification) and whenever it changes. Every ordered notification on the WebSocket stream carries a monotonic `stream_seq` within the server `instance`, so `session`, `event`, `allowed`, `runtime`, `variables`, `availability`, `operation` and `error` share one total order without separate per-channel revision counters. **Replay `stream_seq` ordering (`hello` → replay → `synced`)**: `hello.stream_seq` is the current server head, whereas replayed `event` messages sent between `hello` and `synced` carry their older historical `stream_seq` values (monotonically increasing within the replay, and often lower than `hello.stream_seq`). Live monotonic ordering resumes from `synced.stream_seq` (which is `>= hello.stream_seq`); clients must not discard replayed events for having `stream_seq < hello.stream_seq`. `dropped`, `resync`, and delayed `values` notifications are outside `stream_seq` (`values` carries `{id, watch_id, run_id}`). A later complete variable snapshot includes the current availability; a delta replaces only its named peers.

The fields of `dropped` (the numbers in `ws-slow-client.json` are examples). A screen shows "N lines dropped here" at that position in that source's log.

| Field | Contents |
|---|---|
| `log` | Number of log lines per source. The counts of `log_dropped` events that were not sent are added to their source |
| `at_least` | Sources whose count is only a lower bound (more may have been dropped) |
| `events` | Number of other events |
| `reason` | `not-kept`: the server no longer keeps them (before the replay). `slow-client`: the screen receives too slowly, and the server dropped them from what it was to send |

Source names are the same as in `log` of the summary (sources from the 65th on are `*`).

### Slow connections

For a screen that receives slowly, the server does the following, in order:

1. Drops log lines, `log_dropped` and unknown events (`dropped`).
2. Collapses `time` and `state` events into the last one of each, and drops `heartbeat` without counting.
3. Only if it still cannot catch up: `resync`.

`values` is built from the latest values when it is sent (the queue has room for one per watch; values are not stored while they cannot be sent). `variables`, `operation` and `availability` are never dropped. An ECU that switches the availability of its variables in every step sends at most one `availability` notification per short time (about 20 a second), so a screen that cannot read even those reaches `resync`.

### The `code` of `error` messages

Notifications from the server.

| `code` | Meaning |
|---|---|
| `unauthorized` | Authentication error |
| `invalid-message` | What the screen sent is not a JSON object with a `type` |
| `invalid-watch`, `too-many-watches` | The shape or range of a `watch` is wrong; the connection's watches exceed the limit |
| `launcher-output` | The launcher wrote a line that is not an event |
| `bad-event` | A field of a known event has the wrong type, or the server could not relay the event |
| `line-too-long` | A line exceeds the limit (16 MiB). It is discarded |
| `seq-gap`, `seq-order` | `seq` skipped, or went back |
| `events-format` | `vehicle_sil_events` in `hello` differs from the version this server reads |
| `launcher-stderr` | A line the launcher wrote to stderr |
| `kept-limit` | The amount kept exceeded its limit, so the summary holds only part |
| `no-result` | The launcher exited without writing `result` (`detail.exit_code`) |
| `output-left-open` | The launcher exited, but a process it started keeps its output open, so the server stopped reading the rest |
| `stop-not-delivered` | The stop command could not be written to the launcher's stdin |
| `stop-escalated` | During server shutdown, the configured stop timeout expired or a second signal interrupted the wait, so the server asked the launcher to stop with a signal |
| `launcher-silent` | The launcher process is running but has written no line for longer than `--silent-after` (default 5 seconds) before its result |

- `unauthorized`: `run_id` is `null`. The connection is then closed with 4401.
- `invalid-message`: `run_id` is `null`. Sent only to that connection; the connection stays open.
- `invalid-watch`, `too-many-watches`: `run_id` is `null` and `detail.id` holds the watch id. Sent only to that connection; the connection stays open.
- `launcher-output`: a line that is not a JSON object, has no `event`, or whose `seq` is not an integer from 0 to 2^53. Lines that contain a non-finite number such as `1e999` also go here.
- `bad-event`: includes a `t_ms` that is not an integer from 0 to 2^53, a `count` or `sources` value of `log_dropped` that is not a number, a `rev` of `state` that is not an integer from 0 to 2^53 or an `allowed` that is not an object, and an `id` of `command` that is neither `null` nor an integer from 0 to 2^53. The event is discarded (neither relayed nor applied to the summary).
- `launcher-stderr`: the first 20 lines are sent at once. After that, only the last 30 lines are sent, when the run ends. If lines were dropped in between, one message comes before them (`detail.skipped`: the number of dropped lines).
- `kept-limit`: `detail.what` is `check`, `result`, `findings`, `participants`, `state`, or the kind of an event that exceeded 4 KiB (`phase`, `time`, `registry`, `command`, `participant`). An event that exceeds 4 KiB keeps its required fields ("Limits" below), gets as many of the other fields as fit, and lists the names of the fields that did not fit in `omitted_fields`. Notified once per kind.
- `launcher-silent`: the launcher writes a `heartbeat` every second, so it may be stuck. Sent once each time it falls silent (`detail.seconds`). While the silence lasts, `launcher.silent_s` in the summary grows; it returns to `null` when the launcher writes again.

## Summary (`session`)

| Field | Contents |
|---|---|
| `id` | The run identifier (an opaque string formatted as `"<instance>-<n>"`, one per run started on the server; compare for equality only) |
| `run_t0_ms` | Baseline launcher `t_ms` at the start of the run (`0` on cold runs) |
| `file`, `disable`, `manual`, `speed`, `start_paused` | What was requested at the start |
| `started_at`, `ended_at` | Times (UTC, ISO 8601). `ended_at` is `null` until the run ends |
| `state` | `running` or `ended` |
| `launcher` | `{pid, running, exit_code, signal, silent_s, mode, process_alive}` |
| `stop` | The stop the server asked the launcher for: `none`, `requested` (stop in order), `forced` (stop without waiting) |
| `phase` | The phase of the last `phase` event (`null` if none yet) |
| `last_seq`, `last_t_ms` | `seq` and `t_ms` of the last event received |
| `time` | The last `time` event (as is; `null` if none) |
| `check` | `check` of the `check` event (the whole check result; `null` if none yet) |
| `stop_at_ms` | The checked stop time when the full check exceeded the retention limit or its event line was skipped for size. If the line is skipped, the value comes from the check of the same request before launch. `null` means no stop time. Omitted when the check fits or the value is unknown |
| `registry` | The last `registry` event without `event`, `seq` and `t_ms` (`null` if none) |
| `participants` | The current state of each participant (in the order they first appeared) |
| `participants_omitted` | Number of participant events not kept because the limit (256 participants) was exceeded |
| `findings`, `findings_omitted` | `finding` events (without `event`, `seq` and `t_ms`), and the number not kept because the limit was exceeded |
| `last_command` | The last `command` event without `event` and `t_ms` (`null` if none) |
| `launcher_state` | The last `state` event of the launcher without `event` and `t_ms` (what the launcher accepts now; `null` if none yet). Only the last is kept |
| `allowed` | What this server accepts now: `{start, stop, force, save}` ("What the server accepts (`allowed`)" below) |
| `result` | `result` of the `result` event (`null` if none) |
| `variables` | The state of the ECU variable feature, the variables of each participant, recent operations ("ECU variables" below) |
| `log` | `{kept, dropped}` per source: the number of lines the server keeps and the number it dropped |
| `problems`, `problems_omitted` | The server's notifications (`{code, message, detail}`, as in `error`) in the order they happened, and the number not kept because the limit was exceeded |
| `scenario` | The progress of the scenario run (`scenario` of `api/run`). `null` for a run without a scenario ("Scenarios" below) |
| `intent` | Optional run options used to start this session (such as `variant`, `scenario`, `case`, `after_scenario`, `record`, `record_after_fail_ms`, `params`, `disable`, `enable`, `manual`, `strict`, `connect`, `time_policy`, `speed`, `start_paused`, `stop_at_ms`, `late_tolerance_ms`), with `scenario` preserved as the GUI API root-relative path so clients can re-run via `POST api/run` with `{"file": session.file, ...session.intent}`. Omitted or `null` on older servers |

Notes on the fields:

- **What was requested at the start**: `manual` holds only what `api/run` requested (`manual: true` in the configuration file is in `check`). A `speed` of `null` means no speed was requested; the current speed is `launcher_state.speed`.
- **`state`**: `ended` means the run has finished and its output has been read to the end (or cut off with `output-left-open`). In one-shot (`cold`) mode, `api/session` may show `launcher.running: false` together with `state: running` for up to 2 seconds between process exit and output end; in persistent (`hot`) mode, `ended` arrives with `launcher.running: false` while `launcher.process_alive` stays `true` when the launcher remains ready for the next run.
- **`launcher`**: `running` tells whether the run is still active on its launcher (decides whether the run can be stopped; turns `false` when the run ends, even when a persistent launcher process stays alive for the next run). `mode` is `"hot"` for a persistent `serve` launcher or `"cold"` for a one-shot launcher, and `process_alive` tells whether the underlying launcher process is still alive (older servers omit `mode` and `process_alive`). When the launcher exits, `exit_code` is set (if it ended by a signal, `null`, with the signal number in `signal`; POSIX only). `silent_s` is the number of seconds a running launcher has written nothing (updated only past `--silent-after`, at each multiple of it; `null` once it writes).
- **`time`**: virtual time `sim_ms`, actual speed `actual_speed`, and the lateness figures `late_steps`, `max_late_ms`, `slipped_ms` and `late_ms`. A screen opened midway learns the current time from this; whether the run is paused, where, and the speed set, are in `launcher_state` (`paused`, `paused_at_ms`, `speed`). While the run is paused, `time` can be from before the pause (an earlier `sim_ms` for up to one look of the launcher, and the `actual_speed` and `late_ms` from before it).
- **`check`**: includes `required`, `enabled` and `variables` of each participant.
- **`last_command`**: `seq`, `command`, `source`, `accepted`, `message`, `reason`, `id`, `ms` and `at_ms` of an accepted `step`, and `at_ms` of an accepted `run_until` (`event`, `t_ms`, `run` and `asked_run` are stripped from `last_command`, though `asked_run` appears on launcher-level refused `command` events over WebSocket). `seq` tells a new answer from the same answer as before. `last_command` keeps only the latest single command and overwrites earlier answers under concurrent commands. A launcher before `state` writes no `reason` and `id`.
- **`launcher_state`**: the launcher's `state` event as it wrote it (see "`state`" in [docs/simulation-events.md](../../docs/simulation-events.md)) with its `seq`: `rev`, `phase`, `paused`, `paused_at_ms` (where it is paused), `speed`, `stop`, `scenario`, `handed_over` (boolean, `false` until a scenario passes and hands over to manual control), `scenario_over_at_ms` (virtual time in ms at handover, or `null`), `allowed` (what the launcher accepts: `pause`, `resume`, `step`, `speed`, `run_until`, `stop`, `write`, each `{ok, reason, message, caution}`) and so on. The server does not change it. `rev` goes up with each change, so a screen tells a newer one from the same one sent again. It is the answer of the launcher at the moment it wrote it: `ok` says that the launcher would accept the command, not that it would change anything. Events after `state` (a `command`, a `time`) may be newer. The `state` event also comes as an `event` message; the summary holds only the last, so a screen opened later reads it from the summary and the earlier ones are not replayed. A launcher without the feature `state` writes none, and the field is `null`.
- **`omitted_fields`**: `time`, `registry`, `last_command` and each element of `participants` have an array `omitted_fields` with the names of the fields that were too large to keep. For a participant, a name disappears when a later event brings that field (example: `ws-large-fields.json`).
- **`variables`**: present in the `hello` summary and in HTTP responses. **The summary in a `session` notification does not have this field.** It is large and rarely changes, so it comes in a `variables` notification when it changes. On a `session` notification for the same run, the client keeps using its local value. Right after a `session` notification for another run, that run's `variables` notification arrives.
- **`log`**: dropped lines are the lines the server dropped instead of keeping, and the lines the launcher could not write because its reader was slow (the latter are `sources` of `log_dropped`). A `log_dropped` event is kept at the position where it arrived in the log of each source it names. A screen that reconnects receives it at the same position, and each event is sent only once. The screen shows "N lines dropped here" there.

Each element of `participants` merges, per name, the fields of the `participant` events (without `event`, `seq` and `t_ms`): `name`, `state`, `pid`, `exit_code`, `signal` and others.

- The `reason` of `failed` remains even if `ended` comes later.
- The launcher's `control_link` events (the state of the connection of the API that reads and writes variables) are merged into that participant's `control_link` field, as an object of the event's fields without `name` (such as `{"state", "variables"}`; `state` keeps the launcher's values) (example: `ws-control-trouble.json`).
- `required` and `enabled` are in `check`.
- When a `stop_requested` event arrives for a participant, the server sets `stop_requested: true` on it and keeps it in later states. This tells an exit after a stop request from a failure.

If the launcher exits without writing `result`, the summary has `launcher.running: false` with `exit_code`, `state: ended`, and the notification `no-result`.

## What the server accepts (`allowed`)

`allowed` is `{start, stop, force, save}`. Each of the four is `{ok, reason, message}`, and `save` also has `caution`: `ok` says whether the server would take the request now, `reason` is a code (`null` when `ok`), and `message` is one English sentence for a person (`null` when `ok`). As with error `code` in "Versioning and compatibility", `message` wording is not stable and may change; programs branch on `reason` (and `caution.code`). It is in `session` (the summary), in the `allowed` message, and, so that it is also there when there is no run, at the top of `hello` and of the answer of `api/session`. A screen can enable a button from `ok` and show `message`, without a table of its own. `api/run` that is sent anyway answers with the same code as the error of `start` (409 `already-running` and `starting`, 503 `shutting-down` and `launcher-unusable`). `api/stop-session` answers 200 `{instance, run_id}` or 409 `session-changed`. `POST api/config` sent anyway answers 403 `save-disabled` or 503 `shutting-down`.

| Field | `reason` | When |
|---|---|---|
| `start` | `shutting-down` | The server is ending |
| | `already-running` | A run is running (its output is not yet read to the end), including the run that has just been registered while its `api/run` has not answered |
| | `starting` | A run is starting (`api/run` has not answered) and no run is running |
| | `launcher-unusable` | The launcher that was found cannot be used (`launcher.problems` of `api/app`). It is as the last probe found it |
| `stop` | `shutting-down` | The server is ending, and stops the run itself |
| | `starting` | A run is starting (`api/run` has not answered); `api/stop-session` rejects requests with `409 session-changed` until the new run is identified |
| | `not-running` | There is no run that has a launcher running |
| | `already-stopping` | A stop was already asked for (`stop` is `requested` or `forced`) |
| `force` | `shutting-down`, `starting`, `not-running` | As for `stop` |
| | `already-forced` | The stop was already forced |
| `save` | `shutting-down` | The server is ending |
| | `save-disabled` | The server was started with `--no-save` |

- `caution` of `save` is `null`, or `{code, message}` when the server saves but there is something to know first (the same shape as `caution` of the launcher's `allowed`). Its one code is `command-changes-allowed`: the server was started with `--allow-command-changes`, so it saves an edit that changes the commands that run, and new files. A screen shows the message near its save button. Whether one edit is saved is decided per request (`POST api/config`), not in `allowed`.

- Only the first reason that holds is given, in the order of the table. While a run is starting, `stop` and `force` have `ok: false` with `reason: "starting"`: wait until `api/run` returns `{instance, run_id}` before calling `api/stop-session` ("Stop (`api/stop-session`)").
- What the launcher accepts (pause, step, speed, writes) is not here: it is `allowed` of `launcher_state`, as the launcher wrote it. `ok` there means the launcher would accept the command, not that it would change anything (a `pause` is `ok` while paused).
- **When it is sent**: `allowed` goes out with each `session` message, as it is at that moment. When it changes and no `session` message follows to carry that change, the server sends the `allowed` message instead: a start begins (`starting`), a start fails and `start` is `ok` again, and the server begins to end (`shutting-down`). It does not send the `allowed` message when a `session` message carries the change (a run is registered, a stop is requested, the run ends). The `allowed` message is small, carries `stream_seq`, and is never dropped for a slow connection. A reader that does not know the message type ignores it (see "Versioning and compatibility"), and then still has `allowed` from `hello`, `api/session` and each summary.
- With no run there is no summary, and `allowed` of `hello`, `api/session` and the `allowed` message is all there is.
- A new code may be added; the meaning of a code does not change.

## ECU variables

When the launcher has a Control Server (`variables.available` in `api/app`), the GUI server connects to it for each run and reads and writes the variables the ECUs declared. With a launcher without one, a GUI server without gRPC, or `--no-variables`, `variables.state` is `off` and `reason` gives the reason. Examples: `ws-variables.json` (list, watch, write before the start, stepping, end) and `ws-variables-failed.json` (cannot connect).

### State and list (`variables` in the summary, the `variables` notification)

`{"state", "reason", "peers", "peers_omitted", "variables_omitted", "operations", "operations_from_seq", "operations_open_omitted"}`

| Field | Contents |
|---|---|
| `state` | The state of the variable feature (table below). Values may be added |
| `reason` | The reason for `off`, `connecting` and `failed` (human-readable text, not localized; `null` if none) |
| `peers` | `[{"name", "connected", "variables": [...]}]`. The participants the Control Server knows (also those without variables) |
| `peers[].variables[]` | `{"name", "type", "role", "writable", "builtin", "unit", "min", "max", "step", "description", "enum", "availability"}` |
| `peers_omitted`, `variables_omitted` | Numbers of participants and variables left out because a limit was exceeded |
| `operations` | Operations (`operation` below without `type`, `stream_seq` and `run_id`; in the order the server first saw them) |
| `operations_from_seq` | Every operation with this number (`seq`) or higher that the server receives is in `operations` (0 means every operation the server saw). The operations of a scenario's own writes never reach the server (see `operation` below) |
| `operations_open_omitted` | Number of open operations the server could not keep (normally 0) |

| `state` | Meaning |
|---|---|
| `off` | This run has no variables |
| `connecting` | Trying to connect to the Control Server (also while reconnecting after a disconnection) |
| `ready` | Variables can be read and written |
| `failed` | Could not connect, or was refused |
| `ended` | The run ended. The last values read remain |

- For `ended`, `reason` is set only if the connection dropped before the run ended (the values may be older than the end).
- `peers[].connected` tells whether that ECU is connected now.
- `type` is `bool`, `integer`, `real` or `enum`.
- `role` is `input` (writable during the run), `parameter` (writable only before the first step) or `output` (read-only). `writable` means `role` is not `output`.
- `builtin` is `true` for variables vehicle-sil adds (`ecu.connected`, `peer.*`, `j1939.*`, `can.*`, `lin.*`, `eth.*`), and `false` for variables the ECU application declared (matching `participants[].variables[].builtin` in the check result).
- `min`, `max` and `step` (a hint for the editing increment) are numbers or `null`.
- `enum` is `[{"name", "value"}]` when `type` is `enum`, otherwise `null`.
- `availability` is whether the ECU uses the variable now: `unknown`, `active` or `inactive` ("Availability" below).
- `operations` holds every open operation the server keeps and the last 32 operations. It serves screens opened midway and screens that reconnect.
- The list changes each time a participant connects, so `variables` notifications collect changes over a short time and send them together.

A client waiting for the result of an operation tells, from the summary in `hello` (or `api/session`) after reconnecting, what happened:

| In `operations`? | Number | State |
|---|---|---|
| Yes | | That state. If open, an `operation` notification follows later |
| No | `operations_from_seq` or higher | It has not arrived yet |
| No | Below `operations_from_seq` | It completed while disconnected, and its result (the `operation` notification) will not arrive |

The last row holds when `operations_open_omitted` is 0. When it is 1 or more, it is not even known whether the operation completed. The library treats this last case as `no-answer`.

### Availability (`availability` of a variable, the `availability` notification)

An ECU can say while it runs whether it uses each of its variables now, for example once it has read its configuration in its first steps (see "Availability of a variable" in [docs/control-api.md](../../docs/control-api.md#availability-of-a-variable)). A screen can show a variable that is not in use differently, greyed out for example. The availability is information only: a variable is written, read and watched in the same way whatever its availability.

| `availability` | Meaning |
|---|---|
| `unknown` | The ECU has not decided yet (usually in its first steps). Show the variable as usual |
| `active` | In use. Every variable that does not report its availability is `active` |
| `inactive` | Not in use in the present set-up of the ECU |

- `availability` of a variable in the summary is its state now. The summary in `hello` after a reconnection has the states of that moment.
- The `availability` notification is `{"type": "availability", "stream_seq", "run_id", "peers": {"<participant>": {"<variable>": "<state>", ...}}}`. Of each participant whose states changed, it holds the state of every variable that reports its availability, and replaces what came before for that participant. Changes over a short time are sent together, to every connection, whether it watches variables or not.
- It comes after the `variables` notification (or the `hello`) that lists those variables. Participants and variables that a limit leaves out of the summary are not in it.
- A participant that leaves the simulation, and a lost connection to the Control Server, do not change the states: they stay what the ECU said last (`peers[].connected` and `state` tell of the connection). A new process of the participant starts from the states its variables declare.

### Values (`watch`, `unwatch`, `values`)

After authentication, a screen sends:

- `{"type": "watch", "id": <integer from 0 to 2^53>, "variables": "all" or [{"peer", "name"}, ...], "period_ms": <integer>}`: starts receiving the values of variables.
- `{"type": "unwatch", "id": <id>}`: stops (an unknown `id` does nothing).

| `watch` field | Contents |
|---|---|
| `id` | The number of the watch within the connection. Sending the same `id` again replaces it |
| `variables` | `"all"` or the variables by name. Up to `limits.watch_variables` (2000) names |
| `period_ms` | From `limits.watch_period_ms_min` (20) to `limits.watch_period_ms_max` (if omitted, the minimum) |

- Each connection can have up to `limits.watches` (16) watches. Watches beyond that, and watches of the wrong shape, get an `error` (`invalid-watch`, `too-many-watches`); the connection stays open.
- **A message can be up to 256 KiB. If a screen sends a larger message, the server closes the connection without an `error` (close code 1009).** If you list many long names, check the size before sending; if it is too large, ask for `"all"` or split the watch (the library switches to `"all"`). Sending the same message again gets the connection closed again.
- When the connection drops, all its watches are gone (send them again after reconnecting).
- A watch continues across runs. Nothing arrives while there is no run, while the variable state is neither `ready` nor `ended`, or for variables the current run does not have (only for those). Naming a variable the current run does not have is not an error (you can watch before the run).

The server sends `{"type": "values", "id", "watch_id", "run_id", "sim_ms", "paused", "peers": {"<peer>": {"sample_ms", "values": {"<name>": <value>}}}}`.

**When values arrive**

- Once right after the watch, then every `period_ms`, **only when something changed since the last one sent** (the step at which values were read advanced, or `sim_ms` or `paused` changed). While paused, nothing arrives unless a value changes.
- When a run starts, when the variable state or list changes (the `variables` notification), and when a run ends, values are sent without waiting for `period_ms`.
- The `values` of a run arrive after that run's `variables` notification (or `hello`).
- When a run ends, the state becomes `ended` and the last values read arrive once (`paused` is `false`). A watch started after the end also receives them once.

**Shape of the values**

- `sample_ms` is the virtual time of the step in which that participant's values were read.
- Values: bool as a boolean, integers and enums as numbers, reals as numbers; non-finite reals (NaN, infinity) are `null`. Integers above 2^53 may not be exact as JSON numbers.
- A participant whose values have never been read is not in `peers`.
- Values are "the last values", not the full series (in a fast simulation, the values of the steps in between do not arrive). The server reads from the launcher only the values of watched participants, at the requested rate. The server keeps no history; a screen that needs one keeps it.

**`paused` and `sim_ms`**

`paused`, `sim_ms` and the values come from **a single read**. The server reads the values and virtual time of the watched participants with one call to the Control Server (`GetSnapshot`; see "Read time and values together (`snapshot()`)" in [docs/control-api.md](../../docs/control-api.md#read-time-and-values-together-snapshot)) and sets `paused` from the flag in that response (`consistent`).

| `paused` | Simulation | Values | `sim_ms` |
|---|---|---|---|
| `true` | Paused at `sim_ms` | The values at that time. A screen can show them as "the values at the paused time" | The paused time |
| `false` | Running, ended, or advanced during the read | The last values | The virtual time read together with the values |

- When `true`, the values of every step that started before `sim_ms` are included, and no value of a step after `sim_ms` is.
- When `false`, the values are not older than `sim_ms` (they may be newer). If nothing has been read yet, `sim_ms` is the last virtual time the server knows (`null` if unknown).
- While paused before the first step (`start_paused`), `sim_ms` is 0, `paused` is `true`, and `peers` is empty.

Whether the values of a step that starts exactly at `sim_ms` are included is not defined (they may arrive later while paused). If they arrive, another notification comes with the same `sim_ms`, `paused` `true`, and different values. While paused, the server reads again at the longer of the watch's `period_ms` and about 0.25 seconds. For a watch with a long period, this comes correspondingly later. The per-participant `sample_ms` tells whether they are included.

When `paused` becomes `true`, and when it changes back to `false`:

- **Assumption (from the Control Server)**: this holds as long as the ECU sends its values from within its steps. ECUs built with `ecu_main` do. If the assumption breaks, the launcher issues the warning `control-link-late-values`, and values may be missing even when `true`.
- "The values of a step" are the last values the ECU read before that time.
- There is no guarantee for the values of a participant that runs in real time (not on simulation time); `paused` is decided without looking at its values.
- The values of a disconnected participant stay at their last values (see `connected` in `variables`).
- **Read with separate calls**: when the participants of one watch are read with separate calls (because watches with different periods look at the same participant), `paused` is `true` only if every participant was read as paused at the same time and every later read was paused at that time too.
- **When resume or step was sent with `api/time`**: the server stops treating the values read so far as "the values at the paused time", and decides again from what it reads at the watch's next period. A notification arrives at that period if something changed since the last one sent. If the launcher has not yet received the command, the read still shows it paused at the same time, so no `false` notification arrives.
- **When another client asked the Control Server directly to resume or step**: the values at the paused time stay as the last notification until the server learns that time has moved past the paused time or the run has ended, either by its next read or from a time notification (from the Control Server about every 0.1 seconds). Once it learns that, a `false` notification arrives at the watch's next period, even if the values cannot be read.

When reading does not go well:

- **The response does not fit the limit**: if the values of the watched participants do not fit the limit of one Control Server response (4 MiB), the server splits the participants of the call that did not fit into halves and reads them separately (with a warning in the server log). If every call answers that it is paused at the same time, `paused` is still `true`.
- **One participant exceeds the limit**: if the values of one participant alone exceed the limit, the server does not read them (warning in the log; until that participant is replaced by another process or its variables change). `paused` stays `false` for watches that include that participant.
- **Reads keep failing**: if reading values fails 3 times in a row (the Control Server does not answer before its deadline, for example), a `variables` notification arrives with the state still `ready` and the reason in `reason` (the values are the last ones read). When reading works again, `reason` returns to `null`.

### Writes (`api/write`, `operation`)

`POST api/write` `{"run_id": <run id>, "instance": <server instance>, "writes": {"<peer>": {"<name>": <value>}}, "at_ms": <integer>, "late": "reject" | "apply"}` → `{"run_id", "operation"}`

| Field | Contents |
|---|---|
| `run_id` (required) | The id of the run to write to (`id` of the summary) |
| `instance` (optional) | The server `instance` from `api/app`, `hello` or `api/run`; if given and different, returns 409 `session-changed` |
| `writes` | Values that match the variables' types. Up to `limits.writes_per_request` (256) per request |
| `at_ms` | The virtual time to apply at (0 to `limits.write_at_ms_max`). If omitted, the next step |
| `late` | What to do if the step for that time has already run when the write arrives: `reject` (default) or `apply` |

- **`run_id` and `instance`**: if `instance` (when sent) differs from the current server's `instance`, or `run_id` differs from the id of the running run, the server rejects the request with 409 `session-changed` and writes nothing. An empty or non-string `instance` is rejected with 400 `invalid-field`. This keeps a write made while looking at one run or server process from being written after a transition. When rejected, look at the variables of the current run before writing again.
- **Values in `writes`**: `bool` is `true` / `false`; `integer` an integer (a real with an integral value is accepted); `real` a finite number; `enum` the value of an item (a number) or its name (a string). If `min` and `max` exist, the value must be within them (inclusive).
- The server checks the writes against the variable list and rejects any that do not fit with 400 (`invalid-field`, `unknown-variable`, `not-writable`). If even one does not fit, none is sent.
- **`at_ms`**: the write is applied in the first step that starts at or after that time.
- **`late`**: with `reject`, the write is not applied and the result is `late`. With `apply`, it is applied in the next step and `late` in the result is `true`.
- The writes of one request are applied together, per participant, in the same step. Requests are registered with the launcher in the order they arrive, while waiting for each ECU's queue acknowledgement runs in parallel so one unresponsive ECU does not block writes to other ECUs.
- The response returns once every target ECU has left `sent` (`queued` or settled; `WAIT_FOR_QUEUED`). Whether it was applied arrives in an `operation` notification, which may arrive before the response.

Writes while paused:

- Writes made while paused before the first step (`start_paused`) are applied in the step at time 0 (`parameter` can be written too).
- To write after pausing, set `at_ms` later than the paused time. Some ECUs have already run the step at that time (a launcher rule: [docs/control-api.md](../../docs/control-api.md)).
- A write for the paused time itself (or earlier) becomes `queued` first and ends as `late` at the ECU's next step (with `late: "apply"`, it is applied in that step).

`{"type": "operation", "stream_seq", "run_id", "seq", "state", "done", "peers": {"<peer>": {"state", "applied_at_ms", "late", "reason", "applied_writes", "failed_write"}}}`

The per-participant `state`:

| `state` | Final? | Meaning |
|---|---|---|
| `sent` | No | The launcher sent it to the ECU |
| `queued` | No | The ECU received it |
| `disconnected` | No | |
| `applied` | Yes | Applied in the step at `applied_at_ms` |
| `late` | Yes | |
| `rejected` | Yes | The ECU's setter failed |
| `cancelled` | Yes | The simulation ended before it was applied |
| `invalid` | Yes | The ECU refused it: a `parameter` after the first step, for example (the reason is in `reason`) |
| `busy` | Yes | |
| `unknown` | Yes | Unknown whether it was applied |

- `applied_writes` and `failed_write` are the Control Server's `TargetResult` values, passed on as they are: how many of the participant's writes took effect (from the first) and the position (from 0) of the write `reason` is about. The participant's writes are those of the `api/write` request for it, in the order given. `null` when not known or not said (see "When a write is applied" in [docs/control-api.md](../../docs/control-api.md#when-a-write-is-applied)).
- The overall `state`: while in progress, `sent` (some ECU has not received it yet) or `queued`. Once final (`done` is `true`), it is the participants' state if they all agree, otherwise `mixed`. A final operation does not change.
- Each time the state changes, the server sends it to every connection (operations that other clients wrote directly to the Control Server arrive too). The operations of a scenario's own writes do not arrive: the GUI server does not receive them from the Control Server.
- `operation` is never dropped, even for a slow connection. But if that connection's queue overflows and a `resync` (sending everything again from `hello`) happens, only the operations in `operations` of the summary (the open ones and the last 32) arrive again. Results of operations that completed before that do not reach that connection (`operations_from_seq` of the summary tells which).
- An operation that has not completed by the end of the run is completed by the server as `unknown` (`reason`: `the run ended before the result came`) and sent.
- When there are more than 256 open operations and the server could not keep the old ones (`operations_open_omitted` in the summary), a change to such an operation is still sent as `operation` if it arrives. If the run ends without the operation completing, `unknown` is sent at the end (with empty `peers`).
- A write that the launcher received at the moment the run ended gets its number in the response and is completed and sent as `unknown`.
- Operations that completed while the connection between the GUI server and the Control Server was down, and changes the Control Server could not deliver to the GUI server, are read again and sent on reconnection (or when the server learns that delivery failed).

## Scenarios

The API for using the launcher's `run --scenario` and `check --scenario` ([docs/scenario.md](../../docs/scenario.md)) from a screen. The launcher needs the feature `request` (as for every request of this server). Examples: `scenarios.json`, `check-scenario.json`, `ws-scenario.json`, `error-scenario-not-listed.json`, `error-scenario-running.json`.

### Scenario list (`api/scenarios`)

`GET api/scenarios?file=<configuration file>` returns the `.yaml` and `.yml` files in the directory `scenarios` next to the configuration file (sorted by name; up to 200). `file` goes through the same checks as for `api/check`.

| Field | Contents |
|---|---|
| `directory` | That directory (relative to the root) |
| `found` | Whether the directory exists |
| `scenarios` | `[{path, name, size}]` (`path` relative to the root) |

- Not listed: names that start with `.`, subdirectories, anything that is not a regular file (including symbolic links), names that contain `:` or `\`, names longer than 255 characters.
- If `scenarios` or a directory above it (under the root) is a symbolic link, `found` is `false`.

### `scenario` and `after_scenario` in check and run

If `scenario` of `api/check` and `api/run` is omitted, there is no scenario (`null` gives 400 `invalid-field`).

- The value must be one of the `path`s that `api/scenarios` returns for that configuration file (the server builds the list again to check). Anything else gives 400 `scenario-not-listed`.
- The server writes it into the request file as `scenario`, relative to the directory of the configuration file (the launcher reads a relative `scenario` of a request file from there): `scenarios/throttle.yaml` for `simulation.yaml` and for `sub/simulation.yaml` alike.
- `scenario` in the `api/check` response is the scenario that was passed.
- Optional `after_scenario` (`"stop"` or `"pause"`, default `"stop"` when omitted) chooses what happens when the scenario completes with `passed`. `"pause"` requires `scenario` to be set, `stop_at_ms` to be omitted (400 `invalid-field` otherwise), and `"scenario-handover"` in the launcher's `features` (409 `launcher-unsupported` otherwise). When `"pause"` is used and the scenario passes, the launcher emits `state` with `handed_over: true` and `scenario_over_at_ms`, pauses at that virtual time on the same run (even at `speed: "max"`) until `step` or `resume`, and lifts the `scenario-running` refusal (`launcher_state.allowed.write.ok: true`). Failed, stopped, `stop_at_ms`, or incomplete runs never hand over (`handed_over` stays `false`).
- Optional `record_after_fail_ms` (integer `1`–`600000`, omitted = off) keeps advancing virtual time and recording sampled signals on the timeline for the requested duration after a normal scenario assertion or wait-timeout failure without sending further scenario writes; requires `"record-after-fail"` in the launcher's `features` (409 `launcher-unsupported` otherwise).

What you can do while a scenario runs:

| Action | Handling |
|---|---|
| Start paused (`start_paused: true`) | the launcher refuses it: 422 `check-failed` (`api/run`), a `usage` result (`api/check`) |
| Read variables | Possible if the launcher's `features` have `control-server` |
| `api/write` | Rejected with 409 `scenario-running` until a passed handover (`handed_over: true`) |
| Time commands (`api/time`) and stop | Same as for a normal run |

- With `control-server`, the server adds `--control` (the launcher also rejects writes during replay, and unlocks them after a passed handover). Without it, the server does not add `--control`, the variable state is `off`, and `reason` gives the reason.
- Time commands and stop are written to the launcher's stdin. This is separate from `Pause`, `Step`, `SetSpeed` and the other calls that the Control Server refuses from clients while a scenario runs; the launcher accepts them.

### Scenario events and summary

- `scenario_step` events (`running` when a step starts, and when its result is decided) arrive as the launcher wrote them (one whose `index` is not an integer of 1 or more, or whose `status` is not a string, is a `bad-event`).
- Step findings (`scenario-failed` and others) are `finding`s that have the scenario's `file` and `line`.
- The `simulation` of the result of a run stopped by a scenario is `ended` (the presence of `scenario` in the result tells it from an `ended` caused by a participant).

`scenario` of the summary is `{file, name, case, params, step_count, steps, steps_omitted, status, over_at_ms, timeline_url, handed_over}`. A screen opened midway can build the step table from it.

| Field | Contents |
|---|---|
| `file` | The scenario that was passed |
| `name`, `step_count` | From `scenario` of the check result |
| `case` | Optional selected scenario case name (`null` or omitted when none) |
| `params` | Optional resolved scenario parameters (`[{name, value, from, type?, values?, description?}]`, merging check metadata with result values) |
| `steps` | The steps that have arrived (in `index` order; the last state of each step) |
| `steps_omitted` | Number of steps beyond 1000 |
| `status`, `over_at_ms` | From `scenario` of the result (`null` until the result arrives, or set on handover from `state`) |
| `handed_over` | `true` only when the scenario passed and handed over to manual control; `false` otherwise (normal stop, failure, incomplete, or crash) |
| `timeline_url` | Where the server serves the run's timeline file (`api/timeline/<run_id>`). `null` until the run has ended, and for a run without one (see below) |

- The fields of `steps[]` are the same as in the event. `text` and `message` are cut at 1000 characters.
- Once the result arrives, `steps` becomes the steps of the result (including `not_run`).

### Timeline of a scenario run (`api/timeline`)

When the launcher has `timeline` in its `features`, the server adds `--timeline <file>` to every run with a scenario. The file is in a temporary directory of the server. The run's `result.scenario.timeline` and the launcher's `log` line `timeline: <file>` name it (see "Recording a scenario run on a timeline" in [docs/simulation-run.md](../../docs/simulation-run.md#recording-a-scenario-run-on-a-timeline---timeline)). A launcher without `timeline` gets no `--timeline`, and its runs have no timeline.

- When the run has ended, the server keeps the file if the launcher wrote a whole one: the file is not empty and the result has no `timeline-write` finding. Then the summary's `scenario.timeline_url` is `api/timeline/<run_id>`.
- `GET api/timeline/<run_id>` returns the file as `application/json`, with `Content-Disposition: inline; filename="<scenario name>.timeline.json"`. It needs the token, as every `api/` request does. A run that has no kept file (no scenario, a launcher without `timeline`, a run that is still running or wrote no whole file, an id the server never gave, a file removed) gets 404 `no-timeline`.
- The server keeps the files of the last 4 runs that wrote one. When a fifth is kept, the oldest is removed. All of them are removed when the server stops.
- The page that draws the file is served at `timeline/` without a token (`timeline/`, `timeline/timeline.html`, `timeline/page.js`, and `timeline/vehicle-sil-timeline.js`, the element `<vehicle-sil-timeline>` that screens of the server can load too), with the same `Host` check and headers as the screen's files. It reads the token as the screens do (`#token=` in its URL, else the tab's sessionStorage under the key of `takeToken`) and sends it with `Authorization: Bearer`. Open it as `timeline/?src=/api/timeline/<run_id>`.

## Recording actions (`api/recording`)

For each run, the GUI server keeps the writes made through it (`api/write`) that were applied (`applied` and `applied_at_ms` of each participant's `operation`). This covers writes from every connected screen and from tools that use `api/write`. `GET api/recording` returns them as scenario YAML (for the current run, or for the run that ended last, until the next run starts). **The server writes no file.** The screen hands the YAML to the browser to save, and the browser decides where. Example: `recording.json`.

`{"recording": {"run_id", "file", "state", "available", "reason", "writes", "omitted", "steps", "end_ms", "yaml"}}`

| Field | Contents |
|---|---|
| `available` | Whether this run is recorded (`false` for a run without variables and for a scenario run; `reason` gives the reason) |
| `reason` | Why `available` is `false`, or why `yaml` is `null` (no applied writes yet, for example) |
| `writes` | Number of values in the YAML |
| `steps` | Number of steps (`set`) |
| `omitted` | Number of values left out |
| `end_ms` | Finalized end virtual time (ms) from `result.timing.sim_ms` once the run has ended (`simulation != "not_started"` and valid relative to the last step); `null` while running, on crash, when `simulation == "not_started"`, or when unavailable |
| `yaml` | The scenario YAML (`null` if none) |

- The comments at the top of the YAML give the reasons for values left out: either the limit of 2000 values was exceeded, or the participant name contains `.`. In a scenario, everything up to the first `.` is the participant name, so a participant whose name contains `.` cannot be addressed.
- When `end_ms` is `null` (mid-run, or when no finalized `result.timing.sim_ms` is available), `end_ms` is omitted from the YAML and the top comments note that the scenario ends at its last step (`<last> ms`). Once finalized after the run ends, the YAML includes top-level `end_ms: <end_ms>`, so replaying it runs up to the recorded run's end time without dummy writes or always-true `expect` steps.
- The YAML has `vehicle_sil_scenario: 1`, `name`, optional `end_ms`, and `steps`; each step has only `at_ms` (the virtual time of the step it was applied in) and `set`. Values applied at the same time go into one `set`; for a second write of the same variable, the last value at that time is kept.
- Variables are written `<participant>.<variable name>`; enums as the item name (always quoted; the value if the name is ambiguous).

How the YAML is written:

- A variable name is unquoted only if it starts with a letter or `_`, contains only letters, digits, `_`, `-` and `.`, and contains a `.`; otherwise it is in double quotes.
- Characters other than printable ASCII (newline, tab, BOM, control characters, non-ASCII characters) are escaped as `\uXXXX` (and `\UXXXXXXXX`). `\xNN` is not used, because the launcher's YAML reader reads `\xNN` as one byte, not as the character U+00NN.
- Reals always contain `.` (`1.0`, `1.0e+20`; read as numbers by both YAML 1.1 and 1.2). Non-finite numbers are not written.

Not in the YAML. Add what you need in an editor after saving:

- Time control (pause, step).
- Writes that were not applied (`late`, `rejected` and others).
- Writes made directly to the Control Server, not through the GUI server.
- Expected values (`expect`).
- Which screen or tool made a write (all writes of one run go into one scenario).

## Dashboard files

`GET api/dashboard?file=<configuration file>` reads `<name without extension>.dashboard.yaml` next to the configuration file, checks its shape, and returns it as JSON (for how to write it, see "Dashboard files" in [docs/gui.md](../../docs/gui.md#dashboard-files); the JSON Schema is [dashboard.schema.json](dashboard.schema.json)). It works without a run.

| Field | Contents |
|---|---|
| `path` | The dashboard file (relative to the root) |
| `found` | Whether the file exists. A missing file is not an error (the screen lays out the variables automatically) |
| `errors` | The errors (up to 50). If there is any, `dashboard` is `null` |
| `dashboard` | `{title, panels, sections}` |

- `errors[]`: `line` and `column` count from 1, `at` is the location in the data (`sections[0].items[2].widget`), and `message` is a sentence, not localized, that includes how to fix the problem.
- `panels` is `[{module, url}]`. `url` is relative to the screen's base and is where the module is loaded from (each path segment is percent-encoded).
- `sections` is `[{title, collapsed, items}]`.
- Each element of `items` has one of these shapes. A reader skips items of unknown shape.
  - `{variable: {peer, name}, widget, label, options}` (`widget` and `label` are `null` if omitted)
  - `{panel, label, options, variables: [{peer, name}]}`
  - `{auto}`
- The server does not check whether a variable exists or whether a widget fits it (the screen checks against the current variable list).
- For `widget`, the server checks only the form of the name (starts with a lower-case letter); the screen checks which widgets exist.
- YAML aliases (`*name`), a second document and YAML type tags (`!!python/...` and others) are errors. A file that is a symbolic link out of the root is not read.

| What is read | Limit |
|---|---|
| File size | 256 KiB |
| Nesting | 16 levels |
| Values | 50000 |
| Sections | 200 |
| Items (in total) | 2000 |
| Panel modules | 16 |
| One value (string, number) | 1024 characters |

### Panel modules

`GET panels/<key>/<path relative to the root>` returns the ES module of a custom panel as `text/javascript` (with `X-Content-Type-Options: nosniff`, the same CSP as other responses, and `Cache-Control: no-store`).

- `<key>` is a random value created at each start, and appears only in `url` of `api/dashboard`. A browser cannot add headers to a module import, so the key goes in the URL in place of the token. It is a different value from the token; knowing it lets you read panel modules and nothing else.
- Only regular `.js` / `.mjs` files in a directory named `panels` under the root are returned (up to 1 MiB; a link must also resolve into that `panels`). `panels` may be anywhere under the root, as long as neither `panels` nor a directory above it is a symbolic link.
- Paths with names that start with `.`, or that contain `..`, `\`, `:` or NUL, and requests with a wrong `<key>` get 404.
- The `Host` and `Origin` checks are the same as elsewhere.

## Limits

What the server keeps is limited; whatever exceeds a limit is counted and reported.

### Request bodies and texts

| What | Limit | When exceeded |
|---|---|---|
| The body of a request | 64 KiB; of `api/check` and `POST api/config`, 6 MiB + 64 KiB (room for a text of 1 MiB however JSON escapes it: `\u0001` takes 6 bytes) | 413 `too-large` |
| `text` of `api/check` and of `POST api/config` | 1 MiB as UTF-8 | 413 `text-too-large` |
| The file that `api/config` reads | 1 MiB | 413 `config-too-large` |

### Log limits

| What | Limit | When exceeded |
|---|---|---|
| Log per source | The last 2000 lines and 1 MiB | Drops old lines and counts them |
| Number of sources | 64 | Later sources share `*` |
| Whole log | 16 MiB | |
| One log line | 64 KiB | Not kept (still relayed) |

### Summary limits

| What | Limit | When exceeded |
|---|---|---|
| `check` | 4 MiB | `null` |
| `result` | 4 MiB | Only short, non-nested fields (example: `ws-kept-limit.json`) |
| `findings` | 1000 and 1 MiB | Counted in `findings_omitted` |
| `participants` | 256 participants, 4 KiB each | Counted in `participants_omitted` |
| `phase`, `time`, `registry`, `command` | 4 KiB each | Required fields are kept |
| `launcher_state` | 64 KiB | The one kept before stays, and the server says so once (`kept-limit`) |
| Unknown events | Not kept | Only relayed (screens that reconnect do not receive them) |

Required fields kept even beyond 4 KiB. If these alone exceed 4 KiB, the item is not kept.

| What | Required fields |
|---|---|
| participant | `name`, `state`, `pid`, `exit_code`, `signal`, `reason`, `stop_requested`, `control_link` |
| `phase` | The phase name |
| `time` | `sim_ms`, `late_steps`, `max_late_ms`, `slipped_ms`, `late_ms` |
| `registry` | `uri` |
| `command` | `seq`, `command`, `accepted`, `ms`, `at_ms`, `reason`, `id` |

### `problems`

| What | Limit |
|---|---|
| Written by the launcher (except stderr) | 100 |
| The server's own notifications (`kept-limit`, `no-result`, `output-left-open`, `stop-not-delivered`, `stop-escalated`, `launcher-silent`) | 50 more |
| Stderr | The first 20 lines and the last 30 lines, in addition |

### ECU variable limits

| What | Limit | When exceeded |
|---|---|---|
| Participants in the list | 256 | Counted in `peers_omitted` |
| Variables in the list (in total) | 10000 | Counted in `variables_omitted` |
| Variable name | 256 characters | Not listed |
| Enum items | 256 | Not listed |
| Variables of an unknown type | | Not listed |
| Unit, description, enum item name | 256 characters | Cut |
| Reason text | 500 characters | Cut |
| Values | Only the last set per participant | |
| Availability | Only the last state per variable; changes over 50 ms in one `availability` notification | |
| Operations kept | 256 | The completed ones are forgotten first |
| Operations in the summary | Every open operation kept, and the last 32 | |

### Watches, writes and WebSocket

| What | Limit | When exceeded |
|---|---|---|
| Watches per connection | 16 | |
| Variables named in one watch | 2000 | |
| Watch period | 20 ms to 60 seconds | |
| Values in one `api/write` | 256 | |
| `api/write` requests waiting their turn | 32 | 503 `write-busy` |
| Wait for the Control Server's response | 5 seconds | |
| WebSocket message sent by a screen | 256 KiB | |

### Time spent reading values

- Reading values takes at most half of the time, and reads happen one at a time. If reading is slow, values come later than requested.
- One call reads the participants of all watches whose period has come. Only when a response would not fit the limit and is split does one read issue several calls at once.
- If the Control Server has not sent its current state 10 seconds after connecting, the server reconnects.

## JSON in `examples/`

The JSON files in `examples/` are produced and checked by `gui/server/tests/test_protocol_examples.py` against the Events v4 stand-in launcher (`gui/server/tests/stub_launcher.py`, `serve [--once]`) and the contract validator (`gui/server/tests/fake_runtime.py`), alongside [openapi.yaml](openapi.yaml) and the generated Draft 2020-12 schemas ([schemas/ws-server.schema.json](schemas/ws-server.schema.json), [schemas/ws-client.schema.json](schemas/ws-client.schema.json), [schemas/ws-command-event.schema.json](schemas/ws-command-event.schema.json), and [schemas/cli-json.schema.json](schemas/cli-json.schema.json)). Regenerate and verify byte-for-byte drift offline with `python -m vehicle_sil_gui.openapi --write` and `--check`.

- The files kept are 1–2 per shape, plus those that the documentation and the tests of the screens and the library read. Other cases of the same shape (other error codes, runs that end differently) are checked inside the test, by their error code or how the exchange ends.
- The test also fails if `examples/` holds a file that the test does not produce.
- When you change a shape, rewrite the files with `UPDATE_PROTOCOL_EXAMPLES=1 scripts/verify-gui.py --skip-web`, and update the library's types and parsers (`gui/client/src/protocol/`) and their unit tests (which read the same files) to match.
- Process IDs, times, instances, paths of this computer, and numbers that change with each run are replaced with fixed values (`t_ms` is `seq × 10`, and `"<instance>-<n>"` run IDs are normalized to `"<n>"` in the captured example JSONs).

## See also

| Goal | Document |
|---|---|
| Browse all external API docs, schemas, and examples | [External API and Schemas](../../docs/external-api.md) |
| Build your own screen | [Build your own screen for the browser GUI](../../docs/howto/gui-screen.md) |
| Use the API from TypeScript (connection, reconnection, applying events to the summary, time commands, watching ECU variables, writes and their results) | The client library [gui/client](../client/README.md) |
| See a minimal client without a screen | [examples/gui_client/run.mjs](../../examples/gui_client/run.mjs) in the repository (plain JavaScript, no dependencies, Node 22 or later) |
| See targeted time commands (`id` correlation), reconnect (`after_stream_seq`), and `stop-session` without a library | [examples/gui_client/time-and-stop.mjs](../../examples/gui_client/time-and-stop.mjs) in the repository (plain JavaScript, no dependencies, Node 22 or later) |
| See an example that reads and writes ECU variables | [examples/gui_client/variables.mjs](../../examples/gui_client/variables.mjs) in the repository (uses the library) |

`run.mjs` and `time-and-stop.mjs` are run by `scripts/verify-gui.py` against `stub_launcher.py`. `examples/gui_client/` is not included in the prebuilt tools archive.
