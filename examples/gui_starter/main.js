// Your own script, for what the parts of index.html do not have. The parts need no script: this one adds a display of
// your own, a button of your own and the timeline. README.md explains the steps.
import { ActionError, connect, watch } from '/vsil/vehicle-sil-gui-client.mjs';

// The same connection that the parts use: connect() without options gives every caller the one that the page has.
// With no token or no server it fails, and the parts say why, so there is nothing more to show here.
const client = await connect().catch(() => null);

if (client) {
  // A value shown your own way. watch() calls the function now and on every change; the view has the value, its text and
  // unit, whether it is stale, and a sentence (`note`) when it is not a live value.
  const meter = document.getElementById('rpm');
  const text = document.getElementById('rpm-text');
  watch([client.variable('Engine.engine.speed')], (view) => {
    meter.value = typeof view.value === 'number' ? view.value : 0;
    text.textContent = view.text === '' ? '-' : `${view.text} ${view.unit}`;
    meter.dataset.stale = String(view.stale);
  });

  // An operation of your own. Every operation is a Promise that rejects with an ActionError when it did not go through;
  // `message` is a sentence for the user. `outcome` is 'unknown' when it may have been done: look before you send it again.
  const failure = document.getElementById('floor-error');
  document.getElementById('floor').addEventListener('click', async () => {
    failure.textContent = '';
    try {
      await client.variable('Engine.engine.throttle').write(100);
    } catch (error) {
      failure.textContent = error instanceof ActionError ? error.message : String(error);
    }
  });

  // The timeline of the scenario run: the element loads the file at `timeline_url` (it sends the token the page already has)
  const timeline = document.querySelector('vehicle-sil-timeline');
  watch([client.scenario], (scenario) => {
    const url = scenario?.timeline_url ?? null;
    timeline.hidden = url === null;
    if (url !== null) timeline.src = `/${url}`;
  });
}
