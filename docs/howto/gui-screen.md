# Build your own screen for the browser GUI

[日本語](../ja/howto/gui-screen.md)

In this guide you copy the screen template (`gui_starter`), rewrite it into a screen that shows one ECU variable, and have the GUI server (`vehicle-sil-gui`) serve it. The screen needs no build and no Node.js; step 4 uses Node.js only to read the value through the API. Each "Verify" section also shows how to verify with `curl`, without a browser.

## Prerequisites

| Requirement | Details |
|---|---|
| Template project | You have done step 1 of [Create your first ECU](first-ecu.md): the ECUs are built and the tools' `bin` is in `PATH`. |
| Python | 3.11 or later. If you have [uv](https://docs.astral.sh/uv/), uv installs Python and the packages. |
| A browser or `curl` | Use a browser on the same computer as the GUI server. |
| Node.js | 22 or later. Needed only to verify the value in step 4 without a browser. |

Run the commands in the template project directory (the one that contains `simulation.yaml`). Set this variable to the directory of the prebuilt tools:

```sh
TOOLS=tools/vehicle-sil-tools-<version>-linux-x86_64   # <version> is what vehicle_sil_launch version prints
```

If you use a repository build, use these replacements:

| | Template project | Repository (run in the root) |
|---|---|---|
| Screen template | `$TOOLS/gui_starter/` | `examples/gui_starter/` |
| Library file (`vehicle-sil-gui-client.mjs`) | Included in `gui_starter/` | Build it with `(cd gui/web && npm ci && npm run build:client)`, then copy it from `build/gui-client/` to the screen directory. |
| Start command | `vehicle-sil-gui --web my-screen simulation.live.yaml` | `uv run vehicle-sil-gui --launcher build/<preset>/tools/launch/vehicle_sil_launch --web my-screen build/<preset>/examples/ecu_main/simulation.live.yaml` |
| Launcher and ECUs | `vehicle_sil_launch`, `bin/engine_ecu` | `build/<preset>/tools/launch/vehicle_sil_launch`, the executables in `build/<preset>/examples/ecu_main/` |

## Overview

| Step | What you do | What you get |
|---|---|---|
| 1 | Copy the screen template and serve it as is | `my-screen/` and the template screen |
| 2 | Find the name of the variable to show | The participant name `Engine` and the variable name `engine.speed` |
| 3 | Rewrite the screen to show one variable | `my-screen/index.html` and `my-screen/main.js` |
| 4 | Serve the screen and verify that the value appears | A screen that shows the engine speed |

## Step 1: Copy the screen template and serve it

Copy the screen template to `my-screen/`, then start the GUI server and pass that directory with `--web`.

```sh
cp -r "$TOOLS/gui_starter" my-screen
vehicle-sil-gui --web my-screen simulation.live.yaml
```

`my-screen/` contains these files:

| File | Contents |
|---|---|
| `index.html` | The screen |
| `main.js` | The script that calls the library |
| `style.css` | The styles |
| `vehicle-sil-gui-client.mjs` | The client library, bundled into one file |
| `README.md` | A description of the template (not served) |

### Verify the template screen

The terminal prints a URL, and the browser opens a screen titled "My vehicle-sil screen". To start the server without opening a browser, add `--no-browser`. Click **Start**: **Phase** changes to `running`.

```text
http://127.0.0.1:36917/#token=<a token that changes at every start>
vehicle-sil-gui: open the URL above (it has the token); Ctrl+C stops the server
```

Without a browser, read the part of the printed URL before `#` with `curl` from another terminal. The port changes at every start, so take it from the printed URL (for a fixed port, use `--port`). The screen files can be read without the token.

```sh
URL='<the URL that the server printed>'
BASE="${URL%%/#*}"
curl -s "$BASE/" | grep '<title>'
```

```text
    <title>My vehicle-sil screen</title>
```

When you are done, stop the GUI server with Ctrl+C. From another terminal, `kill <PID>` (SIGTERM) also stops it.

### Troubleshoot serving the template

| Symptom | Cause | Fix |
|---|---|---|
| `there is no index.html in it`, and the exit code is 2 | The directory you passed has no `index.html`. | Pass a directory that contains `index.html`. |
| `it has package.json, so it is the project and not its build`, and the exit code is 2 | You passed the project directory, not its build output. | Pass the build output directory (such as `dist/`). |
| The screen shows `No token: open the URL that vehicle-sil-gui printed (with #token=...).` | You opened a URL without `#token=`. | Open the URL that the terminal printed, including everything after `#token=`. |

## Step 2: Find the name of the variable to show

A variable is identified by a participant name and a variable name. Find the names with one of these methods:

| Method | Best for |
|---|---|
| `participants[].variables[]` in the output of `vehicle_sil_launch check --json <configuration file>` | Looking up the variables of all ECUs in the configuration file before a run |
| `variables[]` in the `--describe` output of the ECU executable (`bin/engine_ecu --describe`) | Looking up one ECU |
| `session.variables.peers[].variables[]` of `GET api/session` during a run | Including the monitoring variables that are added during the run |

This guide uses the first method to list the variables that the ECUs declare (those with `builtin` false):

```sh
vehicle_sil_launch check --json simulation.live.yaml | python3 -c "
import json, sys
for p in json.load(sys.stdin)['participants']:
    for v in p['variables']:
        if not v['builtin']:
            print(p['name'], v['name'], v['type'], v['causality'], v['unit'], v['description'], sep=' | ')
"
```

### Verify the variable name

Each line shows the participant name, the variable name, the type, the direction (input or output), the unit and the description. The ECU author writes the description, and it can be empty. If the name and the description do not tell you which variable you need, ask the ECU author.

```text
Engine | engine.throttle | real | input | % | accelerator pedal position
Engine | engine.speed | integer | output | rpm |
Dashboard | dashboard.engine_speed | integer | output | rpm | the speed shown
Dashboard | dashboard.engine_lost | bool | output |  | no engine speed for more than 300 ms (it comes every 100 ms)
```

This guide shows `engine.speed` (the engine speed) of `Engine`. `output` variables are read-only; `input` variables can be written. Pass the variable to the library as `{ peer: 'Engine', name: 'engine.speed' }`. In scenarios and dashboard files, write it as `Engine.engine.speed`.

### Troubleshoot the variable lookup

| Symptom | Cause | Fix |
|---|---|---|
| `vehicle_sil_launch` is not found | The tools' `bin` is not in `PATH`. | Run the `export PATH=...` line that `get_tools.py` printed. |

## Step 3: Rewrite the screen to show one variable

Replace `my-screen/index.html` with the following. The page loads its script from a file, because the CSP that the GUI server adds blocks inline `<script>`.

```html
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <title>Engine speed</title>
    <script type="module" src="main.js"></script>
  </head>
  <body>
    <h1>Engine speed</h1>
    <p>Connection: <output id="connection">-</output></p>
    <button type="button" id="start">Start</button>
    <button type="button" id="stop">Stop</button>
    <p>Engine.engine.speed: <output id="value">-</output> rpm (<output id="condition">-</output>)</p>
    <p id="error" role="alert"></p>
  </body>
</html>
```

Replace `my-screen/main.js` with the following:

```js
import { createClient, errorMessage, takeToken } from './vehicle-sil-gui-client.mjs';

const CONFIG = 'simulation.live.yaml'; // relative to the directory of the configuration file given at startup (--root)
const SPEED = { peer: 'Engine', name: 'engine.speed' }; // the participant name and the variable name

const show = (id, text) => (document.getElementById(id).textContent = text);

// Read #token= from the URL that the server printed, and remove it from the URL
const token = takeToken(window);
if (token === null) {
  show('connection', 'Open this page from the URL that vehicle-sil-gui printed (with #token=...)');
  throw new Error('no token');
}
const client = createClient({ baseUrl: new URL('/', location.href).href, token });
client.connection.state.subscribe((state) => show('connection', state));

// Show the value of one variable and the condition of the value (live, paused, stale, ended and others)
const speed = client.variables.bind(SPEED, {});
const draw = ({ reading, condition }) => {
  show('value', reading === undefined ? '-' : String(reading.value));
  show('condition', condition.kind);
};
speed.state.subscribe(draw);
draw(speed.state.get());

const fail = (error) => show('error', errorMessage(error));
document.getElementById('start').onclick = () =>
  client.run.start({ file: CONFIG, disable: [], speed: null, start_paused: false }).catch(fail);
document.getElementById('stop').onclick = () => client.run.stop().catch(fail);

await client.loadApp();
client.connect();
```

The screen no longer uses `style.css` and `README.md`, so you can delete them. What the script does:

| Call | What it does |
|---|---|
| `takeToken(window)` | Reads `#token=` from the URL, removes it from the URL and keeps it in the tab's sessionStorage for reloads. |
| `createClient({ baseUrl, token })` | Creates the client. `baseUrl` is the root of the server. |
| `client.variables.bind(ref, options)` | Combines the value and the condition of one variable into one `state`. |
| `state.subscribe(fn)`, `state.get()` | `subscribe` calls `fn` on every change; `get()` returns the current state. |
| `client.run.start(...)`, `client.run.stop()` | Start and stop a run. |
| `client.loadApp()`, `client.connect()` | Load the launcher's features, then connect over WebSocket. |

`condition.kind` is the condition of the value. Your code compares this string.

| `condition.kind` | Meaning |
|---|---|
| `unavailable` | The variable feature is not available (for example, nothing is running) |
| `missing` | The variable is not part of this run |
| `waiting` | No value yet |
| `live` | A value from a running simulation |
| `paused` | The value at the time the simulation is paused at |
| `stale` | An old value (with a reason) |
| `ended` | The last value of an ended run |

## Step 4: Serve the screen and verify it

```sh
vehicle-sil-gui --web my-screen simulation.live.yaml
```

### Verify your screen

The browser opens the "Engine speed" screen.

| Action | What the screen shows |
|---|---|
| Right after opening | `Connection: open`, `Engine.engine.speed: - rpm (unavailable)` |
| Click **Start** | `Engine.engine.speed: 1200 rpm (live)` |
| Click **Stop** | `Engine.engine.speed: 1200 rpm (ended)` |

Without a browser, verify with `curl` that the files are served. Set `URL` and `BASE` again from the URL printed this time, as in step 1.

```sh
URL='<the URL that the server printed>'
BASE="${URL%%/#*}"
curl -s "$BASE/" | grep '<title>'
curl -s -o /dev/null -w '%{http_code} %{content_type}\n' "$BASE/main.js"
```

```text
    <title>Engine speed</title>
200 text/javascript; charset=utf-8
```

To verify the value itself, call the API. The API needs the token, and a `POST` also needs `Origin`. A token in a command argument is visible to other users of the same computer in the process list, so put the header in a file and pass the file.

```sh
(umask 077; printf 'Authorization: Bearer %s\n' "${URL##*#token=}" > token.header)

# Start a run
curl -s -X POST -H @token.header -H "Origin: $BASE" -H 'Content-Type: application/json' \
  -d '{"file": "simulation.live.yaml", "disable": [], "speed": null, "start_paused": false}' \
  "$BASE/api/run" | head -c 60; echo
```

```text
{"session": {"id": 1, "file": "simulation.live.yaml", "disab
```

Values arrive over WebSocket, so `curl` cannot read them. With Node.js (22 or later), you can read them with the same library as the screen. Save the following as `read-value.mjs` and run it.

```js
// Read the value of a variable once with the same library as your screen, and print it (Node 22 or later).
import { createClient } from './my-screen/vehicle-sil-gui-client.mjs';

const url = new URL(process.env.VEHICLE_SIL_GUI_URL); // the URL that the server printed
const client = createClient({ baseUrl: `${url.origin}/`, token: url.hash.replace('#token=', '') });
await client.loadApp();
client.connect();
const speed = client.variables.bind({ peer: 'Engine', name: 'engine.speed' }, {});
speed.state.subscribe(({ reading, condition }) => {
  if (reading === undefined) return;
  console.log(`Engine.engine.speed = ${reading.value} (${condition.kind})`);
  speed.close();
  client.close();
});
```

```sh
VEHICLE_SIL_GUI_URL="$URL" node read-value.mjs
```

```text
Engine.engine.speed = 1200 (live)
```

When you are done, stop the run, delete the token file, and stop the GUI server with Ctrl+C.

```sh
curl -s -X POST -H @token.header -H "Origin: $BASE" -H 'Content-Type: application/json' \
  -d '{"force": false}' "$BASE/api/stop" > /dev/null
rm token.header
```

### Troubleshoot your screen

| Symptom | Cause | Fix |
|---|---|---|
| The value stays `-` and the condition stays `unavailable` | No run has started. | Click **Start**. |
| The condition is `missing` | The participant name or the variable name is not in this run. | Check the names as in step 2. |
| The screen shows nothing, and the browser console shows a CSP error | The page uses inline `<script>` or `onclick="..."`. | Put the script in a file, and register handlers with `addEventListener` or the `onclick` property. |
| A file returns 404 | Its extension is not served, its name starts with `.`, or it is a symbolic link. | See the table in "Files served with `--web`" in [gui.md](../gui.md#files-served-with---web). |
| A `POST` with `curl` is rejected with `forbidden-origin` | The `Origin` header is missing or differs from the server's own origin. | Add `-H "Origin: http://127.0.0.1:<port>"`. |
| The screen does not change after you edit a file | The browser shows the old page. | Reload the page. You do not need to restart the GUI server. |

## What to read next

- Build sliders and switches that write values: "Slider and lamp" in the template's `index.html` and `showSlider()` in its `main.js`; also "Build widgets (`bind`)" in [gui/client/README.md](../../gui/client/README.md#build-widgets-bind)
- Build with TypeScript and a framework (React, Vue and others): "Build with TypeScript and a framework" in [examples/gui_starter/README.md](../../examples/gui_starter/README.md#build-with-typescript-and-a-framework)
- Look up the served files, the CSP, the development server and the rules to follow: "Reference for your own screen" in [gui.md](../gui.md#reference-for-your-own-screen)
