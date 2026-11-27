<script setup lang="ts">
import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/timeline';
import '@vehicle-sil/gui-client/vue';

function expectBoolean(value: boolean) {
  void value;
}

function expectNumber(value: number) {
  void value;
}

function expectString(value: string) {
  void value;
}
</script>

<template>
  <div>
    <vsil-value :noUnit="123" />
    <vsil-slider @vsil-write-queued="(e) => expectString(e.detail.seq)" />
    <vsil-write
      @vsil-write-result="(e) => (!e.detail.ok ? expectBoolean(e.detail.error.code) : undefined)"
    />
    <vehicle-sil-timeline theme="neon" @load="(e) => expectNumber(e.detail.status)" />
  </div>
</template>
