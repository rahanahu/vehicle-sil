// The smallest screen of your own: plain JavaScript with no build, using only the client library (@vehicle-sil/gui-client).
// Connect, check a configuration file, start and stop a run, watch and write one variable, a slider and a lamp.
// README.md explains how to use it.
import {
  WriteError,
  createClient,
  errorMessage,
  hasRange,
  isWriteError,
  takeToken,
} from './vehicle-sil-gui-client.mjs';

const $ = (id) => document.getElementById(id);

// Read #token= from the URL the server printed and remove it from the URL (before the screen rewrites the URL)
const token = takeToken(window);
if (token === null) {
  $('connection').textContent = 'No token: open the URL that vehicle-sil-gui printed (with #token=...).';
  throw new Error('no token');
}

// Call the API relative to the server root (so that on a screen path, api/app does not become /<path>/api/app)
const client = createClient({ baseUrl: new URL('/', location.href).href, token });
const show = (id, text) => ($(id).textContent = text);
const fail = (error) => show('error', errorMessage(error));

// Connection
client.connection.state.subscribe((state) => show('connection', `connection: ${state}`));
client.session.rejected.subscribe((rejected) => {
  if (rejected) show('connection', 'The server refused the token: open the URL it printed again.');
});

// Configuration files (preselect the one given at startup)
const app = await client.loadApp();
client.connect();
const { files } = await client.configs();
for (const file of files) $('config').add(new Option(file.path, file.path));
if (app.initial_file) $('config').value = app.initial_file;

// Check, start, stop
const file = () => $('config').value;
$('check').onclick = async () => {
  show('error', '');
  try {
    const check = await client.check({ file: file(), disable: [] });
    show('check-result', `${check.result} (errors ${check.errors}, warnings ${check.warnings})`);
  } catch (error) {
    fail(error);
  }
};
$('start').onclick = () => {
  show('error', '');
  client.run.start({ file: file(), disable: [], speed: null, start_paused: false }).catch(fail);
};
$('stop').onclick = () => client.run.stop().catch(fail);

// Run state (a copy of the session)
client.session.session.subscribe((session) => {
  show('phase', session?.phase ?? '-');
  show('result', session?.result?.result ?? '-');
});

// Variables: pick one from the list to watch and write
let variables = [];
let watch = null;
client.variables.state.subscribe((state) => {
  show('variables', state.reason ? `${state.status}: ${state.reason}` : state.status);
  const next = state.peers.flatMap((peer) => peer.variables.map((info) => ({ peer: peer.name, info })));
  const key = (v) => `${v.peer}.${v.info.name}`;
  if (next.map(key).join() === variables.map(key).join()) return;
  const chosen = $('variable').value;
  variables = next;
  $('variable').replaceChildren(...variables.map((v) => new Option(key(v), key(v))));
  if (variables.some((v) => key(v) === chosen)) $('variable').value = chosen;
  watchChosen();
  bindParts();
});

function chosen() {
  return variables.find((v) => `${v.peer}.${v.info.name}` === $('variable').value) ?? null;
}

function watchChosen() {
  watch?.close(); // always close a watch you no longer need
  watch = null;
  const v = chosen();
  show('value', '-');
  if (!v) return;
  const ref = { peer: v.peer, name: v.info.name };
  watch = client.variables.watch([ref], { periodMs: 100 });
  watch.values.subscribe(() => {
    const reading = watch?.value(ref);
    if (reading) show('value', `${reading.value} ${v.info.unit} (at ${reading.simMs} ms)`);
  });
}
$('variable').onchange = watchChosen;

$('write').onclick = async () => {
  const v = chosen();
  if (!v) return;
  const text = $('new-value').value.trim();
  // Convert to the type: true / false for bool, an item name or a number for an enum, a number otherwise
  const item = v.info.enum?.find((e) => e.name === text);
  const value = v.info.type === 'bool' ? text === 'true' : item ? item.value : Number(text);
  show('write-result', 'sending…');
  try {
    const operation = await client.variables.write({ [v.peer]: { [v.info.name]: value } });
    show('write-result', `applied at ${operation.peers[v.peer]?.appliedAtMs} ms`);
  } catch (error) {
    // Do not resend a write whose outcome is unknown (no-answer) without checking first
    show('write-result', error instanceof WriteError ? `${error.kind}: ${error.message}` : errorMessage(error));
  }
};

// Slider and lamp: they use client.variables.bind, which binds one widget to one variable. One state holds the value's
// condition (none yet, stale and so on), whether it can be written and why not, and the progress of a write, so the
// widget only draws it. Here a writable number variable with a range drives the slider, and a bool variable (a
// read-only one if there is one) drives the lamp.
let slider = null;
let lamp = null;
let dragging = false;

function bindParts() {
  const numeric = (v) => v.info.type === 'real' || v.info.type === 'integer';
  const forSlider = variables.find((v) => v.info.writable && numeric(v) && hasRange(v.info));
  const bools = variables.filter((v) => v.info.type === 'bool');
  const forLamp = bools.find((v) => !v.info.writable) ?? bools[0];
  slider = rebind(slider, forSlider, { needsRange: true, needsValue: true }, 'slider-name', showSlider);
  lamp = rebind(lamp, forLamp, {}, 'lamp-name', showLamp);
}

/** Rebinds only when the variable changes (always close a binding you no longer need). */
function rebind(bound, v, options, nameId, draw) {
  const same = bound && v && bound.ref.peer === v.peer && bound.ref.name === v.info.name;
  if (same) return bound;
  bound?.close();
  show(nameId, v ? `${v.peer}.${v.info.name}` : '-');
  if (!v) return null;
  const next = client.variables.bind({ peer: v.peer, name: v.info.name }, options);
  next.state.subscribe(draw); // called on every change; get() returns the current state
  draw(next.state.get());
  return next;
}

function showSlider({ info, reading, pending, block, writeStatus }) {
  const input = $('slider');
  // block is why it cannot be written now (read-only, not-ready, ended, peer-disconnected, no-value and so on),
  // or null if it can
  input.disabled = block !== null;
  show('slider-why', block === null ? '' : `(cannot write: ${block})`);
  if (info) {
    input.min = String(info.min);
    input.max = String(info.max);
    input.step = info.type === 'integer' ? '1' : String(info.step ?? 'any');
  }
  // pending is the value sent and waiting for its result. Show it instead of the value read, so the released thumb
  // does not appear to jump back
  const value = pending ?? reading?.value;
  if (!dragging && typeof value === 'number') input.value = String(value);
  show('slider-value', typeof value === 'number' ? `${value} ${info?.unit ?? ''}` : '-');
  // writeStatus: idle, sending, queued, applied, or the reason the write failed (with a message)
  show('slider-write', isWriteError(writeStatus) ? `${writeStatus.kind}: ${writeStatus.message}` : writeStatus.kind);
}

function showLamp({ reading, condition }) {
  const value = reading?.value;
  $('lamp').dataset.on = String(value === true);
  show('lamp', value === undefined ? '-' : value ? 'on' : 'off');
  // condition is the state of the value: besides live, stale, a value at the paused time (paused), a value of an
  // ended run (ended) and so on
  show('lamp-condition', condition.kind === 'live' ? '' : `(${condition.kind})`);
}

// Write when the slider is released (change), not while it moves. If writes come in a row, only the last value is
// kept until the server has received the previous write, then sent (bind does this). The result shows in writeStatus
// of the state
$('slider').oninput = () => {
  dragging = true;
};
$('slider').onchange = () => {
  dragging = false;
  slider?.write(Number($('slider').value));
};
