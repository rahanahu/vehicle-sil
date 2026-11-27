<script setup lang="ts">
import { ref } from 'vue';
import type { Front, Intent } from '@vehicle-sil/gui-client';
import type { WriteOutcome, WriteQueuedInfo } from '@vehicle-sil/gui-client/elements';
import type { VehicleSilTimelineLoadDetail } from '@vehicle-sil/gui-client/timeline';
import {
  SAMPLE_TIMELINE,
  createSmokeFront,
  initSmokeState,
  recordQueued,
  recordResult,
} from './runtime-helpers';

const smoke = initSmokeState();
const front = createSmokeFront(smoke);
const noUnit = ref(true);
const rcClient = ref<Front | undefined>(front);
const rcIntent = ref<Intent | undefined>({ speed: 'max' });
const variables = ['Body.level'];

function clearClient() {
  rcClient.value = undefined;
  rcIntent.value = undefined;
}

function onQueued(source: string, e: CustomEvent<WriteQueuedInfo>) {
  recordQueued(smoke, source, e.detail);
}

function onResult(source: string, e: CustomEvent<WriteOutcome>) {
  recordResult(smoke, source, e.detail);
}

function onLoad(e: CustomEvent<VehicleSilTimelineLoadDetail>) {
  smoke.loads.push({ status: e.detail.status, end: e.detail.end, file: e.detail.file });
}

function onCursor(e: CustomEvent<{ cursor: number | null }>) {
  smoke.cursors.push(e.detail.cursor);
}
</script>

<template>
  <div>
    <vsil-write
      id="w-false"
      :client="front"
      variable="E.e.flag"
      :value="false"
      label="Cut"
      @vsil-write-queued="(e) => onQueued('w-false', e)"
      @vsil-write-result="(e) => onResult('w-false', e)"
    />
    <vsil-write
      id="w-zero"
      :client="front"
      variable="E.e.throttle"
      :value="0"
      @vsil-write-queued="(e) => onQueued('w-zero', e)"
      @vsil-write-result="(e) => onResult('w-zero', e)"
    />
    <vsil-write
      id="w-fail"
      :client="front"
      variable="E.e.fault"
      :value="true"
      label="Fault"
      @vsil-write-queued="(e) => onQueued('w-fail', e)"
      @vsil-write-result="(e) => onResult('w-fail', e)"
    />
    <vsil-slider
      id="sl"
      :client="front"
      variable="E.e.throttle"
      @vsil-write-queued="(e) => onQueued('sl', e)"
      @vsil-write-result="(e) => onResult('sl', e)"
    />
    <vsil-value id="val" :client="front" variable="E.e.speed" :noUnit="noUnit" />
    <vsil-value id="val-kebab" :client="front" variable="E.e.speed" :no-unit="noUnit" />
    <button id="toggle-unit" type="button" @click="noUnit = !noUnit">Toggle unit</button>
    <vsil-run-controls id="rc" :client="rcClient" :intent="rcIntent" />
    <button id="clear-client" type="button" @click="clearClient">Clear client</button>
    <vsil-time-controls id="tc" :client="front" :stepMs="25" speeds="1 5 max" />
    <vehicle-sil-timeline
      id="tl"
      style="display: block; width: 700px"
      :compact="true"
      :variables="variables"
      :data="SAMPLE_TIMELINE"
      @load="onLoad"
      @cursorchange="onCursor"
    />
  </div>
</template>
