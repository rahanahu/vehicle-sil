<script setup lang="ts">
import { ref } from 'vue';
import type { Front, Intent } from '@vehicle-sil/gui-client';
import type { VehicleSilTimelineLoadDetail } from '@vehicle-sil/gui-client/timeline';
import { SAMPLE_TIMELINE, createSmokeFront, initSmokeState } from './runtime-helpers';

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

function onLoad(e: CustomEvent<VehicleSilTimelineLoadDetail>) {
  smoke.loads.push({ status: e.detail.status, end: e.detail.end, file: e.detail.file });
}

function onCursor(e: CustomEvent<{ cursor: number | null }>) {
  smoke.cursors.push(e.detail.cursor);
}
</script>

<template>
  <div>
    <vsil-write id="w-false" :client="front" variable="E.e.flag" :value="false" label="Cut" />
    <vsil-write id="w-zero" :client="front" variable="E.e.throttle" :value="0" />
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
