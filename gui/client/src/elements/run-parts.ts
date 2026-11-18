// The parts of the run: status, the run buttons and the time controls.
import { watch } from '../index';
import type { Front, FrontControls, Intent } from '../index';
import { pageIntent } from './page-intent';
import { Part, h, quietly, setData } from './part';
import { reasonText } from './reasons';

/** `<vsil-status>`: the connection, the run, the virtual time (marked when it falls behind real time) and the one problem. */
export class VsilStatusElement extends Part {
  protected draw(client: Front, root: HTMLElement): void {
    const state = h('span', { class: 'vsil-state' });
    const time = h('span', { class: 'vsil-time' });
    const problem = h('span', { class: 'vsil-problem', role: 'alert' });
    root.append(state, ' ', time, ' ', problem);
    this.own(
      watch(client.status, (s) => {
        state.textContent =
          s.link !== 'online'
            ? s.link
            : s.run === 'none'
              ? 'ready'
              : (s.run === 'ended' || s.run === 'crashed') && s.result && !s.problem
                ? s.result.message // with a problem, the problem has the sentence
                : s.run;
        problem.textContent = s.problem ? reasonText(s.problem) : '';
        setData(this, 'link', s.link);
        setData(this, 'run', s.run);
      }),
    );
    this.own(
      watch(client.clock, (c) => {
        time.textContent =
          c.sim_ms === null
            ? ''
            : `${c.sim_ms} ms${c.pacing === 'late' ? ' (behind real time)' : ''}`;
        setData(this, 'pacing', c.pacing);
      }),
    );
  }
}

/** `<vsil-run-controls>`: Start (with the page's selection and the `intent` property), Stop and, after a first stop, force stop. */
export class VsilRunControlsElement extends Part {
  static override observedAttributes = ['label-start', 'label-stop', 'label-force-stop'];

  /** What to start with, over what the choosing parts of the page selected. */
  get intent(): Intent | null {
    return this.#intent;
  }
  set intent(value: Intent | null) {
    this.#intent = value;
  }
  #intent: Intent | null = null;

  constructor() {
    super();
    this.upgrade('intent');
  }

  protected draw(client: Front, root: HTMLElement): void {
    const force = h(
      'span',
      { class: 'vsil-force' },
      ...this.button(
        client,
        this.text('label-force-stop', 'Stop without waiting'),
        'force_stop',
        () => client.run.forceStop(),
      ),
    );
    root.append(
      ...this.button(client, this.text('label-start', 'Start'), 'start', () =>
        // the file that the scenario part lists the scenarios of (null: the server's start-up file)
        client.run.start(
          { ...pageIntent.get(), ...this.intent },
          client.status.get().file === null ? undefined : { file: client.status.get().file! },
        ),
      ),
      ...this.button(client, this.text('label-stop', 'Stop'), 'stop', () => client.run.stop()),
      force,
    );
    // Offered exactly when the control says it can be used.
    this.own(watch(client.controls, (controls) => (force.hidden = !controls.force_stop.ok)));
  }
}

const speedValue = (text: string): number | 'max' | null => {
  if (text === 'max') return 'max';
  const number = Number(text);
  return text !== '' && Number.isFinite(number) && number > 0 ? number : null;
};

/** Whether the run takes the speed of an option now (`controls.speed`: its range, and `fastest` for `max`). */
const takes = (text: string, range: FrontControls['speed']): boolean => {
  const value = speedValue(text);
  return value === 'max'
    ? range.fastest
    : value !== null && value >= range.min && value <= range.max;
};

/** `<vsil-time-controls speeds="0.5 1 5 max" step-ms="10">`: pause, resume, step, run until and speed. */
export class VsilTimeControlsElement extends Part {
  static override observedAttributes = [
    'speeds',
    'step-ms',
    'label-pause',
    'label-resume',
    'label-step',
    'label-run-until',
  ];

  protected draw(client: Front, root: HTMLElement): void {
    const stepMs = h('input', { type: 'number', min: '1', value: this.text('step-ms', '10') });
    const untilMs = h('input', { type: 'number', min: '0', value: '1000' });
    const speed = h('select');
    const speeds: string[] = [];
    speed.add(new Option('-', '')); // no speed known yet
    const addSpeed = (text: string) => {
      speed.add(new Option(text === 'max' ? 'fastest' : `${text}×`, text));
      speeds.push(text);
    };
    for (const text of this.text('speeds', '0.5 1 5 max').split(/\s+/))
      if (speedValue(text) !== null) addSpeed(text);
    // A speed that the run does not take cannot be chosen; the fastest is not offered at all then.
    const markSpeeds = () => {
      const range = client.controls.get().speed;
      for (const option of speed.options) {
        if (option.value === '') continue;
        option.disabled = !takes(option.value, range);
        option.hidden = option.value === 'max' && !range.fastest;
      }
    };
    // The select shows the speed that the clock says; after a refused change or an ignored choice it goes back to it.
    const showSpeed = () => {
      const { speed: now } = client.clock.get();
      const text = now === null ? '' : String(now);
      if (text !== '' && !speeds.includes(text)) {
        addSpeed(text);
        markSpeeds();
      }
      speed.value = text;
    };
    speed.addEventListener('change', () => {
      const value = speedValue(speed.value);
      if (
        value === null ||
        speed.getAttribute('aria-disabled') === 'true' ||
        !takes(speed.value, client.controls.get().speed)
      )
        showSpeed();
      else void quietly(() => client.time.setSpeed(value)).then(showSpeed);
    });
    const speedWhy = this.describe(client, 'speed', speed);
    root.append(
      ...this.button(client, this.text('label-pause', 'Pause'), 'pause', () => client.time.pause()),
      ...this.button(client, this.text('label-resume', 'Resume'), 'resume', () =>
        client.time.resume(),
      ),
      h('label', {}, 'Step (ms) ', stepMs),
      ...this.button(client, this.text('label-step', 'Step'), 'step', () =>
        client.time.step(Number(stepMs.value)),
      ),
      h('label', {}, 'Until (ms) ', untilMs),
      ...this.button(client, this.text('label-run-until', 'Run until'), 'run_until', () =>
        client.time.runUntil(Number(untilMs.value)),
      ),
      h('label', {}, 'Speed ', speed),
      speedWhy,
    );
    this.own(
      watch(client.controls, (controls) => {
        stepMs.min = String(controls.step.min_ms);
        stepMs.max = String(controls.step.max_ms);
        markSpeeds();
      }),
    );
    this.own(watch(client.clock, showSpeed));
  }
}
