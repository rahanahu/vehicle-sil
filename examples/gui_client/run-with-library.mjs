#!/usr/bin/env node
// The smallest client of the GUI server (vehicle-sil-gui) that uses the client library (gui/client). Plain JavaScript
// that loads the library bundled into one file (ESM). It does what run.mjs does, but leaves connecting, reconnecting
// and applying the summary and events to the library: it checks, starts a run, waits for the end while it reads the
// state changes, and prints the result.
//
//   (cd gui/web && npm run build:client)   # builds build/gui-client/vehicle-sil-gui-client.mjs
//   VEHICLE_SIL_GUI_URL='<URL the server printed (with #token=)>' node examples/gui_client/run-with-library.mjs simulation.yaml --stop-at-ms 1000
//
// Pass the URL in the environment variable (on the command line, the token would show in the process list to other
// users of the computer and stay in the shell history). The URL may also be the first argument. VEHICLE_SIL_GUI_CLIENT
// overrides where the library file is.
//
// Exit codes: 0 the result is passed, 1 failed (check errors, a failed run, a server error, a timeout), 2 usage error.

import { existsSync } from "node:fs";
import { fileURLToPath, pathToFileURL } from "node:url";

function usage(message) {
  console.error(`run-with-library.mjs: ${message}`);
  console.error(
    "usage: VEHICLE_SIL_GUI_URL=<url with #token=...> node run-with-library.mjs <configuration file> [--timeout <seconds>] [--stop-at-ms <milliseconds>]",
  );
  process.exit(2);
}

/** How long to wait for the connection, hello and the replay to finish (seconds). */
const READY_TIMEOUT_S = 15;

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
const { createClient, errorMessage } = await import(
  pathToFileURL(library).href
);

const [file] = args;
const printed = new URL(process.env.VEHICLE_SIL_GUI_URL);
const token = new URLSearchParams(printed.hash.slice(1)).get("token");
if (!token) usage("the URL has no #token=...: use the URL the server printed");
printed.hash = "";

// In Node, the library sends the server's own Origin
const client = createClient({ baseUrl: printed.href, token });

function finish(code, message) {
  if (message) console.log(message);
  client.close();
  process.exit(code);
}

/** Waits until a readable value meets the condition. */
function until(readable, condition) {
  return new Promise((resolve) => {
    if (condition(readable.get())) return resolve(readable.get());
    const stop = readable.subscribe((value) => {
      if (condition(value)) {
        stop();
        resolve(value);
      }
    });
  });
}

async function main() {
  const app = await client.loadApp(); // fails on another protocol version (ProtocolError)
  if (!app.launcher.usable) {
    finish(
      1,
      `the launcher cannot be used: ${app.launcher.problems.map((p) => p.message).join("; ")}`,
    );
  }
  const intent = { file, disable: [], ...(stopAtMs === undefined ? {} : { stop_at_ms: stopAtMs }) };
  const check = await client.check(intent);
  console.log(`check: ${check.result} (errors ${check.errors ?? 0})`);
  if (check.result !== "passed") finish(1);

  client.session.rejected.subscribe(
    (rejected) => rejected && finish(1, "the server refused the token"),
  );
  client.session.incompatible.subscribe((why) => why && finish(1, why));
  client.connect();
  // Start only once synced, so no event of the run we start is missed. If it cannot connect, say why and exit
  const connecting = setTimeout(() => {
    const close = client.connection.lastClose.get();
    finish(
      1,
      `the WebSocket did not get in step within ${READY_TIMEOUT_S} s ` +
        `(${client.connection.failures.get()} handshakes refused or lost` +
        (close ? `, last closed with ${close.code} ${close.reason}` : "") +
        "): is the URL the one the server printed, and is the server running?",
    );
  }, READY_TIMEOUT_S * 1000);
  await until(client.session.ready, (ready) => ready);
  clearTimeout(connecting);

  let phase = null;
  client.session.session.subscribe((session) => {
    if (session && session.phase !== phase) {
      phase = session.phase;
      console.log(`phase: ${phase}`);
    }
  });
  const started = await client.run.start(intent);
  console.log(`started: session ${started?.run_id}`);
  setTimeout(
    () => finish(1, `no result after ${timeoutS} s`),
    timeoutS * 1000,
  ).unref();

  // The launcher has ended (with or without a result)
  const ended = await until(
    client.session.session,
    (s) => s?.id === started?.run_id && !s.launcher.running,
  );
  for (const finding of ended.findings)
    console.log(`finding: ${finding.severity} ${finding.message}`);
  const result =
    ended.result?.result ?? "none (the launcher ended without a result)";
  finish(result === "passed" ? 0 : 1, `result: ${result}`);
}

// A server error carries a code for machines and a message for people (including how to fix it)
main().catch((error) =>
  finish(
    1,
    `error: ${error.problem ? `${error.problem.code}: ` : ""}${errorMessage(error)}`,
  ),
);
