#!/usr/bin/env node
// Raw HTTP + WebSocket example (Node 22+, zero external dependencies) showing the external-caller contracts for:
//   1. Extracting BOTH `{instance, run_id}` from `POST api/run` and sending both on `POST api/time` and
//      `POST api/stop-session`.
//   2. Attaching a numeric `id` to `POST api/time`, distinguishing HTTP 200 (command forwarded to launcher stdin)
//      from the asynchronous WebSocket `command` event (`accepted: true` vs `accepted: false` with `reason`/`message`),
//      and aborting pending waits cleanly on run end or timeout.
//   3. Reconnecting WebSocket with `{type: "auth", token, instance, run_id, after_stream_seq}` while respecting
//      the `hello` -> replay -> `synced` `stream_seq` rules.
//
// Usage:
//   VEHICLE_SIL_GUI_URL='<URL the server printed (with #token=)>' node examples/gui_client/time-and-stop.mjs <configuration file> [--step <ms>] [--timeout <seconds>]
//
// Use a configuration that keeps running until stopped (such as `simulation.live.yaml` or `forever.yaml`).
// Exit codes: 0 passed, 1 failed, 2 usage error.

const PROTOCOL = 4; // GUI protocol version (`vehicle_sil_gui`)
const STEP_MS_MAX = 1_000_000_000_000;

function usage(message) {
  console.error(`time-and-stop.mjs: ${message}`);
  console.error(
    "usage: VEHICLE_SIL_GUI_URL=<url with #token=...> node time-and-stop.mjs <configuration file> [--step <ms>] [--timeout <seconds>]",
  );
  process.exit(2);
}

const args = process.argv.slice(2);
let stepMs = 100;
const stepAt = args.indexOf("--step");
if (stepAt >= 0) {
  const value = args[stepAt + 1];
  stepMs = Number(value);
  if (!value || !Number.isSafeInteger(stepMs) || stepMs < 1 || stepMs > STEP_MS_MAX)
    usage("--step needs an integer from 1 to 1000000000000 milliseconds");
  args.splice(stepAt, 2);
}
let timeoutS = 60;
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
const token = new URLSearchParams(url.hash.slice(1)).get("token") ?? "";
url.hash = "";
const base = url.href;
const origin = url.origin;

/** Sends an HTTP request with bearer auth (if configured) and Origin on POST. */
async function call(method, path, body) {
  const headers = token ? { Authorization: `Bearer ${token}` } : {};
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

class GuiStream {
  constructor() {
    this.socket = null;
    this.intentionalClose = false;
    this.inReplay = false;
    this.lastStreamSeq = 0;
    this.session = null;
    this.launcherStates = new Map(); // run_id -> latest launcher_state | null (scoped per run)
    this.endedSessions = new Map(); // run_id -> ended session summary (handles session-ended before POST api/run resolves)
    this.seenCommands = new Map(); // `${run_id}:${id}` -> command event (handles WS before HTTP 200 resolves)
    this.pendingCommands = new Map(); // `${run_id}:${id}` -> { runId, resolve, reject, timer }
    this.waiters = new Set(); // { check, reject, timer }
  }

  launcherStateFor(runId) {
    return this.launcherStates.get(runId) ?? null;
  }

  /** Connects to /ws, optionally resuming from `(instance, run_id, after_stream_seq)`, and waits for `synced`. */
  connect(resume = null) {
    return new Promise((resolve, reject) => {
      const wsUrl = new URL("ws", base);
      wsUrl.protocol = wsUrl.protocol === "https:" ? "wss:" : "ws:";
      const socket = new WebSocket(wsUrl, { headers: { Origin: origin } });
      this.socket = socket;
      this.intentionalClose = false;
      this.inReplay = false;

      let helloMsg = null;
      let replayed = 0;
      let lastReplaySeq = null;
      let settled = false;

      const failHandshake = (err) => {
        if (!settled) {
          settled = true;
          reject(err);
        }
      };

      socket.onopen = () => {
        // Under --no-auth, the server sends hello/synced immediately without waiting for auth.
        if (!token) return;
        const auth = { type: "auth", token };
        if (resume) {
          auth.instance = resume.instance;
          auth.run_id = resume.run_id;
          auth.after_stream_seq = resume.after_stream_seq;
        }
        socket.send(JSON.stringify(auth));
      };

      socket.onmessage = (raw) => {
        const msg = JSON.parse(raw.data);
        switch (msg.type) {
          case "hello":
            if (msg.vehicle_sil_gui !== PROTOCOL) {
              failHandshake(
                new Error(
                  `protocol mismatch: server=${msg.vehicle_sil_gui}, expected=${PROTOCOL}`,
                ),
              );
              return;
            }
            helloMsg = msg;
            this.inReplay = true;
            lastReplaySeq = null;
            // hello.stream_seq is the current server head; replayed events before `synced`
            // carry their older historical stream_seq values and must NOT be rejected for being < hello.stream_seq.
            this.lastStreamSeq = Math.max(this.lastStreamSeq, msg.stream_seq);
            if (msg.session) this._applySession(msg.session);
            break;

          case "dropped":
            // Note: `dropped` intentionally carries no `stream_seq`.
            break;

          case "synced":
            this.inReplay = false;
            this.lastStreamSeq = Math.max(this.lastStreamSeq, msg.stream_seq);
            if (!settled) {
              settled = true;
              resolve({ hello: helloMsg, synced: msg, replayed });
            }
            this._notifyWaiters();
            break;

          case "event": {
            if (this.inReplay) {
              // Within replay, historical stream_seq increases monotonically.
              if (lastReplaySeq !== null && msg.stream_seq <= lastReplaySeq) {
                failHandshake(
                  new Error(
                    `non-monotonic replay stream_seq: ${msg.stream_seq} <= ${lastReplaySeq}`,
                  ),
                );
                return;
              }
              lastReplaySeq = msg.stream_seq;
              replayed += 1;
            } else {
              this.lastStreamSeq = Math.max(this.lastStreamSeq, msg.stream_seq);
            }
            const ev = msg.event;
            if (ev.event === "state") {
              this.launcherStates.set(msg.run_id, ev);
            } else if (ev.event === "command" && typeof ev.id === "number") {
              this._resolveCommand(msg.run_id, ev);
            }
            this._notifyWaiters();
            break;
          }

          case "session":
            if (!this.inReplay)
              this.lastStreamSeq = Math.max(this.lastStreamSeq, msg.stream_seq);
            this._applySession(msg.session);
            this._notifyWaiters();
            break;

          case "error":
            if (typeof msg.stream_seq === "number" && !this.inReplay)
              this.lastStreamSeq = Math.max(this.lastStreamSeq, msg.stream_seq);
            if (msg.code === "unauthorized") {
              failHandshake(new Error(`${msg.code}: ${msg.message}`));
            }
            break;

          default:
            if (typeof msg.stream_seq === "number" && !this.inReplay)
              this.lastStreamSeq = Math.max(this.lastStreamSeq, msg.stream_seq);
            break;
        }
      };

      socket.onclose = (ev) => {
        if (!settled) {
          failHandshake(
            new Error(`WebSocket closed before synced (${ev.code} ${ev.reason})`),
          );
        } else if (!this.intentionalClose) {
          this._rejectAllPending(
            new Error(`WebSocket closed unexpectedly (${ev.code} ${ev.reason})`),
          );
        }
      };
    });
  }

  /** Closes the current WebSocket cleanly (for demonstrating reconnect or on shutdown). */
  close() {
    if (!this.socket) return Promise.resolve();
    const sock = this.socket;
    this.socket = null;
    this.intentionalClose = true;
    if (sock.readyState === WebSocket.CLOSED) return Promise.resolve();
    return new Promise((resolve) => {
      sock.addEventListener("close", () => resolve(), { once: true });
      sock.close(1000, "client-close");
    });
  }

  _applySession(summary) {
    this.session = summary;
    if (summary.launcher_state !== null || !this.launcherStates.has(summary.id)) {
      this.launcherStates.set(summary.id, summary.launcher_state ?? null);
    }
    if (summary.state === "ended") {
      this.endedSessions.set(summary.id, summary);
      // Terminate pending command waits for this run so a run ending after HTTP 200 never hangs.
      this._rejectPendingForRun(
        summary.id,
        new Error(`run ${summary.id} ended before command event arrived`),
      );
    }
  }

  _resolveCommand(runId, ev) {
    const key = `${runId}:${ev.id}`;
    const waiter = this.pendingCommands.get(key);
    if (waiter) {
      this.pendingCommands.delete(key);
      clearTimeout(waiter.timer);
      waiter.resolve(ev);
    } else {
      this.seenCommands.set(key, ev);
    }
  }

  _rejectPendingForRun(runId, err) {
    for (const [key, waiter] of this.pendingCommands) {
      if (waiter.runId !== runId) continue;
      clearTimeout(waiter.timer);
      waiter.reject(err);
      this.pendingCommands.delete(key);
    }
    this._notifyWaiters();
  }

  _rejectAllPending(err) {
    for (const [key, waiter] of this.pendingCommands) {
      clearTimeout(waiter.timer);
      waiter.reject(err);
      this.pendingCommands.delete(key);
    }
    for (const waiter of [...this.waiters]) {
      clearTimeout(waiter.timer);
      this.waiters.delete(waiter);
      waiter.reject(err);
    }
  }

  _notifyWaiters() {
    for (const waiter of [...this.waiters]) waiter.check();
  }

  waitUntil(predicate, label, waitMs = 15_000) {
    if (predicate()) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const waiter = {
        reject,
        timer: null,
        check: () => {
          if (predicate()) {
            clearTimeout(waiter.timer);
            this.waiters.delete(waiter);
            resolve();
          }
        },
      };
      waiter.timer = setTimeout(() => {
        this.waiters.delete(waiter);
        reject(new Error(`timed out waiting for ${label}`));
      }, waitMs);
      waiter.timer.unref();
      this.waiters.add(waiter);
    });
  }

  /** Registers a waiter for WS `event.event === "command"` matching `(runId, id)`. */
  waitForCommand(runId, id, waitMs = 10_000) {
    const key = `${runId}:${id}`;
    if (this.seenCommands.has(key)) {
      const ev = this.seenCommands.get(key);
      this.seenCommands.delete(key);
      return Promise.resolve(ev);
    }
    if (this.endedSessions.has(runId)) {
      return Promise.reject(
        new Error(`run ${runId} already ended before command id=${id}`),
      );
    }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingCommands.delete(key);
        reject(new Error(`timed out waiting for command event id=${id}`));
      }, waitMs);
      timer.unref();
      this.pendingCommands.set(key, { runId, resolve, reject, timer });
    });
  }

  cancelCommandWait(runId, id) {
    const key = `${runId}:${id}`;
    const waiter = this.pendingCommands.get(key);
    if (waiter) {
      clearTimeout(waiter.timer);
      this.pendingCommands.delete(key);
    }
  }
}

let activeTarget = null;
let stream = null;

async function finish(code, message) {
  if (message) console.log(message);
  // Never leave a started run running on error/timeout
  if (activeTarget && !stream?.endedSessions.has(activeTarget.run_id)) {
    await call("POST", "api/stop-session", {
      ...activeTarget,
      force: true,
    }).catch(() => undefined);
  }
  if (stream) await stream.close().catch(() => undefined);
  process.exit(code);
}

async function main() {
  const deadline = setTimeout(
    () => void finish(1, `error: timed out after ${timeoutS} s`),
    timeoutS * 1000,
  );
  deadline.unref();

  const app = await call("GET", "api/app");
  if (app.vehicle_sil_gui !== PROTOCOL) {
    await finish(
      1,
      `error: protocol mismatch (server=${app.vehicle_sil_gui}, expected=${PROTOCOL})`,
    );
  }
  if (!app.launcher.usable) {
    await finish(
      1,
      `error: launcher unusable: ${app.launcher.problems.map((p) => p.message).join("; ")}`,
    );
  }

  const intent = { file, disable: [], start_paused: true };
  const { check } = await call("POST", "api/check", intent);
  console.log(`check: ${check.result} (errors ${check.errors ?? 0})`);
  if (check.result !== "passed") await finish(1);

  stream = new GuiStream();
  await stream.connect();

  // 1. Start the run and capture BOTH `instance` and `run_id`
  const started = await call("POST", "api/run", intent);
  if (!started.instance || !started.run_id) {
    throw new Error("POST api/run did not return both instance and run_id");
  }
  const target = { instance: started.instance, run_id: started.run_id };
  activeTarget = target;
  console.log(`started: instance=${target.instance} run_id=${target.run_id}`);

  // Wait for setup to finish and the launcher to report `step` allowed (holding paused at t=0)
  await stream.waitUntil(
    () =>
      stream.endedSessions.has(target.run_id) ||
      stream.launcherStateFor(target.run_id)?.allowed?.step?.ok === true,
    "launcher paused and ready for step",
  );
  if (stream.endedSessions.has(target.run_id)) {
    throw new Error("run ended before accepting time commands");
  }

  let nextId = 1;
  async function sendTime(commandBody) {
    const id = nextId++;
    // Register the WS waiter BEFORE HTTP POST so a fast WS command event cannot be missed.
    const eventPromise = stream.waitForCommand(target.run_id, id);
    // Prevent unhandled rejection if the run ends or WS closes while POST api/time is in flight.
    eventPromise.catch(() => undefined);
    // Attach BOTH `instance` and `run_id` along with numeric `id`.
    let ack;
    try {
      ack = await call("POST", "api/time", { ...commandBody, ...target, id });
    } catch (err) {
      stream.cancelCommandWait(target.run_id, id);
      throw err;
    }
    // HTTP 200 means ONLY that the GUI server wrote the line to launcher stdin.
    console.log(
      `http ack: ${ack.command.command} id=${ack.command.id} run_id=${ack.run_id}`,
    );
    // The launcher's actual accept/refuse decision arrives asynchronously over WebSocket.
    const ev = await eventPromise;
    console.log(
      `command event: id=${ev.id} command=${ev.command} accepted=${ev.accepted}` +
        (ev.accepted
          ? `${ev.ms !== undefined ? ` ms=${ev.ms}` : ""}${ev.at_ms !== undefined ? ` at_ms=${ev.at_ms}` : ""}`
          : ` reason=${ev.reason} message=${ev.message}`),
    );
    return { ack, ev };
  }

  // 2a. Accepted `step` command while paused
  const stepped = await sendTime({ command: "step", ms: stepMs });
  if (!stepped.ev.accepted) {
    throw new Error(`expected step to be accepted, got ${stepped.ev.reason}`);
  }
  await stream.waitUntil(
    () =>
      stream.endedSessions.has(target.run_id) ||
      (stream.launcherStateFor(target.run_id)?.paused === true &&
        (stream.launcherStateFor(target.run_id)?.paused_at_ms ?? 0) >= stepMs),
    `paused at >= ${stepMs} ms`,
  );
  if (stream.endedSessions.has(target.run_id)) {
    throw new Error(`run ended before pausing at >= ${stepMs} ms`);
  }

  // 2b. Accepted `resume` command
  const resumed = await sendTime({ command: "resume" });
  if (!resumed.ev.accepted) {
    throw new Error(`expected resume to be accepted, got ${resumed.ev.reason}`);
  }
  await stream.waitUntil(
    () =>
      stream.endedSessions.has(target.run_id) ||
      stream.launcherStateFor(target.run_id)?.paused === false,
    "launcher running (paused=false)",
  );
  if (stream.endedSessions.has(target.run_id)) {
    throw new Error("run ended before resuming");
  }

  // 2c. Refused `step` command while running: HTTP 200 (forwarded) followed by WS `accepted: false` (`not-paused`)
  const refused = await sendTime({ command: "step", ms: stepMs });
  if (refused.ev.accepted !== false || refused.ev.reason !== "not-paused") {
    throw new Error(
      `expected step while running to be refused with not-paused, got accepted=${refused.ev.accepted} reason=${refused.ev.reason}`,
    );
  }

  // 3. Reconnect WebSocket with `(instance, run_id, after_stream_seq)`
  const resumeFrom = stream.lastStreamSeq;
  await stream.close();
  const reconnect = await stream.connect({
    instance: target.instance,
    run_id: target.run_id,
    after_stream_seq: resumeFrom,
  });
  console.log(
    `reconnected: requested_after=${resumeFrom} replay_after=${reconnect.hello.replay?.after_stream_seq ?? null} replayed=${reconnect.replayed} synced_seq=${reconnect.synced.stream_seq}`,
  );

  // 4. Stop the run cleanly with targeted `POST api/stop-session` (`instance` + `run_id`)
  const stopAck = await call("POST", "api/stop-session", {
    ...target,
    force: false,
  });
  console.log(
    `stop-session ack: instance=${stopAck.instance} run_id=${stopAck.run_id}`,
  );
  await stream.waitUntil(
    () => stream.endedSessions.has(target.run_id),
    "run ended after stop-session",
  );
  const summary = stream.endedSessions.get(target.run_id);
  const outcome = summary.result?.result ?? "none";
  await finish(outcome === "stopped" || outcome === "passed" ? 0 : 1, `result: ${outcome}`);
}

main().catch((error) => void finish(1, `error: ${error.message}`));
