# External API and Schemas

[日本語](ja/external-api.md)

Entry index for external tools, scripts, and custom screens that integrate with `vehicle-sil` via the GUI server API, launcher CLI/events, Control API, or YAML/JSON schemas. Every link on this page is repository-relative so that viewing a checked-out Git tag or release archive always resolves the matching specification, schemas, and examples; each linked document remains the single source of truth.

## GUI Server API (HTTP and WebSocket)

Protocol v4 preserves omitting both `run_id` and `instance` on `POST /api/time` for compatibility, but new clients, AI agents, and CI automation should always send both `instance` and `run_id`.

| Resource | Path | Contents |
|---|---|---|
| Protocol specification | [gui/protocol/README.md](../gui/protocol/README.md) | Authoritative HTTP and WebSocket Protocol v4 (`vehicle_sil_gui: 4`) spec: auth, Origin, run targeting (`instance` + `run_id`), `/api/time`, `/api/stop-session`, `/api/write`, reconnect replay, behavioral ordering invariants, and limits |
| OpenAPI 3.1.0 (all 17 endpoints) | [gui/protocol/openapi.yaml](../gui/protocol/openapi.yaml) | Offline-generated machine-readable contract for all 17 `/api/*` HTTP endpoints (`TimeCommandRequest` with `run_id` ↔ `instance` `dependentRequired`, `TimeCommandAck`, `ErrorResponse`, status codes); also bundled as `protocol/openapi.yaml` in the prebuilt tools archive |
| WebSocket server frames schema | [gui/protocol/schemas/ws-server.schema.json](../gui/protocol/schemas/ws-server.schema.json) | Draft 2020-12 JSON Schema for all 13 server-to-client WebSocket frames (`hello`, `synced`, `session`, `event`, `allowed`, `runtime`, `variables`, `values`, `operation`, `availability`, `dropped`, `resync`, `error`) |
| WebSocket client frames schema | [gui/protocol/schemas/ws-client.schema.json](../gui/protocol/schemas/ws-client.schema.json) | Draft 2020-12 JSON Schema for all 3 client-to-server WebSocket frames (`auth`, `watch`, `unwatch`) |
| WebSocket `command` event schema | [gui/protocol/schemas/ws-command-event.schema.json](../gui/protocol/schemas/ws-command-event.schema.json) | Draft 2020-12 JSON Schema for asynchronous WebSocket `command` events (`accepted`, `reason`, `message`, `id`, `asked_run`) |
| CLI `--json` readiness schema | [gui/protocol/schemas/cli-json.schema.json](../gui/protocol/schemas/cli-json.schema.json) | Draft 2020-12 JSON Schema for the `vehicle-sil-gui --json` stdout readiness line |
| Dashboard file schema | [gui/protocol/dashboard.schema.json](../gui/protocol/dashboard.schema.json) | Draft 2020-12 JSON Schema for `<configuration>.dashboard.yaml` |
| Wire examples | [gui/protocol/examples/](../gui/protocol/examples/) | 60 captured HTTP and WebSocket JSON exchanges covering all 17 HTTP endpoints, validated against the generated schemas |
| TypeScript client library | [gui/client/README.md](../gui/client/README.md) | `@vehicle-sil/gui-client` (`connect()` front door and low-level `createClient()`) |

All GUI Server API schemas (`openapi.yaml` and `schemas/*.schema.json`) are exported deterministically offline without starting a server (`python -m vehicle_sil_gui.openapi --write` / `--check`) and are bundled under `protocol/` in the prebuilt tools archive.

## Configuration, Scenario, and Launcher Schemas

For scenario files, **validate syntax with JSON Schema, and validate against the target configuration with Launcher's `check`** (`vehicle_sil_launch check <config> --scenario <scenario>`, or `POST /api/check`). JSON Schema checks structure and value types only; Launcher `check` verifies participant names, variable names, types, ranges, and time-step constraints against the target ECUs.

| Schema | Path | Validates |
|---|---|---|
| Scenario YAML | [schema/scenario.schema.json](../schema/scenario.schema.json) | Scenario files (`vehicle_sil_scenario: 1`) — syntax only; pair with Launcher `check` |
| Simulation configuration YAML | [schema/simulation.schema.json](../schema/simulation.schema.json) | Simulation configuration files (`vehicle_sil_simulation: 2`) |
| Run request JSON | [schema/run-request.schema.json](../schema/run-request.schema.json) | Launcher `--request` files and `POST /api/check` / `POST /api/run` intent fields (`vehicle_sil_run_request: 1`) |
| Scenario timeline JSON | [schema/timeline.schema.json](../schema/timeline.schema.json) | Output of `--timeline` and `GET /api/timeline/<run_id>` |

## Manuals

| Area | Document |
|---|---|
| Browser GUI guide, security, and custom screens | [gui.md](gui.md), [Build your own screen for the browser GUI](howto/gui-screen.md) |
| Scenarios reference and test guide | [scenario.md](scenario.md), [Write tests with scenarios](howto/scenario-tests.md) |
| Simulation configuration and `check` | [simulation-config.md](simulation-config.md), [simulation-check.md](simulation-check.md) |
| Running simulations (`run`, `test`, `serve`) | [simulation-run.md](simulation-run.md) |
| Events v4 (`serve [--once]`), stdin commands, and `version --json` | [simulation-events.md](simulation-events.md) |
| Control API (gRPC) and Python SDK | [control-api.md](control-api.md) |

## Runnable Examples

| Example | Path | Demonstrates |
|---|---|---|
| Minimal raw HTTP + WebSocket client | [examples/gui_client/run.mjs](../examples/gui_client/run.mjs) | Node 22+ zero-dependency `fetch` + `WebSocket`: `api/app`, `api/check`, `api/run`, event stream |
| Targeted time commands, reconnect, and stop | [examples/gui_client/time-and-stop.mjs](../examples/gui_client/time-and-stop.mjs) | Node 22+ zero-dependency `{instance, run_id}` on `POST api/time` & `POST api/stop-session`, numeric `id` correlation (HTTP 200 vs. WS `accepted: true`/`false`), and WS reconnect with `after_stream_seq` |
| Client library examples | [examples/gui_client/run-with-library.mjs](../examples/gui_client/run-with-library.mjs), [examples/gui_client/front.mjs](../examples/gui_client/front.mjs), [examples/gui_client/variables.mjs](../examples/gui_client/variables.mjs) | Using `@vehicle-sil/gui-client` for run lifecycle, front-door `connect()`, and ECU variable watch/write/step |
| Custom screen starter template | [examples/gui_starter/README.md](../examples/gui_starter/README.md) | Building a custom browser UI served by `vehicle-sil-gui --web` |
| Python resident launcher (`Serve` / `RunHandle`) | [examples/launch_control/serve_runs.py](../examples/launch_control/serve_runs.py) | Running sequential simulations via `vehicle_sil_launch serve --control` |
| Scenario and run-request samples | [examples/ecu_main/scenarios/throttle_step.yaml](../examples/ecu_main/scenarios/throttle_step.yaml), [examples/ecu_main/requests/throttle_step.json](../examples/ecu_main/requests/throttle_step.json) | Complete scenario YAML and run-request JSON paired with `examples/ecu_main` |
| Full examples index | [examples/README.md](../examples/README.md) | All repository examples sorted by task |
