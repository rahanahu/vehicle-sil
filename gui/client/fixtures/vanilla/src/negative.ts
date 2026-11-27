import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/timeline';

export function checkVanillaNegative(): void {
  const val = document.createElement('vsil-value');
  // @ts-expect-error noUnit requires boolean
  val.noUnit = 'yes';

  const slider = document.createElement('vsil-slider');
  slider.addEventListener('vsil-write-queued', (e) => {
    // @ts-expect-error seq is number, not string
    const bad: string = e.detail.seq;
    void bad;
  });
  slider.addEventListener('vsil-write-result', (e) => {
    if (e.detail.ok) {
      // @ts-expect-error appliedAtMs is number | null, not string
      const bad: string = e.detail.appliedAtMs;
      void bad;
    } else {
      // @ts-expect-error error.code is string, not number
      const bad: number = e.detail.error.code;
      void bad;
    }
  });

  const write = document.createElement('vsil-write');
  // @ts-expect-error value rejects objects
  write.value = { bad: true };
  write.addEventListener('vsil-write-queued', (e) => {
    // @ts-expect-error session is string, not number
    const bad: number = e.detail.session;
    void bad;
  });
  write.addEventListener('vsil-write-result', (e) => {
    // @ts-expect-error appliedAtMs does not exist before narrowing ok === true
    const bad: number | null = e.detail.appliedAtMs;
    void bad;
  });

  const time = document.createElement('vsil-time-controls');
  // @ts-expect-error stepMs rejects boolean
  time.stepMs = false;

  const tl = document.createElement('vehicle-sil-timeline');
  // @ts-expect-error compact requires boolean
  tl.compact = 'yes';
  // @ts-expect-error theme rejects unknown literal
  tl.theme = 'solarized';
  // @ts-expect-error variables requires string[] | null
  tl.variables = [42];
  tl.addEventListener('cursorchange', (e) => {
    // @ts-expect-error cursor is number | null, not string
    const bad: string = e.detail.cursor;
    void bad;
  });
}
