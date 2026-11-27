import { useState } from 'react';
import { createRoot } from 'react-dom/client';
import type { Front, Intent } from '@vehicle-sil/gui-client';
import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/react';
import '@vehicle-sil/gui-client/timeline';
import {
  SAMPLE_TIMELINE,
  createSmokeFront,
  initSmokeState,
  recordQueued,
  recordResult,
} from './runtime-helpers';

const smoke = initSmokeState();
const front = createSmokeFront(smoke);
const variables = ['Body.level'];

function App() {
  const [noUnit, setNoUnit] = useState(true);
  const [rcClient, setRcClient] = useState<Front | undefined>(front);
  const [rcIntent, setRcIntent] = useState<Intent | undefined>({ speed: 'max' });
  return (
    <div>
      <vsil-write
        id="w-false"
        client={front}
        variable="E.e.flag"
        value={false}
        label="Cut"
        onvsil-write-queued={(e) => recordQueued(smoke, 'w-false', e.detail)}
        onvsil-write-result={(e) => recordResult(smoke, 'w-false', e.detail)}
      />
      <vsil-write
        id="w-zero"
        client={front}
        variable="E.e.throttle"
        value={0}
        onvsil-write-queued={(e) => recordQueued(smoke, 'w-zero', e.detail)}
        onvsil-write-result={(e) => recordResult(smoke, 'w-zero', e.detail)}
      />
      <vsil-write
        id="w-fail"
        client={front}
        variable="E.e.fault"
        value={true}
        label="Fault"
        onvsil-write-queued={(e) => recordQueued(smoke, 'w-fail', e.detail)}
        onvsil-write-result={(e) => recordResult(smoke, 'w-fail', e.detail)}
      />
      <vsil-slider
        id="sl"
        client={front}
        variable="E.e.throttle"
        onvsil-write-queued={(e) => recordQueued(smoke, 'sl', e.detail)}
        onvsil-write-result={(e) => recordResult(smoke, 'sl', e.detail)}
      />
      <vsil-value id="val" client={front} variable="E.e.speed" noUnit={noUnit} />
      <button id="toggle-unit" type="button" onClick={() => setNoUnit((v) => !v)}>
        Toggle unit
      </button>
      <vsil-run-controls id="rc" client={rcClient} intent={rcIntent} />
      <button
        id="clear-client"
        type="button"
        onClick={() => {
          setRcClient(undefined);
          setRcIntent(undefined);
        }}
      >
        Clear client
      </button>
      <vsil-time-controls id="tc" client={front} stepMs={25} speeds="1 5 max" />
      <vehicle-sil-timeline
        id="tl"
        style={{ display: 'block', width: '700px' }}
        compact={true}
        variables={variables}
        data={SAMPLE_TIMELINE}
        onload={(e) => {
          smoke.loads.push({ status: e.detail.status, end: e.detail.end, file: e.detail.file });
        }}
        oncursorchange={(e) => {
          smoke.cursors.push(e.detail.cursor);
        }}
      />
    </div>
  );
}

createRoot(document.getElementById('app')!).render(<App />);
smoke.ready = true;
