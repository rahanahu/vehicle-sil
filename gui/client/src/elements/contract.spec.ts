import type { VehicleSilTimelineContract } from '../../../timeline/vehicle-sil-timeline';
import { fakeFront, viewOf } from './fake-front';
import './index';
import type {
  VsilElementContracts,
  VsilWriteEventMap,
  WriteOutcome,
  WriteQueuedInfo,
} from './index';

const settle = () => new Promise((resolve) => setTimeout(resolve, 0));

const toCamel = (attr: string): string =>
  attr.replace(/-([a-z])/g, (_, ch: string) => ch.toUpperCase());

function hasAccessor(target: object, prop: string): boolean {
  let proto: object | null = target;
  while (proto && proto !== HTMLElement.prototype) {
    const desc = Object.getOwnPropertyDescriptor(proto, prop);
    if (desc) return typeof desc.get === 'function' && typeof desc.set === 'function';
    proto = Object.getPrototypeOf(proto) as object | null;
  }
  return false;
}

// Compile-time checks: VsilElementContracts matches HTMLElementTagNameMap and every contracted prop is writable.
type VsilDomTags = Extract<keyof HTMLElementTagNameMap, `vsil-${string}`>;
type AssertExact<A, B> = [A] extends [B] ? ([B] extends [A] ? true : never) : never;
const _tagsMatch: AssertExact<keyof VsilElementContracts, VsilDomTags> = true;

type ContractElementMatch = {
  [K in keyof VsilElementContracts]: AssertExact<
    VsilElementContracts[K]['element'],
    HTMLElementTagNameMap[K]
  >;
};
const _elementsMatch: Record<keyof VsilElementContracts, true> = {} as ContractElementMatch;

// Compile-time setter compatibility check for every element contract (including divergent getter/setter types).
type CheckWritable = {
  [K in keyof VsilElementContracts]: (
    el: VsilElementContracts[K]['element'],
    props: Required<VsilElementContracts[K]['props']>,
  ) => void;
};
const _writableVsil: CheckWritable = {
  'vsil-status': (el, p) => {
    el.client = p.client;
  },
  'vsil-run-controls': (el, p) => {
    el.client = p.client;
    el.intent = p.intent;
    el.labelStart = p.labelStart;
    el.labelStop = p.labelStop;
    el.labelForceStop = p.labelForceStop;
  },
  'vsil-time-controls': (el, p) => {
    el.client = p.client;
    el.speeds = p.speeds;
    el.stepMs = p.stepMs;
    el.labelPause = p.labelPause;
    el.labelResume = p.labelResume;
    el.labelStep = p.labelStep;
    el.labelRunUntil = p.labelRunUntil;
  },
  'vsil-value': (el, p) => {
    el.client = p.client;
    el.variable = p.variable;
    el.on = p.on;
    el.off = p.off;
    el.noUnit = p.noUnit;
  },
  'vsil-slider': (el, p) => {
    el.client = p.client;
    el.variable = p.variable;
  },
  'vsil-write': (el, p) => {
    el.client = p.client;
    el.variable = p.variable;
    el.value = p.value;
    el.label = p.label;
  },
  'vsil-scenario-select': (el, p) => {
    el.client = p.client;
  },
  'vsil-case-select': (el, p) => {
    el.client = p.client;
  },
  'vsil-scenario-result': (el, p) => {
    el.client = p.client;
  },
  'vsil-timeline': (el, p) => {
    el.client = p.client;
  },
  'vsil-connect': (el, p) => {
    el.client = p.client;
    el.network = p.network;
    el.file = p.file;
  },
  'vsil-outside': (el, p) => {
    el.client = p.client;
  },
};

const _writableTimeline = (
  el: VehicleSilTimelineContract['vehicle-sil-timeline']['element'],
  p: Required<VehicleSilTimelineContract['vehicle-sil-timeline']['props']>,
) => {
  el.src = p.src;
  el.fileName = p.fileName;
  el.data = p.data;
  el.token = p.token;
  el.headers = p.headers;
  el.cursor = p.cursor;
  el.step = p.step;
  el.from = p.from;
  el.to = p.to;
  el.variables = p.variables;
  el.labels = p.labels;
  el.hide = p.hide;
  el.compact = p.compact;
  el.theme = p.theme;
  el.messages = p.messages;
};

const _sliderEventsMatch: AssertExact<
  VsilElementContracts['vsil-slider']['events'],
  VsilWriteEventMap
> = true;
const _writeEventsMatch: AssertExact<
  VsilElementContracts['vsil-write']['events'],
  VsilWriteEventMap
> = true;
const _typedWriteListeners = (
  slider: VsilElementContracts['vsil-slider']['element'],
  write: VsilElementContracts['vsil-write']['element'],
) => {
  const onQueued = (e: CustomEvent<WriteQueuedInfo>) => {
    const seq: number = e.detail.seq;
    void seq;
  };
  const onResult = (e: CustomEvent<WriteOutcome>) => {
    if (e.detail.ok) {
      const at: number | null = e.detail.appliedAtMs;
      void at;
    } else {
      const code: string = e.detail.error.code;
      void code;
    }
  };
  slider.addEventListener('vsil-write-queued', onQueued);
  slider.removeEventListener('vsil-write-queued', onQueued);
  slider.addEventListener('vsil-write-result', onResult);
  slider.removeEventListener('vsil-write-result', onResult);
  write.addEventListener('vsil-write-queued', onQueued);
  write.removeEventListener('vsil-write-queued', onQueued);
  write.addEventListener('vsil-write-result', onResult);
  write.removeEventListener('vsil-write-result', onResult);
};

void [
  _tagsMatch,
  _elementsMatch,
  _writableVsil,
  _writableTimeline,
  _sliderEventsMatch,
  _writeEventsMatch,
  _typedWriteListeners,
];

const ALL_VSIL_TAGS: (keyof VsilElementContracts)[] = [
  'vsil-status',
  'vsil-run-controls',
  'vsil-time-controls',
  'vsil-value',
  'vsil-slider',
  'vsil-write',
  'vsil-scenario-select',
  'vsil-case-select',
  'vsil-scenario-result',
  'vsil-timeline',
  'vsil-connect',
  'vsil-outside',
];

afterEach(() => {
  document.body.replaceChildren();
});

describe('vsil-* element contracts and property reflection', () => {
  it('exposes a camelCase getter/setter pair for all 19 observedAttributes across the 12 parts', () => {
    let totalObserved = 0;
    for (const tag of ALL_VSIL_TAGS) {
      const ctor = customElements.get(tag) as
        (CustomElementConstructor & { observedAttributes?: string[] }) | undefined;
      expect(ctor, `${tag} must be registered`).toBeDefined();
      const observed = ctor!.observedAttributes ?? [];
      totalObserved += observed.length;
      const el = document.createElement(tag);
      expect(hasAccessor(el, 'client'), `${tag}.client`).toBe(true);
      for (const attr of observed) {
        const prop = toCamel(attr);
        expect(hasAccessor(el, prop), `${tag}.${prop} (from ${attr})`).toBe(true);
      }
    }
    expect(totalObserved).toBe(19);
  });

  it('reflects vsil-run-controls label properties to attributes and rendered buttons', async () => {
    const front = fakeFront();
    const el = document.createElement('vsil-run-controls');
    el.client = front;
    el.labelStart = 'Begin';
    el.labelStop = 'End';
    el.labelForceStop = 'Abort';
    document.body.append(el);
    await settle();

    expect(el.getAttribute('label-start')).toBe('Begin');
    expect(el.getAttribute('label-stop')).toBe('End');
    expect(el.getAttribute('label-force-stop')).toBe('Abort');
    expect([el.labelStart, el.labelStop, el.labelForceStop]).toEqual(['Begin', 'End', 'Abort']);
    expect([...el.querySelectorAll('button')].map((b) => b.textContent)).toEqual([
      'Begin',
      'End',
      'Abort',
    ]);

    el.labelStart = null;
    await settle();
    expect(el.hasAttribute('label-start')).toBe(false);
    expect(el.labelStart).toBe('');
    expect(el.querySelector('button')!.textContent).toBe('Start');

    el.intent = { speed: 'max' };
    expect(el.intent).toEqual({ speed: 'max' });
    (el as unknown as { intent: unknown }).intent = undefined;
    expect(el.intent).toBeNull();
  });

  it('reflects vsil-time-controls properties including numeric and string stepMs', async () => {
    const front = fakeFront();
    const el = document.createElement('vsil-time-controls');
    el.client = front;
    el.speeds = '0.25 1 4';
    el.stepMs = 25;
    el.labelPause = 'P';
    el.labelResume = 'R';
    el.labelStep = 'S';
    el.labelRunUntil = 'U';
    document.body.append(el);
    await settle();

    expect(el.getAttribute('speeds')).toBe('0.25 1 4');
    expect(el.getAttribute('step-ms')).toBe('25');
    expect(el.stepMs).toBe('25');
    expect(el.querySelector('input')!.value).toBe('25');
    expect([...el.querySelectorAll('button')].map((b) => b.textContent)).toEqual([
      'P',
      'R',
      'S',
      'U',
    ]);

    el.stepMs = '50';
    await settle();
    expect(el.getAttribute('step-ms')).toBe('50');
    expect(el.querySelector('input')!.value).toBe('50');

    el.stepMs = 0;
    await settle();
    expect(el.getAttribute('step-ms')).toBe('0');
    expect(el.querySelector('input')!.value).toBe('0');

    el.stepMs = null;
    await settle();
    expect(el.hasAttribute('step-ms')).toBe(false);
    expect(el.stepMs).toBe('');
    expect(el.querySelector('input')!.value).toBe('10');
  });

  it('reflects vsil-value boolean noUnit, variable, on and off properties', async () => {
    const front = fakeFront();
    const el = document.createElement('vsil-value');
    el.client = front;
    el.variable = 'Dash.d.speed';
    el.noUnit = true;
    document.body.append(el);
    await settle();

    const cell = front.variables.get('Dash.d.speed')!.cell;
    cell.set(viewOf({ value: 80, text: '80', unit: 'km/h' }));
    expect(el.noUnit).toBe(true);
    expect(el.hasAttribute('no-unit')).toBe(true);
    expect(el.querySelector('output')!.textContent).toBe('80');

    el.noUnit = false;
    await settle();
    expect(el.noUnit).toBe(false);
    expect(el.hasAttribute('no-unit')).toBe(false);
    expect(el.querySelector('output')!.textContent).toBe('80 km/h');

    // Kebab-case attribute set to literal "false" (e.g. Vue `:no-unit="false"`) must not invert to true.
    el.setAttribute('no-unit', 'true');
    await settle();
    expect(el.noUnit).toBe(true);
    expect(el.querySelector('output')!.textContent).toBe('80');

    el.setAttribute('no-unit', 'false');
    await settle();
    expect(el.noUnit).toBe(false);
    expect(el.querySelector('output')!.textContent).toBe('80 km/h');

    el.setAttribute('no-unit', 'FALSE');
    await settle();
    expect(el.noUnit).toBe(false);
    expect(el.querySelector('output')!.textContent).toBe('80 km/h');

    el.removeAttribute('no-unit');
    await settle();

    el.on = 'YES';
    el.off = 'no';
    await settle();
    cell.set(viewOf({ value: true }));
    expect(el.querySelector('output')!.textContent).toBe('YES');
    cell.set(viewOf({ value: false }));
    expect(el.querySelector('output')!.textContent).toBe('no');
  });

  it('reflects vsil-slider variable property', async () => {
    const front = fakeFront();
    const el = document.createElement('vsil-slider');
    el.client = front;
    el.variable = 'Engine.e.throttle';
    document.body.append(el);
    await settle();

    expect(el.variable).toBe('Engine.e.throttle');
    expect(el.getAttribute('variable')).toBe('Engine.e.throttle');
    expect(front.variables.has('Engine.e.throttle')).toBe(true);
  });

  it('keeps falsy values (false, 0, "") on vsil-write.value without dropping them', async () => {
    const front = fakeFront();
    const el = document.createElement('vsil-write');
    el.client = front;
    el.variable = 'Engine.e.flag';
    el.value = false;
    el.label = 'Off';
    document.body.append(el);
    await settle();

    expect(el.getAttribute('value')).toBe('false');
    expect(el.value).toBe(false);
    expect(el.label).toBe('Off');
    front.variables.get('Engine.e.flag')!.cell.set(viewOf({ value: true }));
    el.querySelector('button')!.click();
    expect(front.variables.get('Engine.e.flag')!.enqueue).toHaveBeenLastCalledWith(false);

    el.value = 0;
    el.label = null;
    await settle();
    expect(el.getAttribute('value')).toBe('0');
    expect(el.value).toBe(0);
    expect(el.querySelector('button')!.textContent).toBe('Engine.e.flag = 0');
    el.querySelector('button')!.click();
    expect(front.variables.get('Engine.e.flag')!.enqueue).toHaveBeenLastCalledWith(0);

    el.value = '';
    await settle();
    expect(el.getAttribute('value')).toBe('');
    expect(el.value).toBe('');
    el.querySelector('button')!.click();
    expect(front.variables.get('Engine.e.flag')!.enqueue).toHaveBeenLastCalledWith('');

    el.value = null;
    await settle();
    expect(el.hasAttribute('value')).toBe(false);
    expect(el.value).toBeNull();
    front.variables.get('Engine.e.flag')!.cell.set(viewOf({ value: false }));
    el.querySelector('button')!.click();
    expect(front.variables.get('Engine.e.flag')!.enqueue).toHaveBeenLastCalledWith(true);
  });

  it('reflects vsil-connect network and file properties', async () => {
    const front = fakeFront();
    const el = document.createElement('vsil-connect');
    el.client = front;
    el.network = 'ETH0';
    el.file = 'custom.yaml';
    document.body.append(el);
    await settle();

    expect(el.network).toBe('ETH0');
    expect(el.file).toBe('custom.yaml');
    expect(el.getAttribute('network')).toBe('ETH0');
    expect(el.getAttribute('file')).toBe('custom.yaml');
    expect(el.dataset['network']).toBe('ETH0');

    el.file = '';
    await settle();
    expect(el.hasAttribute('file')).toBe(false);
    expect(el.file).toBe('');
  });
});
