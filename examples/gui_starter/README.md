# Template for your own screen (gui_starter)

[日本語](README.ja.md)

A minimal screen of your own for the GUI server (`vehicle-sil-gui`) to serve. It uses only the client library ([gui/client](../../gui/client/README.md)) and is written in plain HTML and ES modules, with no build step. Copy it and rewrite it into your own screen. The guide [Build your own screen for the browser GUI](../../docs/howto/gui-screen.md) walks you through turning it into a screen that shows one variable.

| File | Contents |
|---|---|
| `index.html` | The screen. It loads its script from a file (CSP blocks inline scripts). |
| `main.js` | The script that uses the library: token, connection, check, start and stop, watching and writing a variable, and `bind`, which connects a widget to a variable. |
| `style.css` | The styles. |
| `vehicle-sil-gui-client.mjs` | The library bundled into one file. |

The screen shows the connection state, checks the configuration file, starts and stops a run, lets you pick one variable to watch and write, and has a slider and a lamp.

## Run it

Start the GUI server and pass this directory with `--web`. The library file (`vehicle-sil-gui-client.mjs`) must sit next to `index.html`. There are 2 ways to get it there:

| Use | Library file | Command to start |
|---|---|---|
| Prebuilt tools | Already in `<tools>/gui_starter/`. | `vehicle-sil-gui --web <tools>/gui_starter simulation.yaml` |
| Repository | Not tracked in Git: build it and copy it with the commands below. | Line 3 of the commands below |

In the repository, run this from the root:

```sh
(cd gui/web && npm ci && npm run build:client)    # build/gui-client/vehicle-sil-gui-client.mjs
cp build/gui-client/vehicle-sil-gui-client.mjs examples/gui_starter/
uv run vehicle-sil-gui --launcher build/<preset>/tools/launch/vehicle_sil_launch \
  --web examples/gui_starter build/<preset>/examples/ecu_main/simulation.yaml
```

### Verify the screen

The browser opens the URL that the server printed (with `#token=`) and shows this screen.

| Action | What happens |
|---|---|
| Click **Check** | The screen checks the configuration file and shows the result. |
| Click **Start** | The run starts. |
| Select a variable under **Variable** | The screen shows the value of that variable. |
| Enter a value in **New value** and click **Write** | The value is written to the variable. |

Variables appear in the list when the launcher has a Control Server. In the example ECUs, writing a number to `Engine.engine.throttle` changes `Engine.engine.speed`.

## The slider and the lamp

The **Slider and lamp** section of the screen shows how to build a widget.

| Widget | Variable it uses |
|---|---|
| Slider | The first writable number variable that has a range |
| Lamp | A bool variable (a read-only one if there is one) |

Both only draw the `state` that `client.variables.bind(ref, options)` returns. The library decides the rest:

| What `state` holds | Examples |
|---|---|
| Value condition | No value yet, stale, the value of an ended run |
| Why a write is blocked | No run, the ECU is disconnected |
| Write status | Sent, accepted, applied, the reason it failed |

If you keep moving the slider before the server has received the previous write, the library keeps only the last value and sends that. You can build your own widgets (switch, number input, choice) the same way (see "Build widgets (`bind`)" in [gui/client/README.md](../../gui/client/README.md#build-widgets-bind)).

## Make it your own screen

Copy the template to another directory and build your screen there.

| Use | Command to copy | Argument to start with |
|---|---|---|
| Prebuilt tools | `cp -r <tools>/gui_starter my-screen` | `--web my-screen` |
| Repository | `cp -r examples/gui_starter my-screen` | `--web my-screen` |

`--web` takes the location of the built screen (the directory that contains `index.html`). For the serving rules (which files are served, CSP, token), see "Reference for your own screen" in [docs/gui.md](../../docs/gui.md#reference-for-your-own-screen).

## Build with TypeScript and a framework

The library also comes as a typed npm package (`.tgz`). Run `npm install <that .tgz>` in your project, then import it as `@vehicle-sil/gui-client`. It has no dependencies.

| Source | Location |
|---|---|
| Prebuilt tools | `<tools>/client/vehicle-sil-gui-client-<version>.tgz` |
| Releases page | The same file |
| Repository | Run `npm run pack:client` in `gui/web`. It creates `build/gui-client/vehicle-sil-gui-client-<version>.tgz`. |

```ts
import { WriteError, createClient, takeToken } from '@vehicle-sil/gui-client';

const token = takeToken(window); // reads #token= from the URL and removes it (before routing rewrites the URL)
if (token === null) throw new Error('open the URL that vehicle-sil-gui printed');
// build the API URL from the server root (so that api/app does not become /settings/api/app on a screen path /settings/foo)
const client = createClient({ baseUrl: new URL('/', location.href).href, token });
await client.loadApp();
client.connect();
client.session.session.subscribe((session) => console.log(session?.phase, session?.result?.result));

const ref = { peer: 'Engine', name: 'engine.speed' };
const watch = client.variables.watch([ref], { periodMs: 100 });
watch.values.subscribe(() => console.log(watch.value(ref)?.value));
try {
  await client.variables.write({ Engine: { 'engine.throttle': 50 } });
} catch (error) {
  if (error instanceof WriteError) console.log(error.kind, error.message);
}

// one widget and one variable: the value condition, why a write is blocked and the write status come in one state
const throttle = client.variables.bind({ peer: 'Engine', name: 'engine.throttle' }, { needsRange: true });
throttle.state.subscribe(({ reading, pending, block, writeStatus }) =>
  console.log(pending ?? reading?.value, block, writeStatus.kind),
);
throttle.write(60);
```

- `npm run pack:client` type-checks this example against the types of the package it built, with both `nodenext` and `bundler` module resolution.
- Every state is a `Readable`: a read-only value with `get()` and `subscribe(fn)`. To turn it into your framework's state (React `useSyncExternalStore`, a Vue `ref`, a Svelte store, an Angular signal and so on), convert it with `subscribe`.
- To develop on a development server (Vite and others) while connected, see "Use a development server" in [docs/gui.md](../../docs/gui.md#use-a-development-server).
