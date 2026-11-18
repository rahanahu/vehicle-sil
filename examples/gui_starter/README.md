# Starter for your own screen (gui_starter)

[日本語](README.ja.md)

A screen of your own for the GUI server (`vehicle-sil-gui`) to serve, made of parts: you place elements such as `<vsil-slider variable="Engine.engine.throttle">` in an HTML file and they work, with no script and no build step. Copy it and change the variable names to your ECU's. The guide [Build your own screen for the browser GUI](../../docs/howto/gui-screen.md) walks you through that.

| File | Contents |
|---|---|
| `index.html` | The screen: the parts, and a few lines of markup for the script. |
| `main.js` | A small script of your own for what the parts do not have: a display, a button and the timeline. |
| `style.css` | The look. The parts have none of their own; this file styles the `data-*` attributes and `aria-disabled` that they set. |

The parts and the library are not in this directory: the page loads them from the GUI server (`/vsil/vehicle-sil-gui-elements.mjs` and `/vsil/vehicle-sil-gui-client.mjs`).

## Run it

Pass this directory to the GUI server with `--web`.

| Use | Command |
|---|---|
| Prebuilt tools | `vehicle-sil-gui --web <tools>/gui_starter simulation.yaml` |
| Repository | Build the library files once, then start the server (the commands below). |

In the repository, run this from the root:

```sh
(cd gui/web && npm ci && npm run build:client)    # build/gui-client/: the files that the server serves under /vsil/
uv run vehicle-sil-gui --launcher build/<preset>/tools/launch/vehicle_sil_launch \
  --web examples/gui_starter build/<preset>/examples/ecu_main/simulation.yaml
```

### Verify the screen

The browser opens the URL that the server printed (with `#token=`) and shows this screen.

| Action | What happens |
|---|---|
| Open the screen | The status line at the top says `ready`. The variables say they are not available now (no run). |
| Click **Start** | The status line says `running` and the virtual time (`running 2820 ms`). The slider and the values come alive. |
| Move the **Throttle** slider | The value is written when you release the knob. **Engine speed** changes. |
| Click **Throttle to 100 %** (your own button) | The slider moves to 100 and the meter under "Your own script" follows. Before a run, the button shows why it was refused. |
| Click **Pause**, **Step**, then **Resume** | The status line says `paused` and the time; **Step** moves it by 10 ms (the field next to it); **Resume** goes on. |
| Click **Stop** | The status line says `the run was stopped`. The slider is disabled, and the values say they are the last ones of a run that has ended. |
| Pick a scenario under **Scenario** and click **Start** | The run ends by itself. The result says `passed` (or `failed`) with each step, and the timeline of the run appears. |

Variables appear when the launcher has a Control Server. In the example ECUs, the throttle drives `Engine.engine.speed`.

## Make it your own screen

Copy the starter to another directory and build your screen there.

| Use | Command to copy |
|---|---|
| Prebuilt tools | `cp -r <tools>/gui_starter my-screen` |
| Repository | `cp -r examples/gui_starter my-screen` |

Then start the server with `--web my-screen`. It takes the directory that contains `index.html`; for the serving rules (which files are served, CSP, token), see "Reference for your own screen" in [docs/gui.md](../../docs/gui.md#reference-for-your-own-screen).

There are 3 steps, and you take only as many as you need:

| Step | You write | Read |
|---|---|---|
| 1. Parts only | HTML: the elements of `index.html` with your variable names, and CSS. | The table below |
| 2. A script of your own | `main.js`: what the parts do not have (a display, a button, a rule). It uses the same connection as the parts. | "[The front door](../../gui/client/README.md#the-front-door-connect)" in `gui/client/README.md` |
| 3. TypeScript and a framework | A project with a build. The parts are custom elements, so any template can contain them. | The end of this page |

### Step 1: parts only

| Element | What it does | Attributes |
|---|---|---|
| `<vsil-status>` | The connection, the run, the virtual time and the one problem | none |
| `<vsil-run-controls>` | Start, Stop and Stop without waiting. It starts the file that the server was started with. | `label-*` |
| `<vsil-time-controls>` | Pause, Resume, Step, Run until and the speed | `speeds`, `step-ms` |
| `<vsil-value>` | A value with its unit; a bool shows as `on` and `off` text (a lamp, with CSS) | `variable`, `on`, `off` |
| `<vsil-slider>` | A number in the variable's range, written when the knob is released | `variable` |
| `<vsil-write>` | A button that writes one value | `variable`, `value`, `label` |
| `<vsil-scenario-select>`, `<vsil-scenario-result>` | The scenario of the next run, and its steps and result | none |

A variable is written `<participant>.<variable>`, cut at the first dot (`Engine.engine.throttle` is the participant `Engine` and the variable `engine.throttle`). To find the names of your ECUs, see step 2 of the guide. If a name is wrong, the part says that the variable is not in this run. The complete list of the parts and what they promise (the connection, the markup and the `data-*` attributes to style) is "Parts" in [gui/client/README.md](../../gui/client/README.md#parts-custom-elements).

### Step 2: a script of your own

`main.js` shows the 3 things that a script is for. `connect()` without options returns the connection that the parts already use, so a refusal of your button shows in `<vsil-status>` as well, and the page has one WebSocket.

| In `main.js` | What it shows |
|---|---|
| `watch([client.variable('Engine.engine.speed')], fn)` | A value drawn your own way (a meter). `fn` runs now and on every change. |
| `await client.variable('Engine.engine.throttle').write(100)` | An operation of your own: a Promise that rejects with an `ActionError` (a sentence in `message`, and `outcome`: `refused` or `unknown`). |
| `watch([client.scenario], fn)` | The timeline: when a scenario run has one, `<vehicle-sil-timeline>` gets its `src`. |

The timeline element is loaded from the GUI server (`<script src="/timeline/vehicle-sil-timeline.js">`). It sends the token that the page already has. Its attributes, properties and events are in "Putting a timeline on your own screen" in [docs/gui.md](../../docs/gui.md#putting-a-timeline-on-your-own-screen).

What the starter leaves out, because the sample screens do it: choosing the configuration file, **Check**, and a field to choose any variable and type a value. `client.low` is the lower library (`createClient`, `check`, `variables.bind`) of the same connection when you need it; see "[Build widgets (`bind`)](../../gui/client/README.md#build-widgets-bind)" for widgets of your own.

### Step 3: TypeScript and a framework

The library comes as a typed npm package (`.tgz`) with the parts. Run `npm install <that .tgz>` in your project, then import it as `@vehicle-sil/gui-client`. It has no dependencies.

| Source | Location |
|---|---|
| Prebuilt tools | `<tools>/client/vehicle-sil-gui-client-<version>.tgz` |
| Releases page | The same file |
| Repository | Run `npm run pack:client` in `gui/web`. It creates `build/gui-client/vehicle-sil-gui-client-<version>.tgz`. |

```ts
import '@vehicle-sil/gui-client/elements'; // defines the parts: write <vsil-slider ...> in your templates
import { ActionError, connect, watch } from '@vehicle-sil/gui-client';

const client = await connect(); // the connection that the parts use
watch([client.variable('Engine.engine.speed')], (view) => console.log(view.text, view.unit));
try {
  await client.variable('Engine.engine.throttle').write(60);
} catch (error) {
  if (error instanceof ActionError) console.log(error.code, error.message, error.outcome);
}
```

- `npm run pack:client` type-checks this example against the types of the package it built, with both `nodenext` and `bundler` module resolution.
- Every state is a `Readable`: a read-only value with `get()` and `subscribe(fn)`. To turn it into your framework's state (React `useSyncExternalStore`, a Vue `ref`, a Svelte store, an Angular signal and so on), convert it with `subscribe`.
- To develop on a development server (Vite and others) while connected, see "Use a development server" in [docs/gui.md](../../docs/gui.md#use-a-development-server).
