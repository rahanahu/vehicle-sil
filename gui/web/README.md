# gui/web

[日本語](README.ja.md)

Reference for developing the sample screens (Angular, TypeScript): requirements, commands, directory layout, and coding rules (Angular style, layout, accessibility, internationalization, tests).

The sample screens are a reference implementation built only on the client library (`gui/client`) and its Angular wrapper. The Python GUI server serves the built screens (when started without `--web`). Using the screens needs no Node.js; you need Node.js only to develop and build them.

| To find                                       | See                                                             |
| --------------------------------------------- | --------------------------------------------------------------- |
| What to install for development               | [Requirements](#requirements)                                   |
| Commands to build, test and lint              | [Commands](#commands)                                           |
| Running the screens on the development server | [Run on the development server](#run-on-the-development-server) |
| What the end-to-end tests start               | [End-to-end tests](#end-to-end-tests)                           |
| Which file is where                           | [Directories](#directories)                                     |
| How to write the code                         | [Coding rules](#coding-rules)                                   |
| Adding UI text, adding a language             | [Internationalization](#internationalization)                   |

For building your own screen from scratch, and for copying the widgets of these screens into another Angular app, see "See also" at the end.

## Requirements

| Requirement                      | Details and use                                                                       |
| -------------------------------- | ------------------------------------------------------------------------------------- |
| Node.js                          | 24 (`.nvmrc`). `engines` in `package.json` allows 24 (24.15 or later) and 26          |
| npm                              | 11.12.1 or later (the one that comes with Node.js). Do not use other package managers |
| [uv](https://docs.astral.sh/uv/) | Runs the GUI server `gui/server` (used in development and in the end-to-end tests)    |
| Browser for the end-to-end tests | Install it with `npx playwright install chromium --only-shell`                        |

- `engine-strict` in `.npmrc` makes `npm ci` stop outside the `engines` range.
- `npx playwright install chromium --only-shell` installs the Chromium that matches the Playwright version into the user's cache. It needs no administrator rights.

The same commands are meant to work on Windows. Use the Windows build of Node.js. Run `scripts/verify-web.py` with a Windows Python (from python.org or MinGW).

## Commands

Run these in `gui/web/`. Each reports success or failure through its exit code.

| Command                 | What it does                                                                                       |
| ----------------------- | -------------------------------------------------------------------------------------------------- |
| `npm ci`                | Installs exactly what `package-lock.json` lists                                                    |
| `npm start`             | Starts the development server (http://127.0.0.1:4200)                                              |
| `npm run build`         | Production build (output in `dist/browser/`, license notices in `dist/3rdpartylicenses.txt`)       |
| `npm test`              | Runs the unit tests (Vitest, jsdom) once and exits                                                 |
| `npm run test:client`   | Type check and unit tests of the client library (`../client`) (Vitest, Node; no Angular, no DOM)   |
| `npm run test:timeline` | Unit tests of the timeline page (`../timeline/viewer.js`) (Node's test runner; no DOM)             |
| `npm run build:client`  | Bundles the client library into one ESM file                                                       |
| `npm run pack:client`   | Packs the client library as a typed npm package                                                    |
| `npm run lint`          | ESLint (angular-eslint). Warnings also fail                                                        |
| `npm run format:check`  | Checks Prettier formatting (`../client` and `../timeline` too; settings in `../.prettierrc`)       |
| `npm run e2e`           | Playwright. The GUI server serves `dist/browser/` and the API from the same origin                 |
| `npm run e2e:dev`       | Playwright. Through the development server and its proxy (WebSocket too)                           |
| `npm run e2e:real`      | Playwright. With the real launcher and the ecu_main example ECUs built next to it (`e2e/real/`)    |
| `npm run i18n:extract`  | Writes the text to translate to `build/i18n/messages.json` (`build/` at the top of the repository) |

- `npm start` forwards `/api/` and `/ws` to the GUI server (127.0.0.1:8765) (`proxy.conf.mjs`; the port comes from the environment variable `GUI_PORT`).
- `npm run build:client` writes `../../build/gui-client/vehicle-sil-gui-client.mjs`. It uses the esbuild that comes with the Angular build tools.
- `npm run pack:client` writes `../../build/gui-client/vehicle-sil-gui-client-<version>.tgz`, then installs it to check it (`../client/pack.mjs`).
- `npm run lint` also lints `../client` and `../timeline`, each with its own `eslint.config.js` (plain ESLint, not angular-eslint).
- `npm run e2e` and `npm run e2e:real` need `npm run build` first.
- `npm run e2e:real` takes the launcher from the environment variable `VEHICLE_SIL_GUI_LAUNCHER`.
- `scripts/verify-web.py` compares and updates the translation files ("Internationalization" below).

### Run everything (`scripts/verify-web.py`)

Run `scripts/verify-web.py` from the top of the repository. It runs the following in order and prints only a summary:

`npm ci`, formatting, lint, unit tests, unit tests of the client library, comparison of the translation files, build, bundling the library (`bundle`), the package (`package`), end-to-end tests.

While you work on the end-to-end tests, narrow the run with these options:

| Option                        | What it does                                                           |
| ----------------------------- | ---------------------------------------------------------------------- |
| `--skip-checks`               | Skips formatting, lint, unit tests and the translation file comparison |
| `--e2e-grep <part of a name>` | Runs only the end-to-end tests whose names match                       |
| `--e2e-repeat <count>`        | Repeats the end-to-end tests                                           |

### Run on the development server

Run these in 2 terminals:

```sh
uv run vehicle-sil-gui --no-browser --json --port 8765 --dev-origin http://127.0.0.1:4200 \
  --launcher ../server/tests/replay_launcher.py --root ../server/tests/configs   # GUI server
npm start                                                                     # screens (127.0.0.1:4200)
```

- The GUI server uses the replay launcher (`gui/server/tests/replay_launcher.py`; see "Development" in docs/gui.md), which replays recordings of the real launcher's output.
- On the screen, you can choose the configuration files in `gui/server/tests/configs/` (the ones that have recordings).
- In the browser, open the `dev_url` (with the token) that the server prints.

### End-to-end tests

The end-to-end tests start the GUI server themselves and stop it when they finish.

| Command                          | What it starts                         | Code that starts it          |
| -------------------------------- | -------------------------------------- | ---------------------------- |
| `npm run e2e`, `npm run e2e:dev` | The GUI server and the replay launcher | `e2e/support/gui-server.ts`  |
| `npm run e2e:real`               | The GUI server and the real launcher   | `e2e/support/real-server.ts` |

- Each run uses free ports that the OS picks (the development server too).
- For `npm run e2e:real`, `gui/server/tests/real_configs.py` writes the configuration files to a temporary directory. `scripts/verify-gui.py --launcher <path>` runs these end-to-end tests.

Set the environment variable `VEHICLE_SIL_GUI_SHOTS` to a directory to take screenshots.

| End-to-end test                                             | What it captures                                 |
| ----------------------------------------------------------- | ------------------------------------------------ |
| `npm run e2e:real`                                          | 4 states at 3 widths                             |
| The replay launcher's end-to-end test (`e2e/shots.spec.ts`) | 6 states of the Run screen at 360 px and 1024 px |

`e2e/shots.spec.ts` starts a new GUI server for each state, so earlier runs do not show. `VEHICLE_SIL_GUI_SHOTS_LANGUAGE=en` captures the English screens.

## Directories

| Path                                     | Contents                                                                         |
| ---------------------------------------- | -------------------------------------------------------------------------------- |
| `src/app/app.*`                          | The shell (toolbar, navigation, content area)                                    |
| `src/app/app.routes.ts`, `navigation.ts` | Routes and navigation items                                                      |
| `src/app/core/`                          | Things shared across screens ("`core/`" below)                                   |
| `src/app/core/session/`                  | The Angular wrapper of the client library ("`core/session/`" below)              |
| `src/app/run/`                           | The Run screen (`''`) ("`run/`" below)                                           |
| `src/app/widgets/`                       | Variable widgets and dashboards ("`widgets/`" below)                             |
| `src/app/variables/`                     | The Variables screen (`variables`). Automatic dashboard and filtering            |
| `src/app/not-found/`                     | The screen for unknown routes                                                    |
| `src/app/testing/`                       | A screen for tests (`/_test/layout`)                                             |
| `src/theme.scss`, `src/styles.css`       | Material theme (light and dark), layout dimensions (CSS custom properties)       |
| `src/main.ts`, `src/start.ts`            | Startup                                                                          |
| `src/locale/`                            | Translation files (`messages.<language>.json`). The build puts them in `locale/` |
| `public/`                                | Files served as they are (icons)                                                 |
| `e2e/`                                   | Playwright tests ("`e2e/`" below)                                                |

- `src/main.ts` chooses the language and loads the translations, and only then loads `src/start.ts` (the app).
- The screen in `src/app/testing/` is used by the layout end-to-end tests. It does not appear in the navigation. The helpers for unit tests (fake WebSocket and HTTP, functions that read `gui/protocol/examples/`) are in `@vehicle-sil/gui-client/testing`.

### `core/`

| Directory  | Contents                             |
| ---------- | ------------------------------------ |
| `auth/`    | Token                                |
| `layout/`  | Width classes                        |
| `format/`  | Formatting of measurements and paths |
| `streams/` | Streams                              |
| `session/` | Wrapper of the client library        |
| `i18n/`    | Screen language                      |

The types and parsers for what passes between the screens and the server, the connection, and the session state are in the client library (`../client`, `@vehicle-sil/gui-client`).

### `core/session/`

The wrapper only turns the library's `Readable` (read-only value) into signals.

| File                   | Contents                                                                                        |
| ---------------------- | ----------------------------------------------------------------------------------------------- |
| `gui-client.ts`        | Creates the one client of the app (HTTP through HttpClient, `SOCKET_FACTORY`, `readableSignal`) |
| `server-connection.ts` | Connection and session copy                                                                     |
| `server-info.ts`       | `api/app` and one-off requests                                                                  |
| `simulation-time.ts`   | Time                                                                                            |
| `run-commands.ts`      | Start and stop                                                                                  |
| `ecu-variables.ts`     | ECU variables                                                                                   |

### `run/`

| File             | Contents                                                                                                                              |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| `run-session.ts` | Actions (start, stop, stop without waiting, elapsed time). Has the same lifecycle as the screen; built on the wrapper's `RunCommands` |
| `run-status.ts`  | The status bar that is always visible                                                                                                 |

### `widgets/`

The variable widgets are: value display, number input, slider, switch, lamp, choice and chart.

| File                  | Contents                                                                         |
| --------------------- | -------------------------------------------------------------------------------- |
| `variable-binding.ts` | Connects a widget to a variable (built on the library's `client.variables.bind`) |
| `variable-frame.ts`   | The frame around a widget                                                        |
| `dashboard-spec.ts`   | Dashboard data and default widgets, and building them from a dashboard file      |
| `dashboard-view.ts`   | Shows dashboard data                                                             |
| `panel-host.ts`       | Hosts custom panels                                                              |

The code in this directory uses only the wrapper's public API (the same way you add a widget yourself; see "The Variables screen and widgets" in docs/gui.md).

### `e2e/`

| File                                             | Contents                                                            |
| ------------------------------------------------ | ------------------------------------------------------------------- |
| `app.spec.ts`                                    | Serving and routing                                                 |
| `run.spec.ts`                                    | The Run screen                                                      |
| `before-start.spec.ts`                           | Before the first step                                               |
| `manual.spec.ts`                                 | Participants you start yourself                                     |
| `scenario.spec.ts`                               | Scenarios                                                           |
| `starter.spec.ts`                                | Serving the screen template `examples/gui_starter` with `--web`     |
| `i18n.spec.ts`                                   | The English screens and switching languages                         |
| `layout.spec.ts`, `support/layout.ts`            | Layout                                                              |
| `support/gui-server.ts`                          | Starting the GUI server                                             |
| `support/run-page.ts`                            | Actions on the Run screen                                           |
| `variables.spec.ts`, `support/variables-page.ts` | The Variables screen                                                |
| `shots.spec.ts`                                  | Screenshots                                                         |
| `real/`                                          | Tests that run with the real launcher (`playwright.real.config.ts`) |

## URLs and CSP

Write the API and WebSocket URLs without a leading `/` (`api/app`, `ws`). They are relative to `<base href>` of `index.html`, so the screens also work under a path other than `/`; build them with `ng build --base-href /gui/` in that case. The WebSocket URL is built from `document.baseURI` (`webSocketUrl` in the client library; `core/session/gui-client.ts` passes `document.baseURI` as `baseUrl`).

The production build emits no inline scripts (`inlineCritical: false` in `angular.json`). It works with the CSP `script-src 'self'`. Component styles go in through `<style>`, so `style-src` needs `'unsafe-inline'` (or a nonce).

## Coding rules

Follow the style of current Angular (22). `npm run lint` stops whatever is an error wherever it is written. Rules that need judgment (where to use RxJS and so on) are written here and checked in review.

### File layout and naming

- File and type names follow the `ng generate` defaults (`ng g service telemetry` → `Telemetry` in `telemetry.ts`, `ng g component not-found` → `NotFound` in `not-found/not-found.ts`, `ng g pipe measure` → `MeasurePipe` in `measure-pipe.ts`).
- Create a directory per screen (`src/app/<screen>/`). Put things shared across screens in `src/app/core/`.
- Give an element that tests look for a `data-testid` only when it cannot be found by its role (`getByRole`).

### Angular style (enforced by lint)

- Components are standalone. Dependencies come from `inject()`. Inputs, outputs and queries use `input()`, `output()`, `model()` and `viewChild()`. Use `host` instead of `@HostBinding`.
- Templates use `@if`, `@for`, `@switch` and `@defer`. Do not use the `async` pipe (templates read signals).
- No zone.js (zoneless). Change detection stays OnPush (the default).
- Do not build on experimental or developer-preview APIs. Use the stable alternatives.
- Write guards, interceptors and resolvers as functions. Take route parameters with `input()` (`withComponentInputBinding`).

### Where state lives

| State                                          | Where it lives            |
| ---------------------------------------------- | ------------------------- |
| State of one screen only                       | Signals in that component |
| State of a feature, and values from the server | That feature's service    |
| App-wide state (width class and so on)         | A service in `core/`      |

A service exposes only read-only signals (`asReadonly()`) and methods that change them.

### Signals and RxJS

**Where to use signals**:

- State, values derived from it, and anything a template reads are signals (`signal`, `computed`, `linkedSignal`).

**The library owns communication with the server**:

- Only the client library (`@vehicle-sil/gui-client`) makes requests to the server and holds the connection.
- Screens use the signals and methods of the wrappers in `core/session/` (`ServerConnection`, `ServerInfo`, `SimulationTime`, `RunCommands`, `EcuVariables`) and do not hold an `HttpClient` or WebSocket of their own. This keeps them on the same API you use to build your own screen (docs/howto/gui-screen.md).
- The wrapper sends the library's HTTP requests with `HttpClient` (an interceptor adds the token).
- The library's state is a `Readable` (`get()` and `subscribe()`). The wrapper turns it into a signal with `readableSignal` (`core/session/gui-client.ts`).
- If the library lacks something, do not build a shortcut in the wrapper; add it to the library's public entry point (`@vehicle-sil/gui-client`).
- Production code does not import `@vehicle-sil/gui-client/internal` (no compatibility guarantee) or `/testing` (lint stops it; tests may use them).
- Whatever a subscription throws goes to Angular's `ErrorHandler`.

**How to send requests**:

- One-off fetches (`api/app`, `api/configs`): call the wrapper's Promise-returning method in the loader of a `resource`.
- Check (`POST api/check`) is close to a read, so it is sent automatically from a signal. But it waits briefly to collect changes, waits for the previous check's response before sending the next, and discards stale responses (`check` in `run-page.ts`).
- Requests that change something (start, stop, time commands) go through the wrapper's methods. The library applies the response (the current summary) to the session copy at once.

**Where to use RxJS**:

- Use RxJS when a stream that continues over time needs operators (thinning out, cancellation, combining several streams). Keep it inside a service and expose signals.
- To a caller that needs the stream itself, expose an Observable whose name says it is a stream; the caller turns it into a signal with `toSignal` (`latestValue` and `recentValues` in `core/streams/stream-signals.ts`).
- Reconnection and waits belong to the library's `Connection` (which does not use RxJS).
- A screen that wants to apply notifications itself (values that arrive dozens of times a second, for example) subscribes to `ServerConnection.batches` (an Observable of notifications read in batches) and applies them to its own signals. Screens do not each hold their own connection or authentication.
- Do not push more than a few dozen updates a second to a screen (thinning out is the server's job). Charts use a fixed-length history (`recentValues`).

```ts
// good: the stream is an Observable inside the service; the screen reads a signal (the subscription stops when the screen is destroyed)
readonly samples = this.server.batches.pipe(map(toSamples)); // service
protected readonly latest = latestValue(this.telemetry.samples); // component
```

```ts
// avoid: RxJS for plain state or a one-off fetch
readonly selected$ = new BehaviorSubject<string | null>(null); // → signal<string | null>(null)
readonly configs = toSignal(inject(HttpClient).get<ConfigList>('api/configs')); // → resource({ loader: () => this.info.configs() })
```

### Effects

- Do not use them to sync state ("when A changes, set B" is a `computed` or a `linkedSignal`).
- Use them only to reflect signals into the world outside signals: logging, saving to `localStorage`, external libraries. To measure or draw the DOM, use `afterRenderEffect` (`updateContentMargins` in the shell is the model).

```ts
effect(() => localStorage.setItem('selected', this.selected() ?? '')); // reflects outward only
```

### Layout

- Arrange with CSS (grid, flex, `repeat(auto-fill, minmax(...))`, container queries, `clamp()`, logical properties). Do not compute dimensions in JavaScript.
- There are the 3 width classes in the table below. Only `BREAKPOINTS` in `core/layout/layout.ts` sets the boundaries; the shell passes them as CSS variables (`--app-breakpoint-*`). Dimensions and spacing are custom properties in `src/styles.css`.
- Only the shell reads the width class. Screens and widgets change their arrangement by the content area (container `content`) or by their own width (container queries, auto-fill). Do not write `@media` rules on the viewport width in CSS. Do not read the class with `:host-context`.
- Moving up a class never makes the content area narrower (the shell limits the content width in each class).
- Colors are Material system tokens (`--mat-sys-*`). Light and dark follow the OS setting; motion follows `prefers-reduced-motion`.
- When you add a screen, add it to `ROUTES` in `e2e/layout.spec.ts`. The test checks 8 widths (360, 480, 599, 600, 1024, 1199, 1200, 1920) for overflow (page, scrolling elements, `main`, cards), overlapping cards, the form of the navigation, and the keyboard.

| Class   | Width         | Navigation  |
| ------- | ------------- | ----------- |
| compact | Below 600 px  | Drawer      |
| medium  | Below 1200 px | Icon rail   |
| wide    | Wider         | With labels |

### Accessibility

- Give every route a `title` ("Screen name - vehicle-sil"). One `h1` per screen.
- Everything can be operated with the keyboard alone. Esc closes the drawer.
- Do not convey state by color alone (add text or shape).
- Use Material components (buttons, lists, cards and others). Do not put `aria-live` on values that change quickly.
- Announce state transitions (run started, paused, result, abnormal exit, connection lost) in a `role="status"` region, and errors of actions in a `role="alert"` region. Put the region in place before its content changes (hide it visually while it is empty).
- Actions whose meaning must not change while being pressed (start and stop) are separate elements, always in fixed positions. Do not remove the unusable one: make it unclickable with `disabledInteractive`, keep it focusable, and have its reason read through `aria-describedby` (so a button in the same position never turns into the opposite action during a double click or repeated Enter).
- An action that has become usable does nothing for the first `ACTION_GUARD_MS` (800 ms, `run/run-status.ts`) (`settledAfter` in `core/streams/stream-signals.ts`; the keyboard too). During that time it is shown faded. End-to-end tests wait for `data-armed="true"` before pressing (`press` in `support/run-page.ts`).
- When the focused element disappears (after confirming "Stop without waiting…", for example), move the focus to the element that states the current state (the result text in the bar), not to the opposite action.

### Internationalization

The screen text is written in Japanese (`i18n.sourceLocale` in `angular.json` is `ja`), and the English translation is served from the same build. There is no build per language (the Python package carries the screens in one directory and serves them).

#### How the language is chosen

`chooseLanguage` in `core/i18n/language.ts` decides, in this order:

| Order | Condition                                                                                    | Language                                                             |
| ----- | -------------------------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| 1     | A language chosen in the language menu exists (`vehicle-sil-gui-language` in `localStorage`) | That language                                                        |
| 2     | The browser languages (`navigator.languages`) include a supported language                   | The first, in order of preference, whose primary subtag is supported |
| 3     | None is supported                                                                            | English                                                              |

The primary subtag of `ja-JP` is `ja`.

#### Startup order

1. `main.ts` chooses the language.
2. For English, it fetches `locale/messages.en.json` from the same origin and passes it to `loadTranslations`. Japanese is the source language, so no translation file is loaded.
3. It sets `<html lang>`.
4. It loads `start.ts` (the app) with `import()` and starts it.

`main.ts` does not import the app modules statically. `$localize` fixes the translation the first time a text is evaluated, and some text is evaluated when its module loads (the tables in `run/labels.ts`, for example). `LOCALE_ID` is the chosen language (`Intl.ListFormat` and others follow it).

#### Switching languages

- Changing the language reloads the app (translations are fixed before startup). The run continues in the server, and the screen reconnects and receives everything again.
- Screen values the server does not have (choices not sent yet) are handed over with `LanguageChoice.keep` and picked up with `take` (on the Run screen: the chosen file, the disabled participants, the speed and the log source).
- Focus returns to the language menu.
- The language menu shows a globe and the name of the current language (in that language's own script). The name always includes "Language", in every language. The menu items are written in each language's own script and carry `lang`.

#### Writing UI text

- Mark template text with `i18n` (`i18n-<attribute>` for attributes) and TypeScript text with `$localize`. Lint stops unmarked text. Use the ids created at extraction (they derive from the source text).
- Do not build sentences by concatenation (word order differs between languages). Put values into one sentence as placeholders (`{{ }}` in templates, `${value}:name:` in `$localize`).
- Text whose wording depends on a number ("N lines"; in Japanese "N 行") uses ICU `plural` in templates (Japanese has only `other`; the English translation adds `one`). In TypeScript, write it so the number does not affect the wording ("errors: N"; in Japanese "誤り: N").
- Show measurements with the `measure` pipe (`core/format/measure-pipe.ts`). It uses no thousands separator and fixes the number of decimals. The format is the same in every language; it does not rely on the locale's number format.
- English text from the ECUs and the launcher (findings, logs) is not translated; show it as is (with `lang="en"`).
- Build separators between listed words with `Intl.ListFormat` (`LOCALE_ID`). Do not add separators or `:` with CSS `content` (they cannot be translated).
- Text without a translation (missing from the translation file, `target` `null` or empty, placeholders that do not match the source) shows in the Japanese source text. It never becomes an id or empty text. If the translation file cannot be loaded, the whole screen shows in Japanese.

#### Translation files

A translation file `src/locale/messages.<language>.json` is JSON that lists each extracted text with its source and translation (easy to compare side by side and to read in diffs). `target` `null` marks a text as "not translated yet". `note` describes the file.

```json
{
  "locale": "en",
  "note": "…",
  "translations": {
    "4418433101149864838": { "source": "停止", "target": "Stop" },
    "6952070187813782386": {
      "source": "{VAR_PLURAL, plural, other {{INTERPOLATION} 行}}",
      "target": "{VAR_PLURAL, plural, one {{INTERPOLATION} line} other {{INTERPOLATION} lines}}"
    }
  }
}
```

The i18n stage of `scripts/verify-web.py` extracts the text and compares it with each translation file (`scripts/web_i18n.py`).

| What it finds                                                                       | Handling               |
| ----------------------------------------------------------------------------------- | ---------------------- |
| Text missing from the translation file                                              | Fails                  |
| Source text that differs from the extraction                                        | Fails                  |
| Empty translation                                                                   | Fails                  |
| Translation whose placeholders (`{$name}`) or ICU structure do not match the source | Fails                  |
| Text with `target` `null` (marked as not translated yet)                            | Fails                  |
| Old text that is no longer extracted                                                | Listed (does not fail) |

To pass locally while a translation is unfinished, add `--allow-untranslated` (CI does not).

#### Add or change text

1. Mark the text ("Writing UI text" above).
2. Update the translation files with `scripts/verify-web.py --i18n-update`. New text goes in with `"target": null`, and old text is removed (the removed texts and their translations are printed, so if you only reworded a text you can reuse its translation).
3. Replace each `null` in each file with a translation. The i18n stage fails while any `null` remains (while a translation is unfinished, you can run locally with `--allow-untranslated`).
4. Check with `scripts/verify-web.py --i18n-only`.

#### Add a language

1. Add the code and the language's name in its own script to `LANGUAGES` in `core/i18n/language.ts`.
2. Create `src/locale/messages.<code>.json` as `{"locale": "<code>", "note": "…", "translations": {}}`, fill in the text with `scripts/verify-web.py --i18n-update`, and translate it.
3. For a language other than `en`, register its locale data (dates, plural rules) from `@angular/common/locales/<code>` with `registerLocaleData` (the build provides Japanese, and Angular includes English).
4. Check the screens in that language, as `e2e/i18n.spec.ts` does.

### Types shared with the server

- The shapes exchanged with the server are written only in the client library's `gui/client/src/protocol.ts`, as types and parsers (`read*`) (screens import them from `@vehicle-sil/gui-client`). The reference for the shapes is `gui/protocol/` (README and `examples/`).
- Use received JSON only after it has gone through a parser (the library parses HTTP responses and WebSocket notifications). Ignore and pass on unknown fields, notifications and events.
- Parse session summaries and launcher events part by part. If a part that comes from the launcher, or a part that the server shrank to a limit, cannot be read, do not discard the whole.
- Set an unreadable part to its default (`null`, empty), add its name to `unreadable`, and show "Some parts cannot be shown" in the bar.
- Treat the whole as unreadable only when the core of a summary (`id`, `state`, `launcher`, `stop`) or of an event (`seq`, `event`) cannot be read (for `hello` and `session`, reconnect and receive again).
- The library's `protocol.spec.ts` feeds it every example in `gui/protocol/examples/`. When the examples change, update the types and parsers to match.

### Writing tests

**Unit tests**:

- Unit tests use TestBed. Replace `Layout` with a fake that has signals (jsdom has no `matchMedia`).
- For a `resource`, make it send its request with `TestBed.tick()`, `flush` it with `HttpTestingController`, then `whenStable()` (`whenStable()` does not finish while a request has no response).
- Replace the WebSocket with `FakeWebSockets` (`@vehicle-sil/gui-client/testing`) (`{ provide: SOCKET_FACTORY, useValue: fake.factory }`). The wrapper sends HTTP with `HttpClient`, so answer HTTP with `HttpTestingController`. Use Vitest's fake timers for time.
- Feed the examples in `gui/protocol/examples/` instead of building server notifications (`webSocketExamples`, `httpExamples`).
- The rules for the connection, the session copy, and time and run control are tested in the library's tests (`gui/client/src/*.spec.ts`; no Angular). Wrapper tests check only the conversion to signals, the token, and the screen text.
- Do not check that "something does not happen" by waiting a fixed time (a slow machine misses it). Wait for a later occurrence that shows the time has passed (a later event, `last_seq` or `stop` in the server's summary), then check.

**End-to-end tests**:

- Wait for what should happen with the waiting `expect` or `expect.poll`. Do not sleep a fixed time and assume "it must have moved on".
- Wait limits (`timeout` and `expect.timeout` in the configuration, `RUN_TIMEOUT`) are multiplied by the environment variable `VEHICLE_SIL_TEST_TIMEOUT_SCALE`. Pass a limit you write through `scaled(ms)` (`e2e/support/timeouts.ts`). Do not scale the time used to confirm that something does not happen, or the time given to the screen (such as the length of a guard).
- Use `getByRole` first; `data-testid` is a fallback. For layout, see "Layout" above.
- The tests run against the real GUI server and the replay launcher, which replays recordings of the real launcher's output. The configuration file and the commands sent decide the recording. For an action with no recording, the replay launcher says what to record and exits.
- `gui/server/tests/test_real_launcher.py` compares the recordings with the current real launcher.
- What happens only with the real launcher (the `--describe` output of the example ECUs, how the real launcher stops) is tested in `e2e/real/`.
- Situations that are not in the recordings (disconnection, lines that keep coming while reading) are created by injecting notifications with `page.routeWebSocket`.
- Check appearance and take screenshots at the size people actually see, without enlarging the window (to see what fits on the first screen).

## Icons

`public/favicon.svg` is the source. `public/favicon.ico` (16, 32, 48 px) is made from the SVG with ImageMagick; make it again when the SVG changes.

```sh
cd public && magick -background none -density 384 favicon.svg -define icon:auto-resize=48,32,16 favicon.ico
```

## Dependency install scripts

`ignore-scripts=true` in `.npmrc` keeps the install scripts of dependencies (`preinstall`, `install`, `postinstall`) from running. Every dependency installed now ships its prebuilt binaries as separate packages (`optionalDependencies`), so it works without its scripts. Scripts you run explicitly, such as `npm run build`, do run, but pre and post scripts such as `prebuild` do not. Do not write pre and post scripts; put the steps into one script. When you add a dependency, check that it works without its install scripts.

## See also

| To find                                                                                   | Document                                                                              |
| ----------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| Adding a part to these screens (custom panels), and when to build your own screen instead | "Build your own screen" in [docs/gui.md](../../docs/gui.md#build-your-own-screen)     |
| Building your own screen from scratch                                                     | [Build your own screen for the browser GUI](../../docs/howto/gui-screen.md)           |
| Copying the widgets of these screens into another Angular app                             | "Copy the sample widgets" in [docs/gui.md](../../docs/gui.md#copy-the-sample-widgets) |
| The public API of the client library                                                      | [gui/client](../client/README.md)                                                     |
| The shapes of the server API                                                              | [gui/protocol](../protocol/README.md)                                                 |
