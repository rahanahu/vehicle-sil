#!/usr/bin/env node
// ECU の変数を、クライアントのライブラリ（gui/client）で読み書きする最小の例。素の JavaScript。
// 止めたまま実行を始め、最初のステップの前に変数を 1 つ書き、仮想時間を進めて止め、止まっている時刻の値を読んで
// 確かめ、最後まで走らせる。自分で結果を確かめる（期待どおりなら終了コード 0）。
//
//   (cd gui/web && npm run build:client)   # build/gui-client/vehicle-sil-gui-client.mjs を作る
//   VEHICLE_SIL_GUI_URL='<サーバーが表示した URL（#token= つき）>' node examples/gui_client/variables.mjs simulation.yaml
//
// 既定は examples/ecu_main の 2 つの ECU 用（build/<preset>/examples/ecu_main/simulation.yaml）: Engine の
// engine.throttle に 50 を書き、500 ms 進めて、engine.speed が 1800 になったことを確かめる（エンジンの回転数は
// 800 rpm + スロットル 1 % あたり 20 rpm へ、10 ms に 50 rpm ずつ近づく）。ほかの構成ファイルでは、次で変える。
//
//   --write <peer> <変数>=<値>    書く変数（値は数、true、false、列挙の項目の名前）
//   --expect <peer> <変数>=<値>   進めた後に確かめる変数
//   --step <ms>                    進める仮想時間（既定 500）
//
// ランチャーが Control Server を持たない（`vehicle_sil_launch version --json` の features に control-server が
// ない）か、GUI サーバーに gRPC がなければ、変数は使えない。そのときは理由を表示して、終了コード 1。
//
// 終了コード: 0 は全部が期待どおり、1 は期待と違う・変数が使えない・サーバーの誤り・時間切れ、2 は使い方の誤り。

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

/** `<peer> <変数>=<値>` を読む。 */
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
const token = new URLSearchParams(printed.hash.slice(1)).get("token");
if (!token) usage("the URL has no #token=...: use the URL the server printed");
printed.hash = "";

const client = createClient({ baseUrl: printed.href, token });
const problems = [];

function expect(ok, what) {
  console.log((ok ? "ok   " : "FAIL ") + what);
  if (!ok) problems.push(what);
}

async function finish(code, message) {
  if (message) console.log(message);
  // 止めて終わる（動いたままにしない）
  if (client.session.session.get()?.launcher.running)
    await client.run.forceStop().catch(() => undefined);
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
  // 変数が使えるかは、始める前にわかる
  if (!app.variables.available)
    await finish(1, `no variables: ${app.variables.reason}`);

  client.session.rejected.subscribe(
    (rejected) => rejected && finish(1, "the server refused the token"),
  );
  client.session.incompatible.subscribe((why) => why && finish(1, why));
  client.connect();
  await until(client.session.ready, (ready) => ready);

  // 止めたまま始める: Control Server のあるランチャーは、最初のステップの前で待つ
  const started = await client.run.start({
    file,
    disable: [],
    speed: null,
    start_paused: true,
  });
  console.log(`started: session ${started?.id}`);
  // ready は Control Server につながったところ。ECU は後から加わり、そのたびに一覧が増えるので、書く ECU が
  // 一覧に入るまで待つ（入らなければ、全体の時間切れで終わる）
  const variables = await until(
    client.variables.state,
    (v) =>
      v.session === started?.id &&
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

  // 値を受け取り始める（書く変数と確かめる変数。履歴つき）
  const watch = client.variables.watch([write, expected], {
    periodMs: 50,
    history: 1000,
  });

  // 始まる前に書く: 全員が受け取った（queued）のを見てから進めると、時刻 0 のステップで反映される
  await until(client.time.state, (t) => t.canStep);
  const written = client.variables.write({
    [write.peer]: { [write.name]: write.value },
  });
  written.catch(() => undefined); // 下で待つ
  let seq = null;
  await Promise.race([
    until(client.variables.operations, (ops) => {
      const last = ops[ops.length - 1];
      seq = last?.seq ?? null;
      return last !== undefined && last.state !== "sent";
    }),
    written, // 断られたら、ここで投げる
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

  // 止まっている時刻の値: paused が true なら、その時刻より前のステップの値は全部入っている
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
  // 列挙を項目の名前で書いたときは、値（数）で読める
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

  // 最後まで走らせる
  await until(client.time.state, (t) => t.canResume);
  client.time.resume();
  const ended = await until(
    client.session.session,
    (s) => s?.id === started?.id && !s.launcher.running,
  );
  // 構成ファイルに終わり（stop_at_ms）がなければ、ここまで来ない: 時間切れで止める
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
