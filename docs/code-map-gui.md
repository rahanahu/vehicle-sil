# Browser GUI, protocol, client and screen examples

[日本語](ja/code-map-gui.md)

This is the map for changes in this area. Start at [code-map.md](code-map.md); common test rules are in [code-map-testing.md](code-map-testing.md).

Related specifications and change areas: [gui.md](gui.md) / [howto/gui-screen.md](howto/gui-screen.md) / [Control API, gRPC and launcher Control Server](code-map-control.md) / [Launcher, process, configuration and run](code-map-launcher.md)

## Code and tests by directory

### `gui/server`, `gui/protocol`, `gui/client`, `gui/web`

For an introduction, see [gui.md](gui.md).

| What | Location |
|---|---|
| GUI server | `gui/server/src/vehicle_sil_gui/` (`control.py` connects to the Control Server) |
| API specification and examples | `gui/protocol` |
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

The GUI server does not build the launcher's options. It writes what the screen chose as a request file and passes it with `--request`; the rules about the choices stay in the launcher's check.

| What | Location |
|---|---|
| The fields of a check or a start, their form and size, and the request file that is written (a new temporary file, mode 0600 on POSIX, removed when the launcher has ended) | `INTENT_FIELDS`, `check_*()`, `write_request_file()`, `check_command()` and `run_command()` in `gui/server/src/vehicle_sil_gui/launcher.py` |
| The only rule the server keeps for `connect`: a target other than `none` is passed on only if the last `POST api/connections` of that network of that file listed it as usable (`target-not-listed` otherwise) | `_check_targets()` and `post_connections()` in `gui/server/src/vehicle_sil_gui/server.py`; `connections_command()` in `launcher.py` |
| The check of a configuration's unsaved text (`text` of `POST api/check`, which starts no process), the descriptions it takes (made by a check of the saved file, per file and variant, in a temporary directory), `POST api/describe` and `GET api/config` | `_check_text()`, `_text_check_locked()`, `_describe_locked()`, `post_describe()` and `get_config()` in `server.py`; `Descriptions` in `descriptions.py`; `text_check_command()` and `describe_command()` in `launcher.py` |
| Tests | `gui/server/tests/test_connect.py`, `test_config_text.py`, `test_units.py`, `test_runs.py`, `test_protocol_examples.py` |

Do not copy a launcher rule into the server (which names exist, which fields go together, what a time policy means): add it to the launcher and let the check report it. To add a field, follow "[Request fields and the request file](code-map-launcher.md#request-fields-and-the-request-file)".

## Cross-area changes and development references

- Launcher events, stdin commands, features and format: [events and commands](code-map-launcher.md#events-stdin-commands-version---json). For recordings and protocol examples, see [run traffic](code-map-launcher.md#run-traffic-traffic).
- Speed and time limits: [launcher limits](code-map-launcher.md#launcher-limits) (GUI copies and the test detecting mismatches).
- Control API replies: [GetSnapshot](code-map-control.md#reading-time-and-values-together), [control.proto](code-map-control.md#controlproto), and [launcher Control Server](code-map-control.md#the-launchers-control-server).
- Dashboard schema and template copies: [configuration and schemas](code-map-launcher.md#configuration-file-or-scenario-keys).
- GUI-specific development, tests and recordings: [GUI development](gui.md#development). For the HTTP/WebSocket contract see [gui/protocol/README.md](../gui/protocol/README.md), for the client API [gui/client/README.md](../gui/client/README.md), for screen development [gui/web/README.md](../gui/web/README.md), and for the starter [examples/gui_starter/README.md](../examples/gui_starter/README.md).
