# Browser GUI, protocol, client and screen examples

[日本語](ja/code-map-gui.md)

This is the map for changes in this area. Start at [code-map.md](code-map.md); common test rules are in [code-map-testing.md](code-map-testing.md).

Related specifications and change areas: [gui.md](gui.md) / [howto/gui-screen.md](howto/gui-screen.md) / [Control API, gRPC and launcher Control Server](code-map-control.md) / [Launcher, process, configuration and run](code-map-launcher.md)

## Code and tests by directory

### `gui/server`, `gui/protocol`, `gui/client`, `gui/web`

For an introduction, see [gui.md](gui.md).

| What | Location |
|---|---|
| GUI server | `gui/server/src/vehicle_sil_gui/` (`server.py` HTTP transport, security middleware and `RouteSpec`; `config_service.py` configs/check/save/TOCTOU; `run_manager.py` run lifecycle and `allowed()`; `ws_gateway.py` WebSocket and watches; `contracts.py` Pydantic v2 models; `openapi.py` offline OpenAPI 3.1 and WS/CLI JSON Schema exporter; `control.py` connects to the Control Server) |
| API specification, schemas, and examples | `gui/protocol` (`README.md`, offline-generated `openapi.yaml` for all 17 HTTP endpoints and `schemas/*.schema.json` checked by `python -m vehicle_sil_gui.openapi --check` / `scripts/verify-python.py`, and 60 captured `examples/*.json`) |
| Client library | `gui/client/src` |
| Sample screens | `gui/web/src/app` |
| The client library and the parts served to a screen of your own (`vsil/`) | `gui/server/src/vehicle_sil_gui/client_files.py` and `get_client_file()` in `server.py`; the files are listed again in `CLIENT_BUNDLES` of `scripts/package.py`; test: `gui/server/tests/test_client_files.py` |

- Tests: `gui/server/tests` (pytest), the unit tests of `gui/client/src` and `gui/web/src/app`, `gui/web/e2e` (Playwright)
- `ctest -R`: not in CTest. Run them with `scripts/verify-gui.py` (`-k` narrows the server tests; `--skip-web`) and `scripts/verify-web.py`.

### `examples/gui_client`, `examples/gui_starter`

| What | Location |
|---|---|
| Examples of tools without a screen | `run.mjs`, `run-with-library.mjs` and `variables.mjs` in `examples/gui_client` |
| Template for your own screen | `examples/gui_starter` |

- Tests: `gui/server/tests/test_example_client.py`, `gui/web/e2e/starter.spec.ts`
- `ctest -R`: not in CTest. Run them with `scripts/verify-gui.py`.

### Launcher request and targets

The GUI server does not build the launcher's options for the user's choices. It writes what the screen chose as a request file and passes it with `--request` to `check --json`, or as `{"command":"run","request":"<file>"}` to `vehicle_sil_launch serve` (or `serve --once`); the rules about the choices stay in the launcher's check.

| What | Location |
|---|---|
| The fields of a check, start or time command, their form and size, intent building, and the request file that is written (a new temporary file, mode 0600 on POSIX, removed when the check or run has ended) | `REQUEST_FIELDS`, `RunRequest`, `CheckTextRequest` and `TimeCommandRequest` in `gui/server/src/vehicle_sil_gui/contracts.py`; `ConfigService.build_intent()` in `config_service.py`; `request_text()`, `write_request_file()`, `serve_prepare_command()`, `serve_run_command()` and `stop_command_line()` in `python/vehicle_sil_control/protocol.py` (re-exported by `launcher.py`), plus `check_command()` and `serve_command()` in `launcher.py` |
| File-only metadata, hot-eligibility inspection (`check --file-only --json` and `serve prepare`), and single-parse Events v4 (`vehicle_sil_events: 4`) delivery | `file_only_check_command()`, `serve_prepare_command()` and `hide_request_in_event()` in `launcher.py`; `ServeRuntime.inspect_config()`, `parse_file_only_check()` and `parse_prepare_event()` in `runtime.py`; `Session.feed_event()` in `session.py`; `RunManager.relay_event()` in `run_manager.py` |
| Protocol v4 (`vehicle_sil_gui: 4`) single-stream WebSocket sequencing `(instance, run_id, stream_seq)` (`run_id` is `"<instance>-<n>"`), reconnect delta log replay (`after_stream_seq`), delayed `values` frames `{id, watch_id, run_id}` outside `stream_seq`, pure HTTP command Ack endpoints (`POST /api/run`, `/api/stop-session`, `/api/prepare`, `/api/time`, `/api/write`; stationary `GET /api/session`), C++ `ExecutionStatus` adoption, and pure UI control projection (`allowed.py`) | `ServeRunHandle.send_stop()`, `ServeRuntime.status()` and `ServeRuntime.execution_status()` in `runtime.py`; `Facts`, `RunFacts`, `stop_of()`, `project_run_facts()` and `allowed()` in `allowed.py`; `Session`, `Session.replay()` and `Session.run_facts()` in `session.py`; `Hub` in `hub.py`; `RunManager.run_facts()`, `RunManager.allowed()` and `RunManager._write_stop()` in `run_manager.py`; `WsGateway.snapshot()` in `ws_gateway.py`; `contracts.py` |
| The only rule the server keeps for `connect`: a target other than `none` is passed on only if the last `POST api/connections` of that network of that file listed it as usable (`target-not-listed` otherwise, guarded by file stamp and `file_sha256` TOCTOU) | `ConfigService.validate_connect_targets()`, `file_sha256()` and `ConfigService.connections()` in `gui/server/src/vehicle_sil_gui/config_service.py`; `post_connections()` in `server.py`; `connections_command()` in `launcher.py` |
| The check of a configuration's unsaved text (`text` of `POST api/check`, which starts no process), the descriptions it takes (made by a check of the saved file, per file and variant, in a temporary directory), `POST api/describe` and `GET api/config` | `ConfigService._check_text()`, `ConfigService._text_check_locked()`, `ConfigService._describe_locked()`, `ConfigService.describe()` and `ConfigService.get_config()` in `config_service.py`; `Descriptions` in `descriptions.py`; `text_check_command()` and `describe_command()` in `launcher.py` |
| Saving a text (`POST api/config`): the body, where a new file may be made, the decision from the launcher's `commands` of `check --compare-commands` (the server compares no YAML), the replacement through a file next to it; `--no-save`, `--allow-command-changes` and `allowed.save`; `--no-auth` (server-side HTTP/WS token bypass while keeping `127.0.0.1`, `Host`, `Origin`, CSP and `--root` guards) | `config_save.py` (pure functions and the replacement); `ConfigService.save_config()` and `ConfigService._save_locked()` in `config_service.py`; `post_config()`, `_guard()` and `websocket()` in `server.py`; `WsGateway.handle_ws()` and `WsGateway._talk()` in `ws_gateway.py`; `save_check_command()` in `launcher.py`; `allowed_save()` in `allowed.py`; the options in `cli.py` |
| Tests | `gui/server/tests/test_connect.py`, `test_config_text.py`, `test_config_save.py`, `test_units.py`, `test_services.py`, `test_runs.py`, `test_runtime_inprocess.py`, `test_serve_runtime_inprocess.py`, `test_real_variables.py`, `test_protocol_examples.py` (`gui/server/tests/fake_runtime.py` and `gui/server/tests/stub_launcher.py`) |

Do not copy a launcher rule into the server (which names exist, which fields go together, what a time policy means): add it to the launcher and let the check report it. To add a field, follow "[Request fields and the request file](code-map-launcher.md#request-fields-and-the-request-file)".

## Cross-area changes and development references

- Launcher events, stdin commands, features and format: [events and commands](code-map-launcher.md#events-stdin-commands-version---json). For the stub launcher and protocol examples, see [run traffic](code-map-launcher.md#run-traffic-traffic).
- Speed and time limits: [launcher limits](code-map-launcher.md#launcher-limits) (GUI copies and the test detecting mismatches).
- Control API replies: [GetSnapshot](code-map-control.md#reading-time-and-values-together), [control.proto](code-map-control.md#controlproto), and [launcher Control Server](code-map-control.md#the-launchers-control-server).
- Dashboard schema and template copies: [configuration and schemas](code-map-launcher.md#configuration-file-or-scenario-keys).
- GUI-specific development, tests and the stub launcher / runtime harness: [GUI development](gui.md#development). For the HTTP/WebSocket contract see [gui/protocol/README.md](../gui/protocol/README.md), for the client API [gui/client/README.md](../gui/client/README.md), for screen development [gui/web/README.md](../gui/web/README.md), and for the starter [examples/gui_starter/README.md](../examples/gui_starter/README.md).
