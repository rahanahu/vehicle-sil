import { DashboardFile, VariableInfo, VariablePeer } from '@vehicle-sil/gui-client';
import {
  WIDGET_KINDS,
  autoDashboard,
  checkDashboard,
  checkItem,
  countItems,
  defaultWidget,
  fileDashboard,
  filterDashboard,
  isMonitoring,
} from './dashboard-spec';
import { valueCondition, writeBlock } from './variable-state';

const info = (fields: Partial<VariableInfo>): VariableInfo => ({
  name: 'x',
  type: 'real',
  role: 'input',
  writable: true,
  unit: '',
  min: null,
  max: null,
  step: null,
  description: '',
  enum: null,
  ...fields,
});

const OUTPUT = { role: 'output', writable: false } as const;
const ITEMS = [
  { name: 'off', value: 0 },
  { name: 'on', value: 1 },
];

describe('defaultWidget (the table in docs/gui.md)', () => {
  it.each([
    ['bool, writable', info({ type: 'bool' }), 'switch'],
    ['bool parameter', info({ type: 'bool', role: 'parameter' }), 'switch'],
    ['bool, read only', info({ type: 'bool', ...OUTPUT }), 'lamp'],
    ['enum, writable', info({ type: 'enum', enum: ITEMS }), 'select'],
    ['enum, read only', info({ type: 'enum', enum: ITEMS, ...OUTPUT }), 'value'],
    ['enum without items', info({ type: 'enum', enum: [] }), 'value'],
    ['real with a range', info({ min: 0, max: 100 }), 'slider'],
    ['integer with a range', info({ type: 'integer', min: -5, max: 5 }), 'slider'],
    ['real with one end', info({ min: 0 }), 'number'],
    ['real with an empty range', info({ min: 3, max: 3 }), 'number'],
    ['integer without a range', info({ type: 'integer' }), 'number'],
    ['real, read only', info({ ...OUTPUT, min: 0, max: 100 }), 'graph'],
    ['integer, read only', info({ type: 'integer', ...OUTPUT }), 'graph'],
    ['a type it does not know', info({ type: 'string' }), 'value'],
  ] as const)('%s', (_, variable, widget) => {
    expect(defaultWidget(variable)).toBe(widget);
  });
});

describe('the automatic dashboard', () => {
  const peers: VariablePeer[] = [
    {
      name: 'Engine',
      connected: true,
      variables: [
        info({ name: 'engine.throttle', min: 0, max: 100 }),
        info({ name: 'engine.speed', type: 'integer', ...OUTPUT }),
        info({ name: 'peer.steps', type: 'integer', ...OUTPUT }),
        info({ name: 'j1939.J1939_CAN.frames_sent', type: 'integer', ...OUTPUT }),
      ],
    },
    { name: 'Logger', connected: true, variables: [] },
    {
      name: 'Dashboard',
      connected: true,
      variables: [info({ name: 'dashboard.engine_speed', type: 'integer', ...OUTPUT })],
    },
  ];

  const monitoring = (peer: string) => `${peer} monitoring`;

  it('puts the variables of each ECU together, in the order they were declared, with their default widgets', () => {
    expect(autoDashboard(peers, monitoring)).toEqual({
      sections: [
        {
          title: 'Engine',
          items: [
            { peer: 'Engine', name: 'engine.throttle', widget: 'slider' },
            { peer: 'Engine', name: 'engine.speed', widget: 'graph' },
          ],
        },
        // ECU が自分で足す監視の変数は、畳んだ別のまとまり
        {
          title: 'Engine monitoring',
          collapsed: true,
          items: [
            { peer: 'Engine', name: 'peer.steps', widget: 'graph' },
            { peer: 'Engine', name: 'j1939.J1939_CAN.frames_sent', widget: 'graph' },
          ],
        },
        {
          title: 'Dashboard',
          items: [{ peer: 'Dashboard', name: 'dashboard.engine_speed', widget: 'graph' }],
        },
      ],
    });
  });

  it('filters by name: every word, in the ECU or the variable, in any case', () => {
    const all = autoDashboard(peers, monitoring);
    expect(filterDashboard(all, '   ')).toBe(all);
    expect(countItems(filterDashboard(all, 'SPEED'))).toBe(2);
    // 畳んだまとまりの中も探す
    expect(filterDashboard(all, 'frames')).toEqual({
      sections: [
        {
          title: 'Engine monitoring',
          collapsed: true,
          items: [{ peer: 'Engine', name: 'j1939.J1939_CAN.frames_sent', widget: 'graph' }],
        },
      ],
    });
    expect(filterDashboard(all, 'engine speed').sections.map((s) => s.title)).toEqual([
      'Engine',
      'Dashboard',
    ]);
    expect(filterDashboard(all, 'dashboard speed')).toEqual({
      sections: [
        {
          title: 'Dashboard',
          items: [{ peer: 'Dashboard', name: 'dashboard.engine_speed', widget: 'graph' }],
        },
      ],
    });
    expect(filterDashboard(all, 'brake')).toEqual({ sections: [] });
  });

  it('copes with hundreds of variables', () => {
    const many: VariablePeer[] = [
      {
        name: 'Big',
        connected: true,
        variables: Array.from({ length: 800 }, (_, i) => info({ name: `big.v${i}` })),
      },
    ];
    const dashboard = autoDashboard(many, monitoring);
    expect(countItems(dashboard)).toBe(800);
    expect(countItems(filterDashboard(dashboard, 'v79'))).toBe(11); // v79, v790..v799
  });
});

describe('which variables are the monitoring ones, and whether a widget fits a variable', () => {
  const item = (widget: string, options?: Record<string, unknown>) =>
    ({ peer: 'E', name: 'x', widget, options }) as Parameters<typeof checkItem>[0];

  it('tells the monitoring variables by their names', () => {
    expect(isMonitoring('peer.steps')).toBe(true);
    expect(isMonitoring('j1939.J1939_CAN.node0.address')).toBe(true);
    expect(isMonitoring('engine.speed')).toBe(false);
    expect(isMonitoring('peerless')).toBe(false);
  });

  it('refuses a widget that does not fit the type, a writing widget on an output, and wrong options', () => {
    expect(checkItem(item('switch'), info({ type: 'real' }))).toEqual({
      kind: 'wrong-type',
      types: ['bool'],
    });
    expect(checkItem(item('switch'), info({ type: 'bool', ...OUTPUT }))).toEqual({
      kind: 'read-only',
    });
    expect(checkItem(item('lamp'), info({ type: 'bool' }))).toBeNull();
    expect(checkItem(item('slider'), info({ min: 0 }))).toEqual({ kind: 'no-range' });
    expect(checkItem(item('slider', { step: 5 }), info({ min: 0, max: 100 }))).toBeNull();
    expect(checkItem(item('slider', { step: -1 }), info({ min: 0, max: 100 }))).toEqual({
      kind: 'bad-option',
      key: 'step',
    });
    expect(checkItem(item('graph', { colour: 'red' }), info({}))).toEqual({
      kind: 'unknown-option',
      key: 'colour',
    });
    expect(checkItem(item('select'), info({ type: 'enum', enum: [] }))).toEqual({
      kind: 'no-items',
    });
    expect(checkItem(item('gauge'), info({}))).toEqual({ kind: 'unknown-widget' });
  });

  it.each(['constructor', '__proto__', 'toString', 'hasOwnProperty'])(
    'a widget or an option named %s is unknown (not a key of the object prototype)',
    (name) => {
      expect(checkItem(item(name), info({}))).toEqual({ kind: 'unknown-widget' });
      expect(checkItem(item(name), undefined)).toEqual({ kind: 'unknown-widget' });
      // JSON.parse makes __proto__ an own key, as the server's answer does
      const options = JSON.parse(`{"${name}": 1}`) as Record<string, unknown>;
      expect(checkItem(item('graph', options), info({}))).toEqual({
        kind: 'unknown-option',
        key: name,
      });
    },
  );

  it('the rest of checkItem', () => {
    // いまの実行にない変数: 設定だけを見る（部品が「この実行にない変数」と出す）
    expect(checkItem(item('switch'), undefined)).toBeNull();
    expect(
      checkDashboard(
        {
          sections: [
            { title: 'a', items: [item('value'), item('switch')] },
            { title: 'b', items: [item('number')] },
          ],
        },
        () => info({ type: 'bool', ...OUTPUT }),
      ),
    ).toEqual([
      { section: 0, item: 1, problem: { kind: 'read-only' } },
      { section: 1, item: 0, problem: { kind: 'wrong-type', types: ['integer', 'real'] } },
    ]);
  });
});

describe('the condition of a value and whether it can be written', () => {
  const reading = { value: 1, sampleMs: 100, simMs: 100, paused: false };
  const base = { status: 'ready', known: true, connected: true, peerConnected: true } as const;

  it('tells a value that is missing, waited for, live, of a stopped time, old or the last of a run', () => {
    expect(valueCondition({ ...base, status: 'off', known: false, reading: undefined }).kind).toBe(
      'unavailable',
    );
    expect(valueCondition({ ...base, known: false, reading: undefined }).kind).toBe('missing');
    expect(valueCondition({ ...base, reading: undefined }).kind).toBe('waiting');
    expect(valueCondition({ ...base, reading }).kind).toBe('live');
    expect(valueCondition({ ...base, reading: { ...reading, paused: true } }).kind).toBe('paused');
    expect(valueCondition({ ...base, reading, connected: false })).toMatchObject({
      kind: 'stale',
      why: 'connection',
    });
    expect(valueCondition({ ...base, reading, status: 'connecting' })).toMatchObject({
      kind: 'stale',
      why: 'reconnecting',
    });
    expect(valueCondition({ ...base, reading, peerConnected: false })).toMatchObject({
      kind: 'stale',
      why: 'peer',
    });
    expect(valueCondition({ ...base, reading, status: 'ended', peerConnected: false }).kind).toBe(
      'ended',
    );
  });

  it('lets a parameter be written only before the first step', () => {
    const parameter = info({ role: 'parameter' });
    const ready = { status: 'ready', connected: true, peerConnected: true } as const;
    expect(writeBlock({ ...ready, info: parameter, beforeStart: true })).toBeNull();
    expect(writeBlock({ ...ready, info: parameter, beforeStart: false })).toBe('parameter');
    expect(writeBlock({ ...ready, info: info({}), beforeStart: false })).toBeNull();
    expect(writeBlock({ ...ready, info: info(OUTPUT), beforeStart: true })).toBe('read-only');
    expect(writeBlock({ ...ready, info: undefined, beforeStart: true })).toBe('missing');
    expect(writeBlock({ ...ready, peerConnected: false, info: info({}), beforeStart: true })).toBe(
      'peer-disconnected',
    );
    expect(writeBlock({ ...ready, connected: false, info: info({}), beforeStart: true })).toBe(
      'disconnected',
    );
    expect(writeBlock({ ...ready, status: 'ended', info: info({}), beforeStart: false })).toBe(
      'ended',
    );
    expect(writeBlock({ ...ready, status: 'failed', info: info({}), beforeStart: false })).toBe(
      'not-ready',
    );
  });
});

describe('fileDashboard (a dashboard file, as the server checked it)', () => {
  const peers: VariablePeer[] = [
    {
      name: 'Engine',
      connected: true,
      variables: [
        info({ name: 'engine.throttle', min: 0, max: 100 }),
        info({ name: 'engine.speed', type: 'integer', ...OUTPUT }),
        info({ name: 'engine.mode', type: 'enum', enum: ITEMS }),
        info({ name: 'peer.steps', type: 'integer', ...OUTPUT }),
      ],
    },
    {
      name: 'Dashboard',
      connected: true,
      variables: [info({ name: 'dashboard.lamp', type: 'bool', ...OUTPUT })],
    },
  ];
  const file: DashboardFile = {
    title: 'Bench',
    panels: [{ module: 'panels/rpm-bar.js', url: 'panels/KEY/panels/rpm-bar.js' }],
    sections: [
      {
        title: 'Engine',
        collapsed: false,
        items: [
          {
            variable: { peer: 'Engine', name: 'engine.throttle' },
            widget: 'number',
            label: 'Throttle',
            options: {},
          },
          {
            variable: { peer: 'Engine', name: 'engine.gone' },
            widget: null,
            label: null,
            options: {},
          },
          {
            panel: 'rpm-bar',
            label: null,
            options: { max: 4000 },
            variables: [{ peer: 'Engine', name: 'engine.speed' }],
          },
          { auto: 'Engine' },
          { auto: 'Engine' },
        ],
      },
      {
        title: 'Others',
        collapsed: true,
        items: [{ auto: 'Dashboard' }, { auto: 'Nobody' }],
      },
    ],
  };

  it('places what the file says, and auto places the rest of a participant', () => {
    const spec = fileDashboard(file, peers, (peer) => `${peer} monitoring`);
    expect(spec.title).toBe('Bench');
    expect(spec.modules).toEqual(file.panels);
    expect(spec.sections).toEqual([
      {
        title: 'Engine',
        items: [
          { peer: 'Engine', name: 'engine.throttle', widget: 'number', label: 'Throttle' },
          // not in this run: the value widget says so
          { peer: 'Engine', name: 'engine.gone', widget: 'value' },
          {
            panel: 'rpm-bar',
            options: { max: 4000 },
            variables: [{ peer: 'Engine', name: 'engine.speed' }],
          },
          // engine.speed is the panel's: not placed again; the second auto places nothing
          { peer: 'Engine', name: 'engine.mode', widget: 'select' },
        ],
      },
      {
        title: 'Engine monitoring',
        collapsed: true,
        items: [{ peer: 'Engine', name: 'peer.steps', widget: 'graph' }],
      },
      {
        title: 'Others',
        collapsed: true,
        // a participant that this run does not have: said in place
        items: [
          { peer: 'Dashboard', name: 'dashboard.lamp', widget: 'lamp' },
          { missing: 'Nobody' },
        ],
      },
    ]);
  });

  it('keeps panels through the name filter and the check', () => {
    const spec = fileDashboard(file, peers, (peer) => `${peer} monitoring`);
    expect(countItems(filterDashboard(spec, 'rpm'))).toBe(1);
    expect(countItems(filterDashboard(spec, 'engine.speed'))).toBe(1);
    expect(filterDashboard(spec, 'rpm').modules).toEqual(spec.modules);
    expect(checkDashboard(spec, () => undefined)).toEqual([]);
  });

  it('the JSON Schema (gui/protocol) lists the same widgets as the screen', async () => {
    // node: の組み込みは、試験の束ねに含めず、実行のときに読む（testing/protocol-examples.ts と同じ）
    const fsName = 'node:fs';
    const fs = (await import(/* @vite-ignore */ fsName)) as {
      readFileSync(path: string, encoding: 'utf8'): string;
    };
    const cwd = (globalThis as unknown as { process: { cwd(): string } }).process.cwd();
    const schema = JSON.parse(
      fs.readFileSync(`${cwd}/../protocol/dashboard.schema.json`, 'utf8'),
    ) as {
      $defs: { item: { oneOf: { properties: { widget?: { enum: string[] } } }[] } };
    };
    expect(schema.$defs.item.oneOf[0].properties.widget?.enum).toEqual([...WIDGET_KINDS]);
  });
});

describe('fileDashboard before a run', () => {
  it('says nothing about auto: while there are no variables yet', () => {
    const spec = fileDashboard(
      {
        title: null,
        panels: [],
        sections: [{ title: 'A', collapsed: false, items: [{ auto: 'Engine' }] }],
      },
      [],
      (peer) => peer,
    );
    expect(spec.sections).toEqual([{ title: 'A', items: [] }]);
  });
});
