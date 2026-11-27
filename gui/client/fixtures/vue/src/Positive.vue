<script setup lang="ts">
import type { Front, Intent } from '@vehicle-sil/gui-client';
import '@vehicle-sil/gui-client/elements';
import type { WriteOutcome, WriteQueuedInfo } from '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/timeline';
import type {
  VehicleSilTimelineErrorDetail,
  VehicleSilTimelineLoadDetail,
} from '@vehicle-sil/gui-client/timeline';
import '@vehicle-sil/gui-client/vue';
import type { VueEventProps } from '@vehicle-sil/gui-client/vue';

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

// Verifies hyphenated custom events map to both on<Capitalize<K>> and on<Capitalize<CamelCase<K>>>.
type HyphenEvents = VueEventProps<{ 'vsil-write-result': CustomEvent<WriteOutcome> }>;
const hyphenCheck: HyphenEvents = {
  onVsilWriteResult: (e) => {
    const ok: boolean = e.detail.ok;
    void ok;
  },
  'onVsil-write-result': (e) => {
    const ok: boolean = e.detail.ok;
    void ok;
  },
};
void hyphenCheck;
</script>

<template>
  <div>
    <vsil-status :client="front" />
    <vsil-run-controls
      :client="front"
      :intent="intent"
      label-start="Run"
      labelStop="Halt"
      labelForceStop="Kill"
    />
    <vsil-time-controls :client="front" speeds="0.5 1 max" :step-ms="25" />
    <vsil-time-controls :client="front" :stepMs="50" />
    <vsil-value :client="front" variable="Dash.d.speed" on="HI" off="lo" no-unit />
    <vsil-value :client="front" variable="Dash.d.speed" :noUnit="true" />
    <vsil-value :client="front" variable="Dash.d.speed" :noUnit="false" />
    <vsil-slider
      :client="front"
      variable="Engine.e.throttle"
      @vsil-write-queued="onWriteQueued"
      @vsil-write-result="onWriteResult"
    />
    <vsil-write
      :client="front"
      variable="Engine.e.flag"
      :value="false"
      label="Cut"
      @vsil-write-queued="onWriteQueued"
      @vsil-write-result="onWriteResult"
    />
    <vsil-write :client="front" variable="Engine.e.throttle" :value="0" />
    <vsil-write :client="front" variable="Engine.e.mode" value="eco" />
    <vsil-scenario-select :client="front" />
    <vsil-scenario-result :client="front" />
    <vsil-timeline :client="front" />
    <vsil-connect :client="front" network="CAN1" file="bench.yaml" />
    <vsil-outside :client="front" />
    <vehicle-sil-timeline
      :compact="true"
      :variables="vars"
      theme="dark"
      :data="{ vehicle_sil_timeline: 1 }"
      @load="onLoad"
      @error="onError"
      @cursorchange="onCursor"
      @stepselect="onStep"
      @viewchange="onView"
    />
    <vehicle-sil-timeline variables="Body.*" compact="" hide="help tables" />
  </div>
</template>
