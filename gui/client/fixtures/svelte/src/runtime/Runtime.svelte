<script lang="ts">
  import type { Front, Intent } from '@vehicle-sil/gui-client';
  import type { VehicleSilTimelineLoadDetail } from '@vehicle-sil/gui-client/timeline';
  import { SAMPLE_TIMELINE, createSmokeFront, initSmokeState } from './runtime-helpers';

  const smoke = initSmokeState();
  const front = createSmokeFront(smoke);
  let noUnit = $state(true);
  let rcClient = $state<Front | undefined>(front);
  let rcIntent = $state<Intent | undefined>({ speed: 'max' });
  const variables = ['Body.level'];

  function clearClient() {
    rcClient = undefined;
    rcIntent = undefined;
  }

  function onLoad(e: CustomEvent<VehicleSilTimelineLoadDetail>) {
    smoke.loads.push({ status: e.detail.status, end: e.detail.end, file: e.detail.file });
  }

  function onCursor(e: CustomEvent<{ cursor: number | null }>) {
    smoke.cursors.push(e.detail.cursor);
  }
</script>

<div>
  <vsil-write id="w-false" client={front} variable="E.e.flag" value={false} label="Cut"></vsil-write>
  <vsil-write id="w-zero" client={front} variable="E.e.throttle" value={0}></vsil-write>
  <vsil-value id="val" client={front} variable="E.e.speed" {noUnit}></vsil-value>
  <button id="toggle-unit" type="button" onclick={() => (noUnit = !noUnit)}>Toggle unit</button>
  <vsil-run-controls id="rc" client={rcClient} intent={rcIntent}></vsil-run-controls>
  <button id="clear-client" type="button" onclick={clearClient}>Clear client</button>
  <vsil-time-controls id="tc" client={front} stepMs={25} speeds="1 5 max"></vsil-time-controls>
  <vehicle-sil-timeline
    id="tl"
    style="display: block; width: 700px"
    compact={true}
    {variables}
    data={SAMPLE_TIMELINE}
    onload={onLoad}
    oncursorchange={onCursor}
  ></vehicle-sil-timeline>
</div>
