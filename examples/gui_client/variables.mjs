#!/usr/bin/env node
// The smallest example that reads and writes ECU variables with the client library (gui/client). Plain JavaScript.
// It starts a run paused, writes one variable before the first step, advances virtual time and pauses, reads and
// checks the value at the paused time, then runs to the end. It checks its own result (exit code 0 if as expected).
//
//   (cd gui/web && npm run build:client)   # builds build/gui-client/vehicle-sil-gui-client.mjs
//   VEHICLE_SIL_GUI_URL='<URL the server printed (with #token=)>' node examples/gui_client/variables.mjs simulation.yaml
//
// The defaults suit the 2 ECUs of examples/ecu_main (build/<preset>/examples/ecu_main/simulation.yaml): write 50 to
// engine.throttle of Engine, advance 500 ms and check that engine.speed has reached 1800 (the engine speed moves
// toward 800 rpm + 20 rpm per 1 % of throttle, by 50 rpm every 10 ms). For another configuration file, change
// them with:
//
//   --write <peer> <variable>=<value>    the variable to write (a number, true, false or the name of an enum item)
//   --expect <peer> <variable>=<value>   the variable to check after advancing
//   --step <ms>                          the virtual time to advance (default 500)
//
// Variables are unavailable if the launcher has no Control Server (`vehicle_sil_launch version --json` lists no
// control-server in features) or the GUI server has no gRPC. Then the example prints why and exits with code 1.
//
// Exit codes: 0 everything as expected, 1 a value not as expected, variables unavailable, a server error or a timeout,
// 2 usage error.

import { existsSync } from "node:fs";
import { fileURLToPath, pathToFileURL } from "node:url";

function usage(message) {
  console.error(`variables.mjs: ${message}`);
  console.error(
    "usage: VEHICLE_SIL_GUI_URL=<url with #token=...> node variables.mjs <configuration file> " +
      "[--write <peer> <variable>=<value>] [--expect <peer> <variable>=<value>] [--step <ms>] [--timeout <seconds>]",
  );
  process.exit(2);
}

/** Parses `<peer> <variable>=<value>`. */
function assignment(option, peer, text) {
  const at = text?.indexOf("=") ?? -1;
  if (!peer || at <= 0) usage(`${option} needs <peer> <variable>=<value>`);
  const raw = text.slice(at + 1);
  const value =
    raw === "true"
      ? true
      : raw === "false"
        ? false
        : raw !== "" && !Number.isNaN(Number(raw))
          ? Number(raw)
          : raw;
  return { peer, name: text.slice(0, at), value };
}

const args = process.argv.slice(2);
let write = { peer: "Engine", name: "engine.throttle", value: 50 };
let expected = { peer: "Engine", name: "engine.speed", value: 1800 };
let stepMs = 500;
let timeoutS = 60;
for (const [option, take] of [
  ["--write", (a) => (write = assignment("--write", a[0], a[1]))],
  ["--expect", (a) => (expected = assignment("--expect", a[0], a[1]))],
  ["--step", (a) => (stepMs = Number(a[0]))],
  ["--timeout", (a) => (timeoutS = Number(a[0]))],
]) {
  const at = args.indexOf(option);
  if (at < 0) continue;
  const count = option === "--step" || option === "--timeout" ? 1 : 2;
  take(args.slice(at + 1, at + 1 + count));
  args.splice(at, 1 + count);
}
if (!(Number.isInteger(stepMs) && stepMs > 0))
  usage("--step needs a whole number of milliseconds");
if (!(timeoutS > 0)) usage("--timeout needs a number of seconds");
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
const token = new URLSearchParams(printed.hash.slice(1)).get("token") ?? "";
printed.hash = "";

const client = createClient({ baseUrl: printed.href, token });
const problems = [];

function expect(ok, what) {
  console.log((ok ? "ok   " : "FAIL ") + what);
  if (!ok) problems.push(what);
}

async function finish(code, message) {
  if (message) console.log(message);
  // Stop the run before exiting (never leave it running)
  if (client.session.session.get()?.launcher.running)
    await client.run.forceStop().catch(() => undefined);
  client.close();
  process.exit(code);
}

/** Whether the launcher of the run accepts that time command now (`allowed` of its state event). */
function accepts(session, command) {
  return session?.launcherState?.allowed[command]?.ok === true;
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
  setTimeout(
    () => finish(1, `FAIL not done after ${timeoutS} s`),
    timeoutS * 1000,
  ).unref();
  const app = await client.loadApp();
  if (!app.launcher.usable)
    await finish(
      1,
      `the launcher cannot be used: ${app.launcher.problems.map((p) => p.message).join("; ")}`,
    );
  // Whether variables are available is known before the start
  if (!app.variables.available)
    await finish(1, `no variables: ${app.variables.reason}`);

  client.session.rejected.subscribe(
    (rejected) => rejected && finish(1, "the server refused the token"),
  );
  client.session.incompatible.subscribe((why) => why && finish(1, why));
  client.connect();
  await until(client.session.ready, (ready) => ready);

  // Start paused: a launcher with a Control Server waits before the first step
  const started = await client.run.start({
    file,
    stop_at_ms: 1000,
    disable: [],
    start_paused: true,
  });
  console.log(`started: session ${started?.run_id}`);
  // ready means connected to the Control Server. ECUs join later and the list grows with each one, so wait until
  // the ECU to write is in the list (if it never appears, the overall timeout ends the example)
  const variables = await until(
    client.variables.state,
    (v) =>
      v.session === started?.run_id &&
      v.status !== "unknown" &&
      v.status !== "connecting" &&
      (v.status !== "ready" ||
        v.peers.some((p) => p.name === write.peer && p.variables.length > 0)),
  );
  if (variables.status !== "ready")
    await finish(1, `no variables: ${variables.status}: ${variables.reason}`);
  for (const peer of variables.peers) {
    const writable = peer.variables.filter((v) => v.writable).length;
    console.log(
      `${peer.name}: ${peer.variables.length} variables (${writable} writable)`,
    );
  }
  const target = variables.peers
    .find((p) => p.name === write.peer)
    ?.variables.find((v) => v.name === write.name);
  expect(
    target?.writable === true,
    `${write.peer} has the writable variable ${write.name}`,
  );
  if (!target) await finish(1);

  // Start receiving values (the variable to write and the one to check, with history)
  const watch = client.variables.watch([write, expected], {
    periodMs: 50,
    history: 1000,
  });

  // Write before the start: advancing only after every peer has received it (queued) applies it in the step at time 0
  await until(client.session.session, (s) => accepts(s, 'step'));
  const written = client.variables.write({
    [write.peer]: { [write.name]: write.value },
  });
  written.catch(() => undefined); // awaited below
  let seq = null;
  await Promise.race([
    until(client.variables.operations, (ops) => {
      const last = ops[ops.length - 1];
      seq = last?.seq ?? null;
      return last !== undefined && last.state !== "sent";
    }),
    written, // throws here if refused
  ]);
  console.log(
    `write: operation ${seq} is ${client.variables.operations.get().at(-1)?.state}`,
  );
  expect(client.time.step(stepMs).sent, `a step of ${stepMs} ms was sent`);
  const operation = await written;
  const applied = operation.peers[write.peer];
  expect(
    applied?.appliedAtMs === 0,
    `${write.name} was applied in the step at ${applied?.appliedAtMs} ms`,
  );

  // Values at the paused time: when paused is true, the values of every step before that time have arrived
  await until(watch.values, () => {
    const reading = watch.value(expected);
    return reading?.paused === true && (reading.simMs ?? 0) >= stepMs;
  });
  const reading = watch.value(expected);
  expect(
    reading.value === expected.value,
    `${expected.name} is ${reading.value} at ${reading.simMs} ms, standing still (expected ${expected.value})`,
  );
  const back = watch.value(write);
  // An enum written by item name reads back as its value (a number)
  expect(
    typeof write.value === "string" || back?.value === write.value,
    `${write.name} reads ${back?.value}`,
  );
  const history = watch.history(expected);
  expect(
    history.length >= 1,
    `${history.length} points of ${expected.name} were kept`,
  );
  watch.close();

  // Run to the end
  await until(client.session.session, (s) => accepts(s, 'resume'));
  client.time.resume();
  const ended = await until(
    client.session.session,
    (s) => s?.id === started?.run_id && !s.launcher.running,
  );
  // Without stop_at_ms in the run request, we never get here: the timeout stops it
  expect(
    ended.result?.result === "passed",
    `the run ${ended.result?.result ?? "ended without a result"}`,
  );
  await until(
    client.variables.state,
    (v) => v.status !== "ready" && v.status !== "connecting",
  );
  console.log(`variables: ${client.variables.state.get().status}`);
  await finish(
    problems.length ? 1 : 0,
    problems.length ? `${problems.length} not as expected` : "all as expected",
  );
}

main().catch((error) =>
  finish(
    1,
    `FAIL ${error.code ? `${error.code}: ` : error.problem ? `${error.problem.code}: ` : ""}${errorMessage(error)}`,
  ),
);
