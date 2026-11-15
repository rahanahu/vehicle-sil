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

const PROTOCOL = 1; // このクライアントが読む口の版（api/app と hello の vehicle_sil_gui）

function usage(message) {
  console.error(`run.mjs: ${message}`);
  console.error(
    "usage: VEHICLE_SIL_GUI_URL=<url with #token=...> node run.mjs <configuration file> [--timeout <seconds>]",
  );
  process.exit(2);
}

const args = process.argv.slice(2);
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
// サーバーは、自分自身の origin からの POST と WebSocket だけを受け付ける（ブラウザなら自分で付ける）
const origin = url.origin;

/** HTTP の要求。失敗は、サーバーの誤りの文（直し方を含む）で投げる。 */
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

/** WebSocket でつなぎ、認証して、届いた通知を onMessage に渡す。 */
function connect(onMessage) {
  const wsUrl = new URL("ws", base);
  wsUrl.protocol = wsUrl.protocol === "https:" ? "wss:" : "ws:";
  // Node の WebSocket は、2 つ目の引数で握手のヘッダーを付けられる（ブラウザでは Origin が自動で付く）
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

  let session = null; // 始めた実行の番号（api/run の応答まで null）
  let synced = false;
  const ended = new Map(); // 応答より先に終わりが届いた実行の要約
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
          // 再送を受け終えた: ここから始めると、始めた実行の事象を取りこぼさない
          if (!synced) {
            synced = true;
            resolve(socket);
          }
          break;
        case "event": {
          // 応答の前に届く事象も、始めた実行のもの（サーバーは一度に 1 つしか実行しない）
          if (session !== null && message.session !== session) break;
          const event = message.event;
          if (event.event === "phase") console.log(`phase: ${event.phase}`);
          if (event.event === "participant")
            console.log(`participant ${event.name}: ${event.state}`);
          if (event.event === "finding")
            console.log(`finding: ${event.severity} ${event.message}`);
          // ほかの事象（log、time、知らない事象）は読み飛ばす
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
        // 知らない通知（後の版で足されるもの）は読み飛ばす
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

  const answer = await call("POST", "api/run", { file, disable: [] });
  session = answer.session.id;
  console.log(`started: session ${session}`);
  if (ended.has(session)) end(ended.get(session));
  setTimeout(
    () => finish(1, `no result after ${timeoutS} s`),
    timeoutS * 1000,
  ).unref();
}

main().catch((error) => finish(1, `error: ${error.message}`));
