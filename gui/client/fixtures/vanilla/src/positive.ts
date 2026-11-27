import type { Front } from '@vehicle-sil/gui-client';
import '@vehicle-sil/gui-client/elements';
import type {
  VsilElementContracts,
  VsilRunControlsElement,
  VsilSliderElement,
  VsilTimeControlsElement,
  VsilValueElement,
  VsilWriteElement,
  WriteOutcome,
  WriteQueuedInfo,
} from '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/timeline';
import type {
  VehicleSilTimelineContract,
  VehicleSilTimelineElement,
} from '@vehicle-sil/gui-client/timeline';

export function checkVanilla(front: Front): void {
  const status = document.createElement('vsil-status');
  status.client = front;

  const run: VsilRunControlsElement = document.createElement('vsil-run-controls');
  run.client = front;
  run.intent = { speed: 'max' };
  run.labelStart = 'Run';
  run.labelStop = 'Stop';
  run.labelForceStop = 'Kill';

  const time: VsilTimeControlsElement = document.createElement('vsil-time-controls');
  time.client = front;
  time.speeds = '0.5 1 max';
  time.stepMs = 25;
  time.stepMs = '50';
  time.labelPause = 'P';
  time.labelResume = 'R';
  time.labelStep = 'S';
  time.labelRunUntil = 'U';

  const val: VsilValueElement = document.createElement('vsil-value');
  val.client = front;
  val.variable = 'Dash.d.speed';
  val.on = 'ON';
  val.off = 'off';
  val.noUnit = true;

  const slider: VsilSliderElement = document.createElement('vsil-slider');
  slider.client = front;
  slider.variable = 'Engine.e.throttle';
  slider.addEventListener('vsil-write-queued', (e) => {
    const info: WriteQueuedInfo = e.detail;
    const seq: number = info.seq;
    void seq;
  });
  slider.addEventListener('vsil-write-result', (e) => {
    const outcome: WriteOutcome = e.detail;
    if (outcome.ok) {
      const at: number | null = outcome.appliedAtMs;
      void at;
    } else {
      const code: string = outcome.error.code;
      void code;
    }
  });

  const write: VsilWriteElement = document.createElement('vsil-write');
  write.client = front;
  write.variable = 'Engine.e.flag';
  write.value = false;
  write.value = 0;
  write.value = 'eco';
  write.value = null;
  write.label = 'Cut';
  write.addEventListener('vsil-write-queued', (e) => {
    const session: string = e.detail.session;
    void session;
  });
  write.addEventListener('vsil-write-result', (e) => {
    if (e.detail.ok) {
      const late: boolean = e.detail.late;
      void late;
    } else {
      const outcome: 'refused' | 'unknown' = e.detail.error.outcome;
      void outcome;
    }
  });

  const select = document.createElement('vsil-scenario-select');
  select.client = front;
  const result = document.createElement('vsil-scenario-result');
  result.client = front;
  const tlWrap = document.createElement('vsil-timeline');
  tlWrap.client = front;
  const conn = document.createElement('vsil-connect');
  conn.client = front;
  conn.network = 'CAN1';
  conn.file = 'bench.yaml';
  const outside = document.createElement('vsil-outside');
  outside.client = front;

  const timeline: VehicleSilTimelineElement = document.createElement('vehicle-sil-timeline');
  timeline.compact = true;
  timeline.variables = ['Body.*'];
  timeline.theme = 'dark';
  timeline.data = { vehicle_sil_timeline: 1 };
  timeline.addEventListener('load', (e) => {
    const s: string = e.detail.status;
    void s;
  });
  timeline.addEventListener('error', (e) => {
    const msg: string = e.detail.message;
    void msg;
  });
  timeline.addEventListener('cursorchange', (e) => {
    const c: number | null = e.detail.cursor;
    void c;
  });

  const _tag: keyof VsilElementContracts = 'vsil-write';
  const _tlTag: keyof VehicleSilTimelineContract = 'vehicle-sil-timeline';
  void [_tag, _tlTag, status, run, time, val, slider, write, select, result, tlWrap, conn, outside];
}
