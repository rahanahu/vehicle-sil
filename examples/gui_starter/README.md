# Starter for your own screen (gui_starter)

[日本語](README.ja.md)

A screen of your own for the GUI server (`vehicle-sil-gui`) to serve, made of parts: you place elements such as `<vsil-slider variable="Engine.engine.throttle">` in an HTML file and they work, with no script and no build step. Copy it and change the variable names to your ECU's. The guide [Build your own screen for the browser GUI](../../docs/howto/gui-screen.md) walks you through that.

| File | Contents |
|---|---|
| `index.html` | The screen: the parts, and a few lines of markup for the script. |
| `main.js` | A small script of your own for what the parts do not have: a display and a button. |
| `style.css` | The look. The parts have none of their own; this file styles the `data-*` attributes and `aria-disabled` that they set. |

The parts and the library are not in this directory: the page loads them from the GUI server (`/vsil/vehicle-sil-gui-elements.mjs` and `/vsil/vehicle-sil-gui-client.mjs`).

## Run it

Pass this directory to the GUI server with `--web`.

| Use | Command |
|---|---|
| Prebuilt tools | `vehicle-sil-gui --web <tools>/gui_starter simulation.live.yaml` |
| Repository | Build the library files once, then start the server (the commands below). |

In the repository, run this from the root:

```sh
(cd gui/web && npm ci && npm run build:client)    # build/gui-client/: the files that the server serves under /vsil/
uv run vehicle-sil-gui --launcher build/<preset>/tools/launch/vehicle_sil_launch \
  --web examples/gui_starter build/<preset>/examples/ecu_main/simulation.live.yaml
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
| Click **Stop** | The status line says `The run was stopped by a stop request without errors`. The slider is disabled, and the values say they are the last ones of a run that has ended. |
| Pick a scenario under **Scenario** and click **Start** | The run ends by itself. The result says `passed` (or `failed`) with each step, and the timeline of the run appears. |

Variables appear when the launcher has a Control Server. In the example ECUs, the throttle drives `Engine.engine.speed`.

## Make it your own screen

Copy the starter to another directory and build your screen there.

| Use | Command to copy |
|---|---|
| Prebuilt tools | `cp -r <tools>/gui_starter my-screen` |
| Repository | `cp -r examples/gui_starter my-screen` |

Then start the server with `--web my-screen`. It takes the directory that contains `index.html`; for the serving rules (which files are served, CSP, token), see "Reference for your own screen" in [docs/gui.md](../../docs/gui.md#reference-for-your-own-screen).

There are 3 layers, and you use only as many as you need:

| Layer | You write | Read |
|---|---|---|
| Parts only | HTML: the elements of `index.html` with your variable names, and CSS. | The table below |
| A script of your own | `main.js`: what the parts do not have (a display, a button, a rule). It uses the same connection as the parts. | "[The front door](../../gui/client/README.md#the-front-door-connect)" in `gui/client/README.md` |
| A project with a framework | A project with a build. The parts are custom elements, so any template can contain them. | The end of this page |

### Parts only

| Element | What it does | Attributes |
|---|---|---|
| `<vsil-status>` | The connection, the run, the virtual time and the one problem | none |
| `<vsil-run-controls>` | Start, Stop and Stop without waiting. It starts the file of the last run (at first, the file that the server was started with). | `label-*` |
| `<vsil-time-controls>` | Pause, Resume, Step, Run until and the speed | `speeds`, `step-ms` |
| `<vsil-value>` | A value with its unit; a bool shows as `on` and `off` text (a lamp, with CSS) | `variable`, `on`, `off` |
| `<vsil-slider>` | A number in the variable's range, written when the knob is released | `variable` |
| `<vsil-write>` | A button that writes one value | `variable`, `value`, `label` |
| `<vsil-scenario-select>`, `<vsil-case-select>`, `<vsil-scenario-result>` | The scenario and case of the next run, and its steps, parameters and result | none |
| `<vsil-timeline>` | The timeline of the scenario run, in the `<vehicle-sil-timeline>` that you put inside it (it adds one when there is none). Its attributes (`compact`, `hide`, `lang`) are yours. The page loads the element's script: `<script src="/timeline/vehicle-sil-timeline.js">`. | none |

A variable is written `<participant>.<variable>`, cut at the first dot (`Engine.engine.throttle` is the participant `Engine` and the variable `engine.throttle`). To find the names of your ECUs, see "Step 2: Find the name of the variable to show" in the guide. If a name is wrong, the part says that the variable is not in this run. The complete list of the parts and what they promise (the connection, the markup and the `data-*` attributes to style) is "Parts" in [gui/client/README.md](../../gui/client/README.md#parts-custom-elements).

### A script of your own

`main.js` shows the 2 things that a script is for. `connect()` without options returns the connection that the parts already use, so the page has one WebSocket. A refused start, stop or time operation shows in `<vsil-status>`; a refused write of a variable shows next to the part that shows that variable, and in the `ActionError` that your script receives.

| In `main.js` | What it shows |
|---|---|
| `watch(client.variable('Engine.engine.speed'), fn)` | A value drawn your own way (a meter). `fn` runs now and on every change. |
| `await client.variable('Engine.engine.throttle').write(100)` | An operation of your own: a Promise that rejects with an `ActionError` (a sentence in `message`, and `outcome`: `refused` or `unknown`). |

What the starter leaves out, because the sample screens do it: choosing the configuration file, **Check**, and a field to choose any variable and type a value. To choose the file, list the files with `client.low.configs()` and start with your own button: `client.run.start(intent, { file })` (`<vsil-run-controls>` starts the file of the last run). `client.low` is the lower library of the same connection (`check`, `variables.bind` and more); see "[Build widgets (`bind`)](../../gui/client/README.md#build-widgets-bind)" for widgets of your own.

### Connect a network outside

To connect a `can` or `ethernet` network of your configuration file to a real bus, add `<vsil-connect network="<network>">` (where it is connected in the next run; it starts at SIL Kit only and lists the targets of the GUI server's PC only when it is opened) and `<vsil-outside>` (the connected networks while the run goes). The starter has neither, because the network names are those of your configuration file. A run connected outside keeps real time; `pageIntent.update({ time_policy: 'free' })` from your script lifts that, for when what is connected can take messages that do not come in real time. How to read a request file of a test bench into the next run and save one is in "Connect a network outside (optional)" in the [guide](../../docs/howto/gui-screen.md#connect-a-network-outside-optional).

### A project with a framework

The library comes as a typed npm package (`.tgz`) with the parts. Run `npm install @vehicle-sil/gui-client@<version>` in your project, with the version of your GUI server (or `npm install <that .tgz>`), then import it as `@vehicle-sil/gui-client`. It has no dependencies.

| Source | Location |
|---|---|
| Prebuilt tools | `<tools>/client/vehicle-sil-gui-client-<version>.tgz` |
| Releases page | The same file |
| npm registry | The same file, as `@vehicle-sil/gui-client` |
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

- `npm run pack:client` type-checks this example, and the other `ts` examples below (except the one that imports from Angular), against the types of the package it built, with both `nodenext` and `bundler` module resolution. It checks the types only: it does not run the calls.
- Through the front door only, a screen starts, pauses, steps, resumes and stops a run, writes an input and reads an output (every call is an operation of `connect()`). A write while the run is paused is applied by the next step, so the example sends it without waiting, steps, and then waits for it. `until` waits for a state of `status`:

```ts
import { ActionError, connect, until } from '@vehicle-sil/gui-client';

const front = await connect();
let unsubscribe = () => {};
try {
  await front.run.start({ start_paused: true }); // resolves when the server has accepted the start, not when the run is up
  await until(front.status, (status) => status.run === 'paused', { timeoutMs: 30_000 });
  unsubscribe = front.variable('Engine.engine.speed').subscribe((view) => console.log(view.text, view.unit)); // values arrive while you subscribe
  const written = front.variable('Engine.engine.throttle').write(60); // not awaited yet: a paused run applies it in the next step
  written.catch(() => undefined); // no unhandled rejection if the step fails first; `await written` below still throws
  await front.time.step(100); // resolves when 100 ms of simulated time have passed and the run is paused again
  await written;
  await front.time.resume();
  await front.time.pause(); // pause again when you like; `resume()` goes on
} catch (error) {
  if (error instanceof ActionError) console.log(error.code, error.message, error.outcome);
  else throw error;
} finally {
  unsubscribe();
  await front.run.stop().catch(() => undefined); // never leave the run behind
}
```

- `front.clock` has `sim_ms`, the virtual time in ms; it is `null` before a run (the type is `number | null`), so check for `null` before you show it.
- To play a scenario, choose one of the scenario files next to the start-up file (`scenarios()` takes `{ file }` for another configuration file; give the same `file` to `run.start`) and wait for the end of the run. A run with a scenario refuses writes (`scenario-running`), so this example writes nothing:

```ts
import { connect, until } from '@vehicle-sil/gui-client';

const front = await connect();
const [scenario] = await front.scenarios(); // the list is empty if there are none
if (scenario) {
  await front.run.start({ scenario: scenario.path }); // when it resolves, `status` already shows this run, not an earlier one that ended
  const status = await until(front.status, (s) => s.run === 'ended' || s.run === 'crashed', {
    timeoutMs: 600_000,
  });
  console.log(status.result?.ok, status.result?.message);
}
```

- To turn a `Readable` into an Angular signal, subscribe in an injection context and unsubscribe when the component goes (`DestroyRef`):

```ts
import { DestroyRef, inject, signal } from '@angular/core';
import type { Readable } from '@vehicle-sil/gui-client';

/** An Angular signal that follows a `Readable` (call it in an injection context). */
export function readableSignal<T>(readable: Readable<T>) {
  const destroyRef = inject(DestroyRef); // first: it throws outside an injection context, before anything is subscribed
  const state = signal(readable.get());
  destroyRef.onDestroy(readable.subscribe((value) => state.set(value)));
  return state.asReadonly();
}
```

- Every state is a `Readable`: a read-only value with `get()` and `subscribe(fn)`. To turn it into your framework's state (React `useSyncExternalStore`, a Vue `ref`, a Svelte store, an Angular signal and so on), convert it with `subscribe` (the signal above is an example; the package itself has no framework code).
- To develop on a development server (Vite and others) while connected, see "Use a development server" in [docs/gui.md](../../docs/gui.md#use-a-development-server).
