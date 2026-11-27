# Build your own screen for the browser GUI

[日本語](../ja/howto/gui-screen.md)

In this guide you copy the screen template (`gui_starter`), change the variable names in it to your ECU's so that it shows one ECU variable, and have the GUI server (`vehicle-sil-gui`) serve it. The screen is made of ready-made parts (`<vsil-slider>`, `<vsil-value>` and others), so you write HTML and no script; it needs no build. Each "Verify" section shows how to verify in a browser and, where that is possible, without one (with `curl`; in step 4 also with Node.js). The prerequisites below say what each way needs.

Your own screen replaces the sample screens. To keep them and add a part for your ECU, write a custom panel instead; "Build your own screen" in [gui.md](../gui.md#build-your-own-screen) compares the ways, and the 3 layers of building one (parts only, a script of your own, a project with a framework).

## Prerequisites

| Requirement | Details |
|---|---|
| Template project | You have done step 1 of [Create your first ECU](first-ecu.md): the ECUs are built and the tools' `bin` directory is in `PATH`. |
| Python | 3.11 or later. If you have [uv](https://docs.astral.sh/uv/), uv installs Python and the packages. |
| A browser | To verify on the screen. Use one on the same computer as the GUI server. |
| `curl` | To verify without a browser. |
| Node.js | 22 or later. Only to read a value without a browser (the last part of step 4). |

Run the commands in the template project directory (the one that contains `simulation.yaml`). Set this variable to the directory of the prebuilt tools, and put its `bin` directory in `PATH` (again in every new shell):

```sh
TOOLS=tools/vehicle-sil-tools-<version>-linux-x86_64   # <version>: as in the directory name under tools/
export PATH="$PWD/$TOOLS/bin:$PATH"                    # vehicle-sil-gui and vehicle_sil_launch
```

If you use a repository build, use these replacements:

| | Template project | Repository (run in the root) |
|---|---|---|
| Screen template | `$TOOLS/gui_starter/` | `examples/gui_starter/` |
| Library and parts | The GUI serves them to the screen (`/vsil/`) | Build them once with `(cd gui/web && npm ci && npm run build:client)`; the GUI server serves them from `build/gui-client/`. |
| Start command | `vehicle-sil-gui --web my-screen simulation.live.yaml` | `uv run vehicle-sil-gui --launcher build/<preset>/tools/launch/vehicle_sil_launch --web my-screen build/<preset>/examples/ecu_main/simulation.live.yaml` |
| Launcher and ECUs | `vehicle_sil_launch`, `bin/engine_ecu` | `build/<preset>/tools/launch/vehicle_sil_launch`, the executables in `build/<preset>/examples/ecu_main/` |

## Overview

| Step | What you do | What you get |
|---|---|---|
| 1 | Copy the screen template and serve it as is | `my-screen/` and the template screen |
| 2 | Find the name of the variable to show | The participant name `Engine` and the variable name `engine.speed` |
| 3 | Change the screen to show one variable | `my-screen/index.html` |
| 4 | Serve the screen and verify that the value appears | A screen that shows the engine speed |

## Step 1: Copy the screen template and serve it

Copy the screen template to `my-screen/`, then start the GUI server and pass that directory with `--web`.

```sh
cp -r "$TOOLS/gui_starter" my-screen
vehicle-sil-gui --web my-screen simulation.live.yaml   # opens a browser (add --no-browser to only print the URL); Ctrl+C stops it
```

`my-screen/` contains these files:

| File | Contents |
|---|---|
| `index.html` | The screen |
| `main.js` | A small script of your own for what the parts do not have (a display, a button) |
| `style.css` | The styles for the parts |
| `README.md` | A description of the template (not served) |
| `README.ja.md` | The same description in Japanese (not served) |

`index.html` loads the parts and the library from the GUI server, not from `my-screen/`: `/vsil/vehicle-sil-gui-elements.mjs` (the parts) and `/vsil/vehicle-sil-gui-client.mjs` (the library that the parts and `main.js` import), and `/timeline/vehicle-sil-timeline.js`. If one of them returns 404, see "Files served under `vsil/`" in [gui.md](../gui.md#files-served-under-vsil).

### Verify the template screen

The terminal prints a URL, and the browser opens a screen titled "My vehicle-sil screen". To start the server without opening a browser, add `--no-browser`. Click **Start**: the line at the top of the screen changes to `running` with the virtual time.

```text
http://127.0.0.1:36917/#token=<a token that changes at every start>
vehicle-sil-gui: open the URL above (it has the token); Ctrl+C stops the server
```

To verify without a browser, fetch the part of the printed URL before `#` with `curl` from another terminal. The port changes at every start, so take it from the printed URL (for a fixed port, use `--port`). The server serves the screen files without the token.

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
| One sentence, `missing or wrong token: send Authorization: Bearer <token>, the token in the URL the server printed (#token=...)` | You opened a URL without `#token=` against a server with token authentication enabled (the sentence appears once on the page: in `<vsil-status>`, or in the first part when the page has none; every part carries `data-error`). | Open the URL that the terminal printed, including everything after `#token=` (or start the server with `--no-auth` in a trusted local environment). |

## Step 2: Find the name of the variable to show

A variable is identified by a participant name and a variable name. Find the names with one of these methods:

| Method | Best for |
|---|---|
| `participants[].variables[]` in the output of `vehicle_sil_launch check --json <configuration file>` | Looking up the variables of all ECUs in the configuration file before a run |
| `variables[]` in the `--describe` output of the ECU executable (`bin/engine_ecu --describe`) | Looking up one ECU |
| `session.variables.peers[].variables[]` of `GET api/session` during a run | Looking up variables including the monitoring variables that are added during the run |

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

The list has the variables that the ECUs of the configuration file declare: the 4 of the template here, and those of your own ECUs if you use them. Each line shows the participant name, the variable name, the type, the direction (input or output), the unit and the description. The ECU author writes the description, and it can be empty. If the name and the description do not tell you which variable you need, ask the ECU author.

```text
Engine | engine.throttle | real | input | % | accelerator pedal position
Engine | engine.speed | integer | output | rpm |
Dashboard | dashboard.engine_speed | integer | output | rpm | the speed shown
Dashboard | dashboard.engine_lost | bool | output |  | no engine speed for more than 300 ms (it comes every 100 ms)
```

This guide shows `engine.speed` (the engine speed) of `Engine`. `output` variables are read-only; `input` variables can be written. In the parts, in scenarios and in dashboard files, write it as `Engine.engine.speed` (the participant name, a dot, the variable name; the name is cut at the first dot, so a participant name with a dot needs `{ peer, name }` in a script).

### Troubleshoot the variable lookup

| Symptom | Cause | Fix |
|---|---|---|
| `vehicle_sil_launch` or `vehicle-sil-gui` is not found | The tools' `bin` directory is not in `PATH` (each new shell needs it again). | Run the `TOOLS=...` and `export PATH=...` lines of "[Prerequisites](#prerequisites)". |

## Step 3: Change the screen to show one variable

The template is a page of parts. A part is an HTML element that draws one thing and does its job by itself, so `<vsil-value variable="Engine.engine.speed">` shows that variable with no script. To show your variable, you change the `variable` attribute to the participant name and the variable name from step 2, joined by a dot (`Engine.engine.speed`; the name is cut at the first dot).

To see how little a page needs, replace `my-screen/index.html` with the following. The page loads the parts from the GUI server (`/vsil/`), so there is nothing to copy. It loads them with `src`, because the CSP that the GUI server adds blocks inline `<script>`.

```html
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <title>Engine speed</title>
    <script type="module" src="/vsil/vehicle-sil-gui-elements.mjs"></script>
  </head>
  <body>
    <h1>Engine speed</h1>
    <vsil-status></vsil-status>
    <vsil-run-controls></vsil-run-controls>
    <p>Engine.engine.speed: <vsil-value variable="Engine.engine.speed"></vsil-value></p>
  </body>
</html>
```

The parts of this page:

| Element | What it shows or does |
|---|---|
| `<vsil-status>` | The connection, the run, the virtual time and the one problem to tell you now (such as a refused start) |
| `<vsil-run-controls>` | **Start** and **Stop**. **Start** runs the file of the last run (at first, the configuration file that you gave the server at startup). |
| `<vsil-value variable="...">` | The value with its unit. When it is not a live value, a sentence says why (no run yet, paused, stale, the run has ended). |

Attributes of `<vsil-value>` that this page does not use:

| Attribute | Meaning |
|---|---|
| `on`, `off` | For a bool variable: the words to show for true and false (`on="LOST" off="ok"` shows `LOST` or `ok`). Without them it shows `on` and `off`. |
| `no-unit` | Leave the unit out (`1200` instead of `1200 rpm`). |

All the parts and their attributes are in "Parts" in [gui/client/README.md](../../gui/client/README.md#parts-custom-elements); the template uses most of them (a slider, a button that writes a value, a lamp, time and scenario controls).

The page has no script, so `main.js` is no longer used and you can delete `main.js` and `style.css` (the template has them as the model for "a script of your own" and for styling the parts; see its README). The parts have no look of their own: the template's `style.css` shows how to style what they mark with `data-*` attributes.

Some details of this page:

| What | Details |
|---|---|
| Names | A name that is not in this run does not make an error in the page: the part shows "this variable is not in the current run: check the names (ECU and variable)". |
| One connection | The parts share the connection of the page. A script of your own shares it too: `connect()` in the template's `main.js` returns the same connection. A refused start, stop or time operation shows in `<vsil-status>`. A refused write of a variable shows next to the part that shows that variable (the slider, for example), and in the `ActionError` that your own script receives. |
| Why a button cannot be used | A part that cannot do its job now (**Stop** with no run) has `aria-disabled="true"` and the code of the reason in `data-reason`. The sentence is read by a screen reader; a reason that only says there is nothing to do (no run) is not shown on the screen. |

## Step 4: Serve the screen and verify it

If the server of step 1 is still running, do not start it again: reload the page in the browser (the server reads the files again at each request). To start a server, run:

```sh
vehicle-sil-gui --web my-screen simulation.live.yaml   # opens a browser (add --no-browser to only print the URL); Ctrl+C stops it
```

### Verify on the screen

The browser opens the "Engine speed" screen.

| Action | What the screen shows |
|---|---|
| Right after opening | The top line says `ready`. `Engine.engine.speed: -`, with a sentence that the variables are not available now (no run). |
| Click **Start** | The top line says `running` and the virtual time. `Engine.engine.speed: 1200 rpm` |
| Click **Stop** | The top line says `The run was stopped by a stop request without errors`. `Engine.engine.speed: 1200 rpm` and "the last value of the run that has ended" |

### Verify without a browser (the API and Node.js)

First, verify with `curl` that the files are served. Set `URL` and `BASE` from the URL printed this time, as in step 1.

```sh
URL='<the URL that the server printed>'
BASE="${URL%%/#*}"
curl -s "$BASE/" | grep '<title>'
curl -s -o /dev/null -w '%{http_code} %{content_type}\n' "$BASE/vsil/vehicle-sil-gui-elements.mjs"
```

```text
    <title>Engine speed</title>
200 text/javascript; charset=utf-8
```

Next, call the API. The API needs the token, and a `POST` also needs the `Origin` header. A token in a command argument is visible to other users of the same computer in the process list, so put the header in a file and pass the file. Start a run:

```sh
(umask 077; printf 'Authorization: Bearer %s\n' "${URL##*#token=}" > token.header)

# Start a run (save {instance, run_id} to target stop-session later)
RUN_JSON=$(curl -s -X POST -H @token.header -H "Origin: $BASE" -H 'Content-Type: application/json' \
  -d '{"file": "simulation.live.yaml", "disable": [], "start_paused": false}' \
  "$BASE/api/run")
echo "$RUN_JSON"
```

```text
{"instance": "0123456789abcdef", "run_id": "0123456789abcdef-1"}
```

Values arrive over WebSocket, so `curl` cannot read them. Read the value with Node.js (22 or later) and the same library as the screen.

Copy the library file to the project directory (it is in `$TOOLS/client/` of the tools, and in `build/gui-client/` of the repository):

```sh
cp "$TOOLS/client/vehicle-sil-gui-client.mjs" .
```

Save the following as `read-value.mjs` in the project directory:

```js
// Read the value of a variable once with the same library as your screen, and print it (Node 22 or later).
import { connect, until } from './vehicle-sil-gui-client.mjs';

const url = new URL(process.env.VEHICLE_SIL_GUI_URL); // the URL that the server printed
const client = await connect({ baseUrl: `${url.origin}/`, token: url.hash.replace('#token=', '') });
const speed = client.variable('Engine.engine.speed');
// wait for a live value (`note` is a sentence when the value is not a live one)
const view = await until(speed, (v) => v.value !== null && v.note === null, { timeoutMs: 10_000 });
console.log(`Engine.engine.speed = ${view.text} ${view.unit}`);
client.close();
```

Run it there:

```sh
VEHICLE_SIL_GUI_URL="$URL" node read-value.mjs
```

```text
Engine.engine.speed = 1200 rpm
```

When you are done, stop the run by sending its `instance` and `run_id` to `POST /api/stop-session`, and delete the token file. `"force": false` stops the run in order; `"force": true` stops it without waiting.

```sh
STOP_BODY=$(node -e 'const r = JSON.parse(process.argv[1]); console.log(JSON.stringify({instance: r.instance, run_id: r.run_id, force: false}));' "$RUN_JSON")
curl -s -X POST -H @token.header -H "Origin: $BASE" -H 'Content-Type: application/json' \
  -d "$STOP_BODY" "$BASE/api/stop-session" > /dev/null
rm token.header
```

The answer (`{"instance": "...", "run_id": "..."}`) only says that the server received the stop request for that run. The run ends a few seconds later (`state` in the `session` of `GET $BASE/api/session` becomes `"ended"`). Then stop the GUI server with Ctrl+C.

### Troubleshoot your screen

| Symptom | Cause | Fix |
|---|---|---|
| The value stays `-`, with a sentence that the variables are not available | No run has started, or the run has no Control Server. | Click **Start**. If the run has started, start the GUI server with a launcher that has a Control Server. |
| The part says `this variable is not in the current run` | The participant name or the variable name is not in this run. | Check the names as in step 2. |
| The screen shows nothing, and the browser console shows a CSP error | The page uses inline `<script>` or `onclick="..."`. | Put the script in a file, and register handlers with `addEventListener` or the `onclick` property. |
| A file returns 404 | Its extension is not served, its name starts with `.`, or it is a symbolic link. | See the table in "Files served with `--web`" in [gui.md](../gui.md#files-served-with---web). |
| A `POST` with `curl` is rejected with `forbidden-origin` | The `Origin` header is missing or differs from the server's own origin. | Add `-H "Origin: http://127.0.0.1:<port>"`. |
| The screen does not change after you edit a file | The browser shows the old page. | Reload the page. You do not need to restart the GUI server. |
| `cannot listen on 127.0.0.1:<port> (Address already in use)` | You started a second GUI server with the `--port` of one that is running. | Stop the first one, or choose another `--port` (or leave it out). Without `--port`, the second server starts on another port with its own token and its own launcher: use the URL it prints. |

## Connect a network outside (optional)

A `can` or `ethernet` network of your configuration file can be connected to a real bus through the launcher's bridge (see "Connecting a bus outside (`--connect`)" in [simulation-run.md](../simulation-run.md#connecting-a-bus-outside---connect)). Two parts do it:

```html
<label>ETH_EXT <vsil-connect network="ETH_EXT"></vsil-connect></label>
<vsil-outside></vsil-outside>
```

| Element | What it shows or does |
|---|---|
| `<vsil-connect network="...">` | Where the network is connected in the next run. It starts at `SIL Kit only (not connected)`. It asks for the list of targets only when you open it, and the list is that of the PC where the GUI server runs, not of the PC of the browser. After you choose, it shows what the check says about that choice, in the launcher's words (for example, that the run then keeps real time). **Start** of `<vsil-run-controls>` starts with the choice. |
| `<vsil-outside>` | While the run goes, one row for each network that it connected: the link or the bus, the condition and the frame counts. Nothing when there is none. |

A run connected outside keeps real time, so pause and step cannot be used. To use them anyway, and only when what is connected there can take messages that do not come in real time, set the time policy from a script of your own: `pageIntent.update({ time_policy: 'free' })` (`pageIntent` is exported by `/vsil/vehicle-sil-gui-elements.mjs`; the check then says what that means).

A test bench can keep its choices in a [request file](../simulation-run.md#request-files---request). A script can read one into the choice of the next run, and save the choice as one; the GUI server writes no file:

```js
import { pageIntent } from '/vsil/vehicle-sil-gui-elements.mjs';

// <input type="file" id="request">: read the request file that the user picks
document.querySelector('#request').addEventListener('change', async (event) => {
  const request = JSON.parse(await event.target.files[0].text());
  delete request.$schema; // for an editor only
  pageIntent.update(request);
});
// <button type="button" id="save">: save the choice of the next run as a request file
document.querySelector('#save').addEventListener('click', () => {
  const text = JSON.stringify({ vehicle_sil_run_request: 1, ...pageIntent.get() }, null, 2);
  const link = document.createElement('a');
  link.href = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
  link.download = 'request.json';
  link.click();
});
```

- `<vsil-connect>` shows a target read from a file as written. The GUI server takes a target only from its last list of that network, so open the choice before you start: a target that the list does not offer goes back to SIL Kit only, with a line under the choice that says which and why. Without a list, the start is refused with `target-not-listed`.
- `scenario` of a request file is a path from the directory of the configuration file; the GUI server takes the path from its list of scenarios instead (`<vsil-scenario-select>` writes it).

## What to read next

- Add a display, a button or a rule of your own with a script: `main.js` of the template, and "The front door (`connect()`)" in [gui/client/README.md](../../gui/client/README.md#the-front-door-connect). The script uses the same connection as the parts.
- Build with TypeScript and a framework (React, Vue, Angular and others; the parts are custom elements, so any template can contain them): "A project with a framework" in [examples/gui_starter/README.md](../../examples/gui_starter/README.md#a-project-with-a-framework) (it has a whole run through the front door and an Angular signal)
- Show the timeline of a scenario run on your screen (one script and one tag, in any framework): "Putting a timeline on your own screen" in [gui.md](../gui.md#putting-a-timeline-on-your-own-screen)
- Look up the served files, the CSP, the development server and the rules to follow: "Reference for your own screen" in [gui.md](../gui.md#reference-for-your-own-screen)
- Keep the sample screens and add a part to them: "Custom panels" in [gui.md](../gui.md#custom-panels)
