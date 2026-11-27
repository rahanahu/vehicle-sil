<script setup lang="ts">
import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/timeline';
import '@vehicle-sil/gui-client/vue';

function expectNumber(value: number) {
  void value;
}
</script>

<template>
  <div>
    <vsil-value :noUnit="123" />
    <vehicle-sil-timeline theme="neon" @load="(e) => expectNumber(e.detail.status)" />
  </div>
</template>
