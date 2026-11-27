import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/timeline';
import { SAMPLE_TIMELINE, createSmokeFront, initSmokeState } from './runtime-helpers';

const smoke = initSmokeState();
const front = createSmokeFront(smoke);
const app = document.getElementById('app')!;

const writeFalse = document.createElement('vsil-write');
writeFalse.id = 'w-false';
writeFalse.client = front;
writeFalse.variable = 'E.e.flag';
writeFalse.value = false;
writeFalse.label = 'Cut';

const writeZero = document.createElement('vsil-write');
writeZero.id = 'w-zero';
writeZero.client = front;
writeZero.variable = 'E.e.throttle';
writeZero.value = 0;

const val = document.createElement('vsil-value');
val.id = 'val';
val.client = front;
val.variable = 'E.e.speed';
val.noUnit = true;

const toggle = document.createElement('button');
toggle.id = 'toggle-unit';
toggle.type = 'button';
toggle.textContent = 'Toggle unit';
toggle.addEventListener('click', () => {
  val.noUnit = !val.noUnit;
});

const rc = document.createElement('vsil-run-controls');
rc.id = 'rc';
rc.client = front;
rc.intent = { speed: 'max' };

const clearClient = document.createElement('button');
clearClient.id = 'clear-client';
clearClient.type = 'button';
clearClient.textContent = 'Clear client';
clearClient.addEventListener('click', () => {
  (rc as unknown as { client: unknown }).client = undefined;
  (rc as unknown as { intent: unknown }).intent = undefined;
});

const tc = document.createElement('vsil-time-controls');
tc.id = 'tc';
tc.client = front;
tc.stepMs = 25;
tc.speeds = '1 5 max';

const tl = document.createElement('vehicle-sil-timeline');
tl.id = 'tl';
tl.style.display = 'block';
tl.style.width = '700px';
tl.compact = true;
tl.variables = ['Body.level'];
tl.data = SAMPLE_TIMELINE;
tl.addEventListener('load', (e) => {
  smoke.loads.push({ status: e.detail.status, end: e.detail.end, file: e.detail.file });
});
tl.addEventListener('cursorchange', (e) => {
  smoke.cursors.push(e.detail.cursor);
});

app.append(writeFalse, writeZero, val, toggle, rc, clearClient, tc, tl);
smoke.ready = true;
