<script lang="ts">
  import '@vehicle-sil/gui-client/elements';
  import '@vehicle-sil/gui-client/svelte';
  import '@vehicle-sil/gui-client/timeline';

  function expectNumber(value: number) {
    void value;
  }
</script>

<vsil-value noUnit={123}></vsil-value>
<vsil-value no-unit={false}></vsil-value>
<vehicle-sil-timeline theme="neon" onload={(e) => expectNumber(e.detail.status)}
></vehicle-sil-timeline>
