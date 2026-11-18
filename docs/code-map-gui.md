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

## Cross-area changes and development references

- Launcher events, stdin commands, features and format: [events and commands](code-map-launcher.md#events-stdin-commands-version---json). For recordings and protocol examples, see [run traffic](code-map-launcher.md#run-traffic-traffic).
- Speed and time limits: [launcher limits](code-map-launcher.md#launcher-limits) (GUI copies and the test detecting mismatches).
- Control API replies: [GetSnapshot](code-map-control.md#reading-time-and-values-together), [control.proto](code-map-control.md#controlproto), and [launcher Control Server](code-map-control.md#the-launchers-control-server).
- Dashboard schema and template copies: [configuration and schemas](code-map-launcher.md#configuration-file-or-scenario-keys).
- GUI-specific development, tests and recordings: [GUI development](gui.md#development). For the HTTP/WebSocket contract see [gui/protocol/README.md](../gui/protocol/README.md), for the client API [gui/client/README.md](../gui/client/README.md), for screen development [gui/web/README.md](../gui/web/README.md), and for the starter [examples/gui_starter/README.md](../examples/gui_starter/README.md).
