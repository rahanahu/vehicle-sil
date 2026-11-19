import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { FakeWebSockets, protocolExamples } from '@vehicle-sil/gui-client/testing';
import { authInterceptor } from '../core/auth/auth-interceptor';
import { SOCKET_FACTORY } from '../core/session/gui-client';
import { BATCH_MS } from '@vehicle-sil/gui-client';
import { DESIGN_CHECK_DELAY_MS, DesignPage } from './design-page';

describe('DesignPage', () => {
  let examples: Map<string, unknown>;
  let http: HttpTestingController;
  let fake: FakeWebSockets;

  beforeAll(async () => {
    examples = await protocolExamples();
  });

  const server = (name: string) =>
    structuredClone((examples.get(name) as { server: unknown[] }).server);
  const response = (name: string) =>
    structuredClone((examples.get(name) as { response: object }).response);
  const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
  /** 要求の応答を待たずに、画面を更新する（応答待ちの要求があると whenStable は終わらない）。 */
  const settle = async () => {
    await sleep(20);
    TestBed.tick();
  };

  beforeEach(async () => {
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    fake = new FakeWebSockets();
    await TestBed.configureTestingModule({
      imports: [DesignPage],
      providers: [
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: fake.factory },
      ],
    }).compileComponents();
    http = TestBed.inject(HttpTestingController);
  });

  afterEach(() => sessionStorage.clear());

  const element = (f: ComponentFixture<DesignPage>) => f.nativeElement as HTMLElement;
  const text = (f: ComponentFixture<DesignPage>, id: string) =>
    (element(f).querySelector(`[data-testid="${id}"]`)?.textContent ?? '')
      .replace(/\s+/g, ' ')
      .trim();

  /** 保存してある文と、その照合の答え。`mutate` で答えを変える。 */
  async function openWith(
    mutate: (check: Record<string, unknown>) => void = () => undefined,
    save?: { ok: boolean; reason: string | null; message: string | null; caution: unknown },
  ) {
    const fixture = TestBed.createComponent(DesignPage);
    TestBed.tick();
    // the server's hello: what it allows (save too)
    fake.last().open();
    const messages = server('ws-pass') as { allowed?: { save: unknown } }[];
    if (save) for (const m of messages) if (m.allowed) m.allowed.save = save;
    for (const m of messages.slice(0, 3)) fake.last().send(m);
    await sleep(BATCH_MS + 20);
    http.expectOne('api/app').flush(response('app'));
    http.expectOne('api/configs').flush(response('configs'));
    await settle();
    http.expectOne((r) => r.url.startsWith('api/config?')).flush(response('config'));
    await settle();
    await sleep(DESIGN_CHECK_DELAY_MS + 20);
    const body = response('check-text') as { check: Record<string, unknown> };
    mutate(body.check);
    http.expectOne('api/check').flush(body);
    await settle();
    return fixture;
  }

  it('builds the tables from the check of the text, and puts the findings on their rows', async () => {
    const fixture = await openWith((check) => {
      check['findings'] = [
        {
          severity: 'warning',
          code: 'single-participant',
          file: 'bad-lines.yaml',
          line: 6,
          participant: null,
          network: 'J1939_CAN',
          message: 'network J1939_CAN: only one participant is on it',
          detail: null,
        },
      ];
    });
    expect(text(fixture, 'network-J1939_CAN')).toContain('J1939_CAN');
    expect(text(fixture, 'network-J1939_CAN')).toContain('single-participant');
    expect(element(fixture).querySelectorAll('[data-testid="finding"]').length).toBe(1);
    expect(element(fixture).querySelector('[data-testid="not-described"]')).toBeNull();
    expect(
      (element(fixture).querySelector('[data-testid="design-text"]') as HTMLTextAreaElement).value,
    ).toContain('vehicle_sil_simulation: 1');
  });

  it('says that a passed check did not compare a participant it could not describe, and describes again', async () => {
    const fixture = await openWith((check) => {
      (check['participants'] as { status: string }[])[0].status = 'not_described';
    });
    expect(text(fixture, 'not-described')).toContain('申告を読んでいない participant が 1 個');
    expect(
      element(fixture).querySelector('[data-testid="design-outcome"]')?.getAttribute('data-result'),
    ).toBe('passed');
    (element(fixture).querySelector('[data-testid="describe-again"]') as HTMLButtonElement).click();
    TestBed.tick();
    const described = http.expectOne('api/describe');
    expect(described.request.body).toMatchObject({ file: 'bad-lines.yaml' });
  });

  it('sends a participant that the saved file does not have to saving, not to describing again', async () => {
    const fixture = await openWith((check) => {
      const participants = check['participants'] as { name: string; status: string }[];
      participants[0].name = 'AddedInText';
      participants[0].status = 'not_described';
    });
    expect(text(fixture, 'save-first-names')).toContain('AddedInText');
    expect(element(fixture).querySelector('[data-testid="describe-again"]')).toBeNull();
  });

  it('asks before an edit is thrown away by reading the file again', async () => {
    const fixture = await openWith();
    const area = element(fixture).querySelector(
      '[data-testid="design-text"]',
    ) as HTMLTextAreaElement;
    area.value = 'a: 1\n';
    area.dispatchEvent(new Event('input'));
    TestBed.tick();
    const confirm = vi.spyOn(window, 'confirm').mockReturnValue(false);
    (element(fixture).querySelector('[data-testid="design-reload"]') as HTMLButtonElement).click();
    TestBed.tick();
    expect(confirm).toHaveBeenCalledOnce();
    http.expectNone((r) => r.url.startsWith('api/config?'));
    confirm.mockReturnValue(true);
    (element(fixture).querySelector('[data-testid="design-reload"]') as HTMLButtonElement).click();
    TestBed.tick();
    http.expectOne((r) => r.url.startsWith('api/config?'));
    confirm.mockRestore();
  });

  it('does not say that it is checking after the check failed', async () => {
    const fixture = TestBed.createComponent(DesignPage);
    TestBed.tick();
    http.expectOne('api/app').flush(response('app'));
    http.expectOne('api/configs').flush(response('configs'));
    await settle();
    http.expectOne((r) => r.url.startsWith('api/config?')).flush(response('config'));
    await settle();
    await sleep(DESIGN_CHECK_DELAY_MS + 20);
    http
      .expectOne('api/check')
      .flush(
        { error: { code: 'check-failed', message: 'the launcher failed' } },
        { status: 500, statusText: 'x' },
      );
    await settle();
    expect(text(fixture, 'design-check-error')).not.toBe('');
    expect(element(fixture).textContent).not.toContain('照合しています');
  });

  it('checks the text again after an edit, once, and sends no more than the last text', async () => {
    const fixture = await openWith();
    const area = element(fixture).querySelector(
      '[data-testid="design-text"]',
    ) as HTMLTextAreaElement;
    for (const edited of ['a: 1\n', 'a: 2\n', 'a: 3\n']) {
      area.value = edited;
      area.dispatchEvent(new Event('input'));
      TestBed.tick();
    }
    await sleep(DESIGN_CHECK_DELAY_MS + 20);
    const check = http.expectOne('api/check');
    expect(check.request.body).toMatchObject({ file: 'bad-lines.yaml', text: 'a: 3\n' });
    expect(text(fixture, 'design-outcome')).toContain('照合しています');
  });

  it('adds a network to the text through the table', async () => {
    const fixture = await openWith();
    const name = element(fixture).querySelector('[data-testid="network-name"]') as HTMLInputElement;
    name.value = 'ETH_AB';
    name.dispatchEvent(new Event('input'));
    TestBed.tick();
    (element(fixture).querySelector('[data-testid="network-add"]') as HTMLButtonElement).click();
    TestBed.tick();
    const area = element(fixture).querySelector(
      '[data-testid="design-text"]',
    ) as HTMLTextAreaElement;
    expect(area.value).toContain('name: ETH_AB');
    expect(area.value).toContain('# A configuration of the GUI server');
  });

  /** 文を直して（ネットワークを足して）、保存を押す。 */
  async function editAndSave(fixture: ComponentFixture<DesignPage>) {
    const name = element(fixture).querySelector('[data-testid="network-name"]') as HTMLInputElement;
    name.value = 'ETH_AB';
    name.dispatchEvent(new Event('input'));
    TestBed.tick();
    (element(fixture).querySelector('[data-testid="network-add"]') as HTMLButtonElement).click();
    TestBed.tick();
    await settle();
    (element(fixture).querySelector('[data-testid="save"]') as HTMLButtonElement).click();
    TestBed.tick();
  }

  it('saves the text with the stamp that it read, and keeps what was typed while it waited', async () => {
    const fixture = await openWith();
    const stamp = (response('config') as { stamp: string }).stamp;
    await editAndSave(fixture);
    const saved = http.expectOne((r) => r.method === 'POST' && r.url === 'api/config');
    expect(saved.request.body).toMatchObject({ file: 'bad-lines.yaml', stamp });
    expect((saved.request.body as { text: string }).text).toContain('name: ETH_AB');
    // typed while the answer was awaited
    const area = element(fixture).querySelector(
      '[data-testid="design-text"]',
    ) as HTMLTextAreaElement;
    area.value = `${area.value}# typed while saving\n`;
    area.dispatchEvent(new Event('input'));
    TestBed.tick();
    saved.flush({ file: 'bad-lines.yaml', stamp: 'f'.repeat(64) });
    await sleep(100);
    TestBed.tick();
    http.expectNone((r) => r.method === 'GET' && r.url.startsWith('api/config?'));
    expect(area.value).toContain('# typed while saving');
    // the edit after the save is a change again, so the note of the save is gone
    expect(text(fixture, 'saved')).toBe('');
  });

  it('says that it saved, until the next edit', async () => {
    const fixture = await openWith();
    await editAndSave(fixture);
    http
      .expectOne((r) => r.method === 'POST' && r.url === 'api/config')
      .flush({ file: 'bad-lines.yaml', stamp: 'f'.repeat(64) });
    await sleep(100);
    TestBed.tick();
    expect(text(fixture, 'saved')).not.toBe('');
    const area = element(fixture).querySelector(
      '[data-testid="design-text"]',
    ) as HTMLTextAreaElement;
    area.value = `${area.value}# more\n`;
    area.dispatchEvent(new Event('input'));
    TestBed.tick();
    expect(text(fixture, 'saved')).toBe('');
  });

  it('does not offer describing again for a participant of the saved file that describing again did not fix', async () => {
    const stuckCheck = (check: Record<string, unknown>) => {
      (check['participants'] as { status: string }[])[0].status = 'not_described';
    };
    const fixture = await openWith(stuckCheck);
    const name = (response('check-text') as { check: { participants: { name: string }[] } }).check
      .participants[0].name;
    expect(text(fixture, 'reread-names')).toContain(name);
    (element(fixture).querySelector('[data-testid="describe-again"]') as HTMLButtonElement).click();
    TestBed.tick();
    http.expectOne('api/describe').flush(response('describe'));
    await sleep(DESIGN_CHECK_DELAY_MS + 100);
    TestBed.tick();
    const body = response('check-text') as { check: Record<string, unknown> };
    stuckCheck(body.check);
    http.expectOne('api/check').flush(body);
    await settle();
    expect(text(fixture, 'save-first-names')).toContain(name);
    expect(element(fixture).querySelector('[data-testid="describe-again"]')).toBeNull();
  });

  it("says why a save was refused, with the launcher's finding", async () => {
    const fixture = await openWith();
    await editAndSave(fixture);
    const refused = (examples.get('error-commands-changed') as { response: object }).response;
    http
      .expectOne((r) => r.method === 'POST' && r.url === 'api/config')
      .flush(structuredClone(refused), { status: 403, statusText: 'Forbidden' });
    await settle();
    expect(text(fixture, 'save-problem')).toContain('--allow-command-changes');
    expect(text(fixture, 'saved')).toBe('');
  });

  it('asks before it reads a file again that changed outside, and does so when told to', async () => {
    const fixture = await openWith();
    await editAndSave(fixture);
    const confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    http
      .expectOne((r) => r.method === 'POST' && r.url === 'api/config')
      .flush(
        { error: { code: 'config-changed', message: 'the file changed after it was read' } },
        { status: 409, statusText: 'Conflict' },
      );
    await settle();
    expect(confirm).toHaveBeenCalledOnce();
    http.expectOne((r) => r.method === 'GET' && r.url.startsWith('api/config?'));
    confirm.mockRestore();
  });

  it('cannot save when the server does not save, and says why', async () => {
    const fixture = await openWith(undefined, {
      ok: false,
      reason: 'save-disabled',
      message: 'the GUI server was started with --no-save',
      caution: null,
    });
    const name = element(fixture).querySelector('[data-testid="network-name"]') as HTMLInputElement;
    name.value = 'ETH_AB';
    name.dispatchEvent(new Event('input'));
    TestBed.tick();
    (element(fixture).querySelector('[data-testid="network-add"]') as HTMLButtonElement).click();
    TestBed.tick();
    await settle();
    expect(
      (element(fixture).querySelector('[data-testid="save"]') as HTMLButtonElement).disabled,
    ).toBe(true);
    expect(text(fixture, 'save-reason')).toContain('--no-save');
  });

  it('asks to be opened from the server URL when there is no token', async () => {
    sessionStorage.clear();
    const fixture = TestBed.createComponent(DesignPage);
    await fixture.whenStable();
    expect(text(fixture, 'no-token')).toContain('#token=');
  });
});
