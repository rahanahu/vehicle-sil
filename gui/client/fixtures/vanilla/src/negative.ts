import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/timeline';

export function checkVanillaNegative(): void {
  const val = document.createElement('vsil-value');
  // @ts-expect-error noUnit requires boolean
  val.noUnit = 'yes';

  const write = document.createElement('vsil-write');
  // @ts-expect-error value rejects objects
  write.value = { bad: true };

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
