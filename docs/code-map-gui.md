# Browser GUI, protocol, client and screen examples

[日本語](ja/code-map-gui.md)

<!-- Style: write each entry as a short block of the same shape (a subheading, a bullet list with one fact per line, a table whose cells are one sentence each). Do not pack several facts into one cell or one bullet: the line gets long and unrelated changes hit the same line. -->

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

- Tests: `gui/server/tests` (pytest), the unit tests of `gui/client/src` and `gui/web/src/app`, `gui/web/e2e` (Playwright)
- `ctest -R`: not in CTest. Run them with `scripts/verify-gui.py` (`-k` narrows the server tests; `--skip-web`) and `scripts/verify-web.py`.

### `examples/gui_client`, `examples/gui_starter`

| What | Location |
|---|---|
| Examples of tools without a screen | `run.mjs`, `run-with-library.mjs` and `variables.mjs` in `examples/gui_client` |
| Template for your own screen | `examples/gui_starter` |

- Tests: `gui/server/tests/test_example_client.py`, `gui/web/e2e/starter.spec.ts`
- `ctest -R`: not in CTest. Run them with `scripts/verify-gui.py`.
