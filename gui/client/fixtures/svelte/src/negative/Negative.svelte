<script lang="ts">
  import '@vehicle-sil/gui-client/elements';
  import '@vehicle-sil/gui-client/svelte';
  import '@vehicle-sil/gui-client/timeline';

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

<vsil-value noUnit={123}></vsil-value>
<vsil-value no-unit={false}></vsil-value>
<vsil-slider onvsil-write-queued={(e) => expectString(e.detail.seq)}></vsil-slider>
<vsil-write
  onvsil-write-result={(e) => (!e.detail.ok ? expectBoolean(e.detail.error.code) : undefined)}
></vsil-write>
<vehicle-sil-timeline theme="neon" onload={(e) => expectNumber(e.detail.status)}
></vehicle-sil-timeline>
