import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { CheckResult, ConnectEntry, Front, Outside, Readable } from '@vehicle-sil/gui-client';
import { pageIntent } from '@vehicle-sil/gui-client/elements';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { GUI_FRONT } from '../core/session/gui-client';
import { OutsideChoice, OutsideRows, connectChoice, connectText } from './outside-connect';

/** 試験が値を決める、読める値。 */
class Cell<T> implements Readable<T> {
  private readonly listeners = new Set<(value: T) => void>();
  constructor(private value: T) {}
  get = (): T => this.value;
  subscribe = (listener: (value: T) => void): (() => void) => {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  };
  set(value: T): void {
    this.value = value;
    for (const listener of [...this.listeners]) listener(value);
  }
}

const network = (name: string | null, kind: string | null) => ({ name, kind, participants: [] });

/** 照合の結果のうち、この部品が読む networks[] だけのもの。 */
const checkWith = (networks: ReturnType<typeof network>[]) =>
  ({ networks, findings: [] }) as unknown as CheckResult;

const target = (value: string, detail: string, usable = true): ConnectEntry => ({
  value,
  label: value,
  detail,
  usable,
  reason: usable ? null : 'not Ethernet',
  backend: 'packet',
});

/** 部品が使う正面の口の部分（状態は Cell、操作は spy）。 */
function fakeFront() {
  const outside = new Cell<readonly Outside[]>([]);
  const front = {
    // 起動のときのファイルは別: 部品は、画面が選んだファイルの一覧を取る
    status: new Cell({ file: 'start-up.yaml' }),
    outside,
    connections: vi.fn(async (): Promise<ConnectEntry[]> => []),
    check: vi.fn(async () => ({ findings: [], raw: {} }) as unknown as CheckResult),
  };
  return { front, outside };
}

@Component({
  imports: [OutsideChoice],
  template: `<app-outside-choice [check]="check()" [file]="'connect.yaml'" />`,
})
class ChoiceHost {
  readonly check = signal<CheckResult | null>(null);
  readonly connect = connectChoice();
}

describe('OutsideChoice', () => {
  let fixture: ComponentFixture<ChoiceHost>;
  let fake: ReturnType<typeof fakeFront>;
  const element = () => fixture.nativeElement as HTMLElement;
  const part = (name: string) =>
    element().querySelector<HTMLElement & { client: unknown }>(
      `[data-testid="connect-${name}"] vsil-connect`,
    );

  beforeEach(async () => {
    fake = fakeFront();
    TestBed.configureTestingModule({
      providers: [{ provide: GUI_FRONT, useValue: fake.front as unknown as Front }],
    });
    fixture = TestBed.createComponent(ChoiceHost);
    await fixture.whenStable();
  });

  afterEach(() => {
    fixture.destroy();
    pageIntent.update({ connect: undefined });
  });

  it('puts a choice for each can and ethernet network, on the screen connection, and lists none until opened', async () => {
    expect(element().querySelector('app-outside-choice h3')).toBeNull();
    fixture.componentInstance.check.set(
      checkWith([
        network('J1939_CAN', 'can'),
        network('LIN1', 'lin'),
        network('ETH_EXT', 'ethernet'),
        network(null, 'can'),
      ]),
    );
    await fixture.whenStable();

    expect(element().textContent).toContain('GUI サーバーが動いている PC');
    const names = [...element().querySelectorAll('vsil-connect')].map((p) =>
      p.getAttribute('network'),
    );
    expect(names).toEqual(['J1939_CAN', 'ETH_EXT']);
    const eth = part('ETH_EXT')!;
    // 画面の接続を使う（部品が自分の接続を開いたら、トークンがないので誤りになる）
    expect(eth.client).toBe(fake.front);
    expect(eth.hasAttribute('data-error')).toBe(false);
    expect(eth.lang).toBe('en');
    const select = eth.querySelector('select')!;
    expect(select.value).toBe('none');
    expect(eth.getAttribute('data-chosen')).toBe('none');
    // 選択の名前は、見出しのネットワーク名から
    expect(select.closest('label')?.textContent).toContain('ETH_EXT');
    // 開くまでは一覧を取らない
    expect(fake.front.connections).not.toHaveBeenCalled();
  });

  it('shows the detail of a target that another bridge uses, and gives the choice to the screen', async () => {
    fixture.componentInstance.check.set(checkWith([network('ETH_EXT', 'ethernet')]));
    await fixture.whenStable();
    fake.front.connections.mockResolvedValue([
      target('fake0', 'up, connected, in use by a vehicle-sil bridge (process 4321)'),
      target('tap:lo', 'loopback', false),
    ]);
    const select = part('ETH_EXT')!.querySelector('select')!;
    select.dispatchEvent(new Event('pointerdown'));
    await vi.waitFor(() => expect(select.querySelector('option[value="fake0"]')).not.toBeNull());
    expect(fake.front.connections).toHaveBeenCalledWith(
      { network: 'ETH_EXT' },
      { file: 'connect.yaml' },
    );
    expect(select.querySelector('option[value="fake0"]')?.textContent).toContain(
      'in use by a vehicle-sil bridge (process 4321)',
    );
    expect(select.querySelector<HTMLOptionElement>('option[value="tap:lo"]')?.disabled).toBe(true);

    select.value = 'fake0';
    select.dispatchEvent(new Event('change'));
    expect(fixture.componentInstance.connect()).toEqual({ ETH_EXT: 'fake0' });

    // ファイルを替えて照合がなくなったら、部品が外れ、選択も消える（黙ってつながない）
    fixture.componentInstance.check.set(null);
    await fixture.whenStable();
    expect(element().querySelector('vsil-connect')).toBeNull();
    expect(fixture.componentInstance.connect()).toEqual({});
  });

  it('keeps the parts and the choice while the same networks are checked again', async () => {
    fixture.componentInstance.check.set(checkWith([network('ETH_EXT', 'ethernet')]));
    await fixture.whenStable();
    const before = part('ETH_EXT');
    pageIntent.update({ connect: { ETH_EXT: 'fake0' } });
    fixture.componentInstance.check.set(checkWith([network('ETH_EXT', 'ethernet')]));
    await fixture.whenStable();
    expect(part('ETH_EXT')).toBe(before);
    expect(fixture.componentInstance.connect()).toEqual({ ETH_EXT: 'fake0' });
  });
});

@Component({
  imports: [OutsideRows],
  template: `<app-outside-rows />`,
})
class RowsHost {}

describe('OutsideRows', () => {
  it('shows a card with a row for each connected network, and nothing without one', async () => {
    const fake = fakeFront();
    TestBed.configureTestingModule({
      providers: [{ provide: GUI_FRONT, useValue: fake.front as unknown as Front }],
    });
    const fixture = TestBed.createComponent(RowsHost);
    await fixture.whenStable();
    const element = fixture.nativeElement as HTMLElement;
    expect(element.querySelector('[data-testid="outside-card"]')).toBeNull();

    fake.outside.set([
      {
        network: 'ETH_EXT',
        target: 'fake0',
        participant: 'bridge.ETH_EXT',
        link: 'up',
        bus: null,
        condition: 'degraded',
        reasons: ['frames-dropped'],
        nothing_received: true,
        to_outside: 3,
        from_outside: 0,
      } as Outside,
    ]);
    await fixture.whenStable();
    expect(element.querySelector('h2')?.textContent).toContain('外につないだネットワーク');
    const row = element.querySelector('vsil-outside li');
    expect(row?.getAttribute('data-network')).toBe('ETH_EXT');
    expect(row?.textContent).toContain('fake0');
    expect(row?.textContent).toContain('frames-dropped');
    expect(row?.textContent).toContain('nothing received');
    expect(element.querySelector('vsil-outside')?.hasAttribute('data-error')).toBe(false);

    fake.outside.set([]);
    await fixture.whenStable();
    expect(element.querySelector('[data-testid="outside-card"]')).toBeNull();
  });
});

describe('connectText', () => {
  it('lists each network and its target, and is null without one', () => {
    const list = new Intl.ListFormat('en', { type: 'conjunction' });
    expect(connectText({}, list)).toBeNull();
    expect(connectText({ ETH_EXT: 'fake0', CAN1: 'vcan0' }, list)).toBe(
      'ETH_EXT → fake0 and CAN1 → vcan0',
    );
  });
});
