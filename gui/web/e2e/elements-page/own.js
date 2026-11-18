// The page's own script: it asks for the connection that the parts use, and has a button of its own.
import { connect } from '/vsil/vehicle-sil-gui-client.mjs';

const client = await connect();
// Pausing with no run is refused: the refusal shows in <vsil-status>, because the parts use this same connection.
document
  .getElementById('pause')
  .addEventListener('click', () => client.time.pause().catch(() => undefined));
document.body.dataset.own = 'connected';
// With ?paused the start part starts the run paused (its intent wins over what the choosing parts selected).
if (new URLSearchParams(location.search).has('paused')) {
  document.querySelector('vsil-run-controls').intent = { start_paused: true };
}
