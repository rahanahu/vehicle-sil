<script lang="ts">
  import type { Front, Intent } from '@vehicle-sil/gui-client';
  import '@vehicle-sil/gui-client/elements';
  import type { WriteOutcome, WriteQueuedInfo } from '@vehicle-sil/gui-client/elements';
  import '@vehicle-sil/gui-client/svelte';
  import '@vehicle-sil/gui-client/timeline';
  import type {
    VehicleSilTimelineErrorDetail,
    VehicleSilTimelineLoadDetail,
  } from '@vehicle-sil/gui-client/timeline';

  const front: Front | null = null;
  const intent: Intent = { speed: 'max' };
  const vars = ['Body.*'];

  function onWriteQueued(event: CustomEvent<WriteQueuedInfo>) {
    const seq: number = event.detail.seq;
    void seq;
  }

  function onWriteResult(event: CustomEvent<WriteOutcome>) {
    if (event.detail.ok) {
      const at: number | null = event.detail.appliedAtMs;
      void at;
    } else {
      const code: string = event.detail.error.code;
      void code;
    }
  }

  function onLoad(event: CustomEvent<VehicleSilTimelineLoadDetail>) {
    const status: string = event.detail.status;
    void status;
  }

  function onError(event: CustomEvent<VehicleSilTimelineErrorDetail>) {
    const message: string = event.detail.message;
    void message;
  }

  function onCursor(event: CustomEvent<{ cursor: number | null }>) {
    const cursor: number | null = event.detail.cursor;
    void cursor;
  }

  function onStep(event: CustomEvent<{ step: number | null }>) {
    const step: number | null = event.detail.step;
    void step;
  }

  function onView(event: CustomEvent<{ from: number; to: number }>) {
    const from: number = event.detail.from;
    void from;
  }
</script>

<vsil-status client={front}></vsil-status>
<vsil-run-controls
  client={front}
  {intent}
  label-start="Run"
  labelStop="Halt"
  labelForceStop="Kill"
></vsil-run-controls>
<vsil-time-controls client={front} speeds="0.5 1 max" stepMs={25}></vsil-time-controls>
<vsil-value client={front} variable="Dash.d.speed" on="HI" off="lo" no-unit noUnit={true}
></vsil-value>
<vsil-slider
  client={front}
  variable="Engine.e.throttle"
  onvsil-write-queued={onWriteQueued}
  onvsil-write-result={onWriteResult}
></vsil-slider>
<vsil-write
  client={front}
  variable="Engine.e.flag"
  value={false}
  label="Cut"
  onvsil-write-queued={onWriteQueued}
  onvsil-write-result={onWriteResult}
></vsil-write>
<vsil-write client={front} variable="Engine.e.throttle" value={0}></vsil-write>
<vsil-write client={front} variable="Engine.e.mode" value="eco"></vsil-write>
<vsil-scenario-select client={front}></vsil-scenario-select>
<vsil-scenario-result client={front}></vsil-scenario-result>
<vsil-timeline client={front}></vsil-timeline>
<vsil-connect client={front} network="CAN1" file="bench.yaml"></vsil-connect>
<vsil-outside client={front}></vsil-outside>
<vehicle-sil-timeline
  compact={true}
  variables={vars}
  theme="dark"
  data={{ vehicle_sil_timeline: 1 }}
  onload={onLoad}
  onerror={onError}
  oncursorchange={onCursor}
  onstepselect={onStep}
  onviewchange={onView}
></vehicle-sil-timeline>
<vehicle-sil-timeline variables="Body.*" compact="" hide="help tables"></vehicle-sil-timeline>
