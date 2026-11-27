#!/usr/bin/env node
// The smallest client of the GUI server (vehicle-sil-gui) that uses the front door of the client library
// (gui/client: connect(), status, controls, Promise operations). Plain JavaScript that loads the library bundled into
// one file (ESM). It starts a run, prints every change of the run's state, and prints the result and its sentence.
// Compare run-with-library.mjs, which does the same with the low client (createClient): the front door takes the
// "which state is this, and what may I press now" work off the screen.
//
//   (cd gui/web && npm run build:client)   # builds build/gui-client/vehicle-sil-gui-client.mjs
//   VEHICLE_SIL_GUI_URL='<URL the server printed (with #token=)>' node examples/gui_client/front.mjs simulation.yaml --stop-at-ms 1000
//
// Pass the URL in the environment variable (on the command line, the token would show in the process list to other
// users of the computer and stay in the shell history). The URL may also be the first argument. VEHICLE_SIL_GUI_CLIENT
// overrides where the library file is.
//
// Exit codes: 0 the run succeeded (result.ok), 1 it did not (a failed run, a refused start, a timeout), 2 usage error.

import { existsSync } from "node:fs";
import { fileURLToPath, pathToFileURL } from "node:url";

function usage(message) {
  console.error(`front.mjs: ${message}`);
  console.error(
    "usage: VEHICLE_SIL_GUI_URL=<url with #token=...> node front.mjs <configuration file> [--timeout <seconds>] [--stop-at-ms <milliseconds>]",
  );
  process.exit(2);
}

const args = process.argv.slice(2);
let stopAtMs;
const stopAt = args.indexOf("--stop-at-ms");
if (stopAt >= 0) {
  const value = args[stopAt + 1];
  stopAtMs = Number(value);
  if (!value || !Number.isSafeInteger(stopAtMs) || stopAtMs < 1 || stopAtMs > 1_000_000_000_000)
    usage("--stop-at-ms needs an integer from 1 to 1000000000000 milliseconds");
  args.splice(stopAt, 2);
}
let timeoutS = 120;
const timeoutAt = args.indexOf("--timeout");
if (timeoutAt >= 0) {
  timeoutS = Number(args[timeoutAt + 1]);
  if (!(timeoutS > 0)) usage("--timeout needs a number of seconds");
  args.splice(timeoutAt, 2);
}
if (args.length === 2) process.env.VEHICLE_SIL_GUI_URL = args.shift();
if (args.length !== 1)
  usage("give a configuration file (and the URL in VEHICLE_SIL_GUI_URL)");
if (!process.env.VEHICLE_SIL_GUI_URL)
  usage("set VEHICLE_SIL_GUI_URL to the URL the server printed");
if (typeof WebSocket === "undefined")
  usage(`Node ${process.versions.node} has no WebSocket: use Node 22 or later`);

const library =
  process.env.VEHICLE_SIL_GUI_CLIENT ??
  fileURLToPath(
    new URL(
      "../../build/gui-client/vehicle-sil-gui-client.mjs",
      import.meta.url,
    ),
  );
if (!existsSync(library))
  usage(`no ${library}: build it with "npm run build:client" in gui/web`);
const { ActionError, connect, until, watch } = await import(
  pathToFileURL(library).href
);

const [file] = args;
const printed = new URL(process.env.VEHICLE_SIL_GUI_URL);
const token = new URLSearchParams(printed.hash.slice(1)).get("token") ?? "";
printed.hash = "";

let front = null;

function finish(code, message) {
  if (message) console.log(message);
  front?.close();
  process.exit(code);
}

async function main() {
  // In Node, pass the server's URL and token. In a browser page the server served, connect() needs no options
  front = await connect({ baseUrl: printed.href, token });

  // The state of the run, one line per change. watch() calls now and on every change; the return value stops it
  watch(front.status, (status) => {
    console.log(
      `status: link ${status.link}, run ${status.run}` +
        (status.problem ? `, problem ${status.problem.code}` : ""),
    );
  });

  // Wait until the connection is in step with the server: the controls are those of the run, not a guess
  await until(front.status, (status) => status.link === "online", {
    timeoutMs: 15_000,
  });

  // controls say, before the call, whether a button would work (and why not, in the server's words)
  const { start } = front.controls.get();
  if (!start.ok) finish(1, `cannot start: ${start.reason.code}: ${start.reason.message}`);

  await front.run.start(stopAtMs === undefined ? {} : { stop_at_ms: stopAtMs }, { file });
  // start() resolves when the server took the request; whether the run began is status.run. Wait for the end of the run
  // (ended, or crashed when the launcher died without a result)
  const status = await until(
    front.status,
    (s) => s.run === "ended" || s.run === "crashed",
    { timeoutMs: timeoutS * 1000 },
  );
  const { result } = status;
  finish(
    result.ok ? 0 : 1,
    `result: ${result.end}, ok ${result.ok}: ${result.message}`,
  );
}

// An ActionError carries a code for machines, a sentence for people, and whether the thing may have been done
main().catch((error) => {
  if (error instanceof ActionError) {
    const findings = error.findings.map(
      (f) => `\nfinding: ${f.severity} ${f.message}`,
    );
    finish(
      1,
      `error: ${error.code} (${error.outcome}): ${error.message}${findings.join("")}`,
    );
  }
  finish(1, `error: ${error instanceof Error ? error.message : error}`);
});
