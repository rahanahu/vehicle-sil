#!/usr/bin/env node
// The smallest client that calls the API of the GUI server (vehicle-sil-gui) without the screens. Plain JavaScript with
// no dependencies (only the standard fetch and WebSocket of Node 22 or later). It checks a configuration file, starts a
// run, reads the events until the end and prints the result. The API: gui/protocol/README.md.
//
//   VEHICLE_SIL_GUI_URL='<URL the server printed (with #token=)>' node examples/gui_client/run.mjs <configuration file (relative to the root)>
//
// Pass the URL in the environment variable (on the command line, the token would show in the process list to other
// users of the computer and stay in the shell history). The URL may also be the first argument. The same example with
// the client library: run-with-library.mjs.
//
// Exit codes: 0 the result is passed, 1 failed (check errors, a failed run, a server error, a timeout), 2 usage error.

const PROTOCOL = 2; // the API version this client reads (vehicle_sil_gui in api/app and hello)

function usage(message) {
  console.error(`run.mjs: ${message}`);
  console.error(
    "usage: VEHICLE_SIL_GUI_URL=<url with #token=...> node run.mjs <configuration file> [--timeout <seconds>] [--stop-at-ms <milliseconds>]",
  );
  process.exit(2);
}

const args = process.argv.slice(2);
let stopAtMs;
const stopAt = args.indexOf("--stop-at-ms");
if (stopAt >= 0) {
  const value = args[stopAt + 1];
  stopAtMs = Number(value);
  if (!value || !Number.isSafeInteger(stopAtMs) || stopAtMs <= 0)
    usage("--stop-at-ms needs a positive integer of milliseconds");
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
if (typeof WebSocket === "undefined" || typeof fetch === "undefined") {
  usage(`Node ${process.versions.node} has no WebSocket: use Node 22 or later`);
}

const [file] = args;
const url = new URL(process.env.VEHICLE_SIL_GUI_URL);
const token = new URLSearchParams(url.hash.slice(1)).get("token");
if (!token) usage("the URL has no #token=...: use the URL the server printed");
url.hash = "";
const base = url.href; // http://127.0.0.1:<port>/
// The server accepts POST and WebSocket only from its own origin (a browser sends it by itself)
const origin = url.origin;

/** Sends an HTTP request. On failure, throws with the server's error message (which says how to fix it). */
async function call(method, path, body) {
  const headers = { Authorization: `Bearer ${token}` };
  if (method === "POST")
    Object.assign(headers, {
      "Content-Type": "application/json",
      Origin: origin,
    });
  const response = await fetch(new URL(path, base), {
    method,
    headers,
    body: method === "POST" ? JSON.stringify(body ?? {}) : undefined,
  });
  const answer = await response.json().catch(() => null);
  if (!response.ok) {
    const error = answer?.error;
    throw new Error(
      error ? `${error.code}: ${error.message}` : `HTTP ${response.status}`,
    );
  }
  return answer;
}

/** Connects over WebSocket, authenticates and passes each incoming message to onMessage. */
function connect(onMessage) {
  const wsUrl = new URL("ws", base);
  wsUrl.protocol = wsUrl.protocol === "https:" ? "wss:" : "ws:";
  // Node's WebSocket takes handshake headers in the second argument (a browser adds Origin by itself)
  const socket = new WebSocket(wsUrl, { headers: { Origin: origin } });
  socket.onopen = () => socket.send(JSON.stringify({ type: "auth", token }));
  socket.onmessage = (event) => onMessage(JSON.parse(event.data));
  return socket;
}

function finish(code, message) {
  if (message) console.log(message);
  process.exit(code);
}

async function main() {
  const app = await call("GET", "api/app");
  if (app.vehicle_sil_gui !== PROTOCOL) {
    finish(
      1,
      `the server speaks version ${app.vehicle_sil_gui} of the GUI protocol; this client reads ${PROTOCOL}`,
    );
  }
  if (!app.launcher.usable) {
    finish(
      1,
      `the launcher cannot be used: ${app.launcher.problems.map((p) => p.message).join("; ")}`,
    );
  }

  const { check } = await call("POST", "api/check", { file, disable: [] });
  console.log(`check: ${check.result} (errors ${check.errors ?? 0})`);
  if (check.result !== "passed") finish(1);

  let session = null; // the session ID of the run we started (null until api/run answers)
  let synced = false;
  const ended = new Map(); // summaries of runs whose end arrived before the answer
  const end = (summary) => {
    const result =
      summary.result?.result ?? "none (the launcher ended without a result)";
    finish(result === "passed" ? 0 : 1, `result: ${result}`);
  };
  const started = new Promise((resolve, reject) => {
    const socket = connect((message) => {
      switch (message.type) {
        case "hello":
          if (message.vehicle_sil_gui !== PROTOCOL)
            reject(new Error("unknown protocol version in hello"));
          break;
        case "synced":
          // The replay is complete: starting from here, no event of the run we start is missed
          if (!synced) {
            synced = true;
            resolve(socket);
          }
          break;
        case "event": {
          // Events that arrive before the answer belong to our run too (the server runs one at a time)
          if (session !== null && message.session !== session) break;
          const event = message.event;
          if (event.event === "phase") console.log(`phase: ${event.phase}`);
          if (event.event === "participant")
            console.log(`participant ${event.name}: ${event.state}`);
          if (event.event === "finding")
            console.log(`finding: ${event.severity} ${event.message}`);
          // Skip other events (log, time, unknown ones)
          break;
        }
        case "session": {
          const summary = message.session;
          if (summary.state !== "ended") break;
          if (summary.id === session) end(summary);
          else ended.set(summary.id, summary);
          break;
        }
        case "error":
          console.log(`server: ${message.code}: ${message.message}`);
          if (message.code === "unauthorized") finish(1);
          break;
        // Skip unknown messages (added by later versions)
      }
    });
    socket.onclose = (event) => {
      if (!synced)
        reject(
          new Error(`the WebSocket closed (${event.code} ${event.reason})`),
        );
      else
        finish(
          1,
          `the WebSocket closed during the run (${event.code} ${event.reason})`,
        );
    };
  });
  await started;

  const answer = await call("POST", "api/run", { file, disable: [], ...(stopAtMs === undefined ? {} : { stop_at_ms: stopAtMs }) });
  session = answer.session.id;
  console.log(`started: session ${session}`);
  if (ended.has(session)) end(ended.get(session));
  setTimeout(
    () => finish(1, `no result after ${timeoutS} s`),
    timeoutS * 1000,
  ).unref();
}

main().catch((error) => finish(1, `error: ${error.message}`));
