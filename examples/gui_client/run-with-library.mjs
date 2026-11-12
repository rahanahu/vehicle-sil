#!/usr/bin/env node
// GUI サーバー（vehicle-sil-gui）を、クライアントのライブラリ（gui/client）で使う最小の例。素の JavaScript で、
// ライブラリを 1 つにまとめたファイル（ESM）を読む。run.mjs と同じことを、接続、再接続、要約と事象の当て方を
// ライブラリに任せて行う: 照合して実行を始め、状態の変化を読みながら終わりを待ち、結果を表示する。
//
//   (cd gui/web && npm run build:client)   # build/gui-client/vehicle-sil-gui-client.mjs を作る
//   VEHICLE_SIL_GUI_URL='<サーバーが表示した URL（#token= つき）>' node examples/gui_client/run-with-library.mjs simulation.yaml
//
// URL は環境変数で渡す（コマンドの引数に書くと、トークンが同じ PC のほかの利用者のプロセスの一覧と、シェルの
// 履歴に残るため）。引数の先頭に URL を書いてもよい。ライブラリのファイルの場所は VEHICLE_SIL_GUI_CLIENT で変えられる。
//
// 終了コード: 0 は結果が passed、1 は失敗（照合の誤り、実行の失敗、サーバーの誤り、時間切れ）、2 は使い方の誤り。

import { existsSync } from "node:fs";
import { fileURLToPath, pathToFileURL } from "node:url";

function usage(message) {
  console.error(`run-with-library.mjs: ${message}`);
  console.error(
    "usage: VEHICLE_SIL_GUI_URL=<url with #token=...> node run-with-library.mjs <configuration file> [--timeout <seconds>]",
  );
  process.exit(2);
}

/** つないで、hello と再送を受け終えるまで待つ時間（秒）。 */
const READY_TIMEOUT_S = 15;

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

// Node では、ライブラリがサーバー自身の Origin を付ける
const client = createClient({ baseUrl: printed.href, token });

function finish(code, message) {
  if (message) console.log(message);
  client.close();
  process.exit(code);
}

/** 読める値が条件を満たすまで待つ。 */
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
  const app = await client.loadApp(); // 版が違えば読めない（ProtocolError）
  if (!app.launcher.usable) {
    finish(
      1,
      `the launcher cannot be used: ${app.launcher.problems.map((p) => p.message).join("; ")}`,
    );
  }
  const check = await client.check({ file, disable: [] });
  console.log(`check: ${check.result} (errors ${check.errors ?? 0})`);
  if (check.result !== "passed") finish(1);

  client.session.rejected.subscribe(
    (rejected) => rejected && finish(1, "the server refused the token"),
  );
  client.session.incompatible.subscribe((why) => why && finish(1, why));
  client.connect();
  // 揃ってから始める（始めた実行の事象を取りこぼさない）。つながらなければ、わけを言って終わる
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
  const started = await client.run.start({
    file,
    disable: [],
    speed: null,
    start_paused: false,
  });
  console.log(`started: session ${started?.id}`);
  setTimeout(
    () => finish(1, `no result after ${timeoutS} s`),
    timeoutS * 1000,
  ).unref();

  // ランチャーが終わった（結果を出したか、出さずに終わった）
  const ended = await until(
    client.session.session,
    (s) => s?.id === started?.id && !s.launcher.running,
  );
  for (const finding of ended.findings)
    console.log(`finding: ${finding.severity} ${finding.message}`);
  const result =
    ended.result?.result ?? "none (the launcher ended without a result)";
  finish(result === "passed" ? 0 : 1, `result: ${result}`);
}

// サーバーの誤りは、機械が読む code と、人向けの文（直し方を含む）
main().catch((error) =>
  finish(
    1,
    `error: ${error.problem ? `${error.problem.code}: ` : ""}${errorMessage(error)}`,
  ),
);
