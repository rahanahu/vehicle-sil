import { CheckParticipant, CheckResult } from '@vehicle-sil/gui-client';
import { open, text, view, addNetwork, removeNetwork, wire } from './designer-model';

interface Channel {
  name: string;
  kind?: string;
  use?: string;
  mac?: string | null;
}
interface Spec {
  name: string;
  channels?: Channel[];
  status?: string;
  describe?: boolean;
  map?: { name: string; from: string; line: number }[];
}

/** 照合の答え。画面が読むもの（`raw` の `reported`・`network_map`・`describe`、`variants`）だけ持つ。 */
function checkOf(
  participants: Spec[],
  extra: { variants?: string[]; variant?: string | null } = {},
): CheckResult {
  return {
    file: 'sim.yaml',
    result: 'passed',
    errors: 0,
    warnings: 0,
    notes: 0,
    stop_at_ms: null,
    findings: [],
    scenario: null,
    networks: [],
    participants: participants.map((p): CheckParticipant => {
      const described = (p.status ?? 'described') === 'described';
      return {
        name: p.name,
        line: null,
        required: true,
        enabled: true,
        manual: false,
        step_ms: null,
        status: p.status ?? 'described',
        variables: null,
        control: null,
        raw: {
          describe: p.describe ?? true,
          reported: described
            ? {
                networks: (p.channels ?? []).map((c) => ({
                  name: c.name,
                  kind: c.kind ?? 'ethernet',
                  use: c.use ?? 'ethernet',
                  nodes: [],
                  mac: c.mac ?? null,
                })),
              }
            : null,
          network_map: p.map ?? null,
        },
      };
    }),
    raw: {
      ...(extra.variants ? { variants: extra.variants } : {}),
      variant: extra.variant ?? null,
    },
  };
}

const START = `# kept comment
vehicle_sil_simulation: 1
networks: []
participants:
  - name: ECU_A
    command: [./a, eth0]   # from Alice
  - name: ECU_B
    command: [./b, "eth0,eth1"]
`;

const tree = (source: string) => open(source).doc.toJS() as Record<string, unknown>;

describe('designer model', () => {
  it('lists the channels as the executable names them, and the network each goes to', () => {
    const m = open(START);
    const v = view(
      m,
      checkOf([
        { name: 'ECU_A', channels: [{ name: 'eth0', mac: '02:00:00:00:00:01' }] },
        { name: 'ECU_B', channels: [{ name: 'eth0' }, { name: 'eth1' }] },
      ]),
    );
    expect(v.editable).toBe(true);
    expect(
      v.participants.map((p) => [p.name, p.channels.map((c) => `${c.from}->${c.network}`)]),
    ).toEqual([
      ['ECU_A', ['eth0->eth0']],
      ['ECU_B', ['eth0->eth0', 'eth1->eth1']],
    ]);
    expect(v.participants[0].channels[0].mac).toBe('02:00:00:00:00:01');
    expect(v.participants[1].channels[0].mac).toBeNull();
    expect(v.participants.every((p) => p.inCommon)).toBe(true);
  });

  it('merges channels that one executable names twice (can and j1939)', () => {
    const v = view(
      open(START),
      checkOf([
        {
          name: 'ECU_A',
          channels: [
            { name: 'CAN1', kind: 'can', use: 'can' },
            { name: 'CAN1', kind: 'can', use: 'j1939' },
          ],
        },
      ]),
    );
    expect(v.participants[0].channels).toHaveLength(1);
    expect(v.participants[0].channels[0].uses).toEqual(['can', 'j1939']);
  });

  it('designs two networks and three connections from participants only, and keeps the comments', () => {
    const m = open(START);
    addNetwork(m, 'ETH_AB', 'ethernet');
    addNetwork(m, 'ETH_EXT', 'ethernet');
    wire(m, 'ECU_A', 'eth0', 'ETH_AB');
    wire(m, 'ECU_B', 'eth0', 'ETH_AB');
    wire(m, 'ECU_B', 'eth1', 'ETH_EXT');
    const out = text(m);
    expect(out).toContain('# kept comment');
    expect(out).toContain('# from Alice');
    expect(out).not.toMatch(/[&*][a-z]/i);
    const t = tree(out);
    expect(t['networks']).toEqual([
      { name: 'ETH_AB', kind: 'ethernet' },
      { name: 'ETH_EXT', kind: 'ethernet' },
    ]);
    const names = (t['participants'] as { name: string; network_names?: unknown }[]).map(
      (p) => p.network_names,
    );
    expect(names).toEqual([{ ETH_AB: 'eth0' }, { ETH_AB: 'eth0', ETH_EXT: 'eth1' }]);
  });

  it('keeps a long line of the text on one line, and the flow style the author chose', () => {
    const long = `/very/long/${'directory/'.repeat(12)}ecu`;
    const source = `vehicle_sil_simulation: 1\nparticipants:\n  - name: ECU_A\n    command: ["${long}", eth0, 1]\n`;
    const m = open(source);
    wire(m, 'ECU_A', 'eth0', 'ETH_AB');
    expect(text(m)).toContain(`command: ["${long}", eth0, 1]`);
  });

  it('writes nothing for a connection to the same name, and removes the line that was there', () => {
    const m = open(START);
    wire(m, 'ECU_A', 'eth0', 'ETH_AB');
    expect(text(m)).toContain('network_names');
    wire(m, 'ECU_A', 'eth0', 'eth0');
    expect(text(m)).not.toContain('network_names');
    wire(m, 'ECU_B', 'eth0', 'eth0');
    expect(text(m)).not.toContain('network_names');
  });

  it('moves a connection to another network (one line per executable name)', () => {
    const m = open(START);
    wire(m, 'ECU_B', 'eth1', 'ETH_X');
    wire(m, 'ECU_B', 'eth1', 'ETH_Y');
    const b = (tree(text(m))['participants'] as { network_names?: unknown }[])[1];
    expect(b.network_names).toEqual({ ETH_Y: 'eth1' });
  });

  it('does not overwrite the line of another port of the participant', () => {
    const m = open(START);
    wire(m, 'ECU_B', 'eth0', 'ETH_AB');
    wire(m, 'ECU_B', 'eth1', 'ETH_EXT');
    const before = text(m);
    expect(() => wire(m, 'ECU_B', 'eth0', 'ETH_EXT')).toThrow(
      /ETH_EXT is already the name that eth1/,
    );
    expect(text(m)).toBe(before);
    // the same line again is not a conflict
    wire(m, 'ECU_B', 'eth1', 'ETH_EXT');
    expect(text(m)).toBe(before);
  });

  it('keeps the spelling of every plain value that the table did not touch', () => {
    const values = [
      '007',
      '+12',
      '0x1F',
      '1E3',
      '1e3',
      '.5',
      '100.',
      '0x8000000000000001',
      '18446744073709551615',
      '~',
      'true',
      'null',
    ];
    const source = `vehicle_sil_simulation: 1
networks:
  - name: 1
    kind: ethernet
participants:
  - name: 123
    command: [./ecu, ${values.join(', ')}]
  - name: ECU_B
    command: [./b, --id, 007]
`;
    const m = open(source);
    addNetwork(m, 'ETH_AB', 'ethernet');
    wire(m, '123', 'eth0', 'ETH_AB');
    removeNetwork(m, '1');
    const out = text(m);
    expect(out).toContain(`command: [./ecu, ${values.join(', ')}]`);
    expect(out).toContain('command: [./b, --id, 007]');
    expect(out).toContain('- name: 123');
    expect(out).not.toContain('name: 1\n');
    expect(tree(out)['networks']).toEqual([{ name: 'ETH_AB', kind: 'ethernet' }]);
  });

  it('quotes a new name that the launcher would read as something other than text', () => {
    const m = open(START);
    addNetwork(m, '123', 'ethernet');
    addNetwork(m, 'true', 'ethernet');
    wire(m, 'ECU_A', 'eth0', '0x1F');
    const out = text(m);
    expect(out).toContain('name: "123"');
    expect(out).toContain('name: "true"');
    expect(out).toContain('"0x1F": eth0');
    expect(tree(out)['networks']).toEqual([
      { name: '123', kind: 'ethernet' },
      { name: 'true', kind: 'ethernet' },
    ]);
  });

  it('keeps the line ends and the indentation of the text', () => {
    const crlf = START.replace(/\n/g, '\r\n');
    const m = open(crlf);
    wire(m, 'ECU_A', 'eth0', 'ETH_AB');
    const out = text(m);
    expect(out.replace(/\r\n/g, '')).not.toContain('\n');
    expect(out).toContain('\r\n');

    // a text whose sequences are flush with their key, and one whose maps use 4 spaces
    const flush = open(
      'vehicle_sil_simulation: 1\nparticipants:\n- name: ECU_A\n  command: [./a]\n',
    );
    wire(flush, 'ECU_A', 'eth0', 'ETH_AB');
    expect(text(flush)).toContain(
      'participants:\n- name: ECU_A\n  command: [./a]\n  network_names:\n    ETH_AB: eth0',
    );
    const four = open(
      'vehicle_sil_simulation: 1\nparticipants:\n    - name: ECU_A\n      command: [./a]\n',
    );
    wire(four, 'ECU_A', 'eth0', 'ETH_AB');
    expect(text(four)).toContain(
      'participants:\n    - name: ECU_A\n      command: [./a]\n      network_names:\n          ETH_AB: eth0',
    );
  });

  it('refuses to write for a participant that the common part does not have', () => {
    const m = open(START);
    expect(() => wire(m, 'Ghost', 'eth0', 'ETH_AB')).toThrow(/common part/);
  });

  it('removing a network also removes the lines of the common part that name it, not those of a variant', () => {
    const source = `${START}variants:\n  other:\n    participants:\n      ECU_A:\n        network_names:\n          ETH_AB: eth0\n`;
    const m = open(source);
    addNetwork(m, 'ETH_AB', 'ethernet');
    wire(m, 'ECU_A', 'eth0', 'ETH_AB');
    removeNetwork(m, 'ETH_AB');
    const t = tree(text(m));
    expect(t['networks']).toEqual([]);
    expect((t['participants'] as { network_names?: unknown }[])[0].network_names).toBeUndefined();
    expect(text(m)).toContain('ETH_AB: eth0'); // the line of the variant stays: the check says it
  });

  it('tells a line of a variant by its place in the tree, not by deriving the merge', () => {
    const source = `vehicle_sil_simulation: 1
networks:
  - name: ETH_AB
    kind: ethernet
participants:
  - name: ECU_B
    command: [./b]
    network_names:
      ETH_AB: eth0
variants:
  plain: {}
  ext2:
    participants:
      ECU_B:
        network_names:
          ETH_EXT: eth1
`;
    const m = open(source);
    const check = checkOf(
      [
        {
          name: 'ECU_B',
          channels: [{ name: 'eth0' }, { name: 'eth1' }],
          map: [
            { name: 'ETH_AB', from: 'eth0', line: 9 },
            { name: 'ETH_EXT', from: 'eth1', line: 16 },
          ],
        },
      ],
      { variants: ['plain', 'ext2'], variant: 'ext2' },
    );
    const v = view(m, check);
    expect(v.variants).toEqual(['plain', 'ext2']);
    expect(v.variant).toBe('ext2');
    expect(v.participants[0].channels.map((c) => [c.from, c.network, c.overriddenBy])).toEqual([
      ['eth0', 'ETH_AB', null],
      ['eth1', 'ETH_EXT', 'ext2'],
    ]);
  });

  it('does not know the channels of a participant that was not described', () => {
    const v = view(
      open(START),
      checkOf([
        {
          name: 'ECU_A',
          status: 'not_described',
          map: [{ name: 'ETH_AB', from: 'eth0', line: 8 }],
        },
        { name: 'ECU_B', status: 'not_checked', describe: false },
      ]),
    );
    expect(v.notDescribed).toEqual(['ECU_A']);
    expect(v.participants[0].known).toBe(false);
    expect(v.participants[0].channels).toEqual([]);
    expect(v.participants[0].mapLines).toEqual([{ name: 'ETH_AB', from: 'eth0', line: 8 }]);
    expect(v.participants[1].describeOff).toBe(true);
    expect(v.participants[1].known).toBe(false);
  });

  it('leaves a JSON configuration to the text pane', () => {
    expect(open('{"vehicle_sil_simulation": 1}').editable).toBe(false);
    expect(open('\n  {"vehicle_sil_simulation": 1}').json).toBe(true);
    expect(open('vehicle_sil_simulation: 1\n').editable).toBe(true);
  });

  it('reports a text that is not YAML, with its line, and stops the tables', () => {
    const m = open('vehicle_sil_simulation: 1\nnetworks: [a\nparticipants: x: y\n');
    expect(m.editable).toBe(false);
    expect(m.errors.length).toBeGreaterThan(0);
    expect(m.errors[0].line).not.toBeNull();
  });
});
