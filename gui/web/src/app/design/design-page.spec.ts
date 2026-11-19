import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { FakeWebSockets, protocolExamples } from '@vehicle-sil/gui-client/testing';
import { authInterceptor } from '../core/auth/auth-interceptor';
import { SOCKET_FACTORY } from '../core/session/gui-client';
import { DESIGN_CHECK_DELAY_MS, DesignPage } from './design-page';

describe('DesignPage', () => {
  let examples: Map<string, unknown>;
  let http: HttpTestingController;

  beforeAll(async () => {
    examples = await protocolExamples();
  });

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
    await TestBed.configureTestingModule({
      imports: [DesignPage],
      providers: [
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: new FakeWebSockets().factory },
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
  async function openWith(mutate: (check: Record<string, unknown>) => void = () => undefined) {
    const fixture = TestBed.createComponent(DesignPage);
    TestBed.tick();
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
    expect(text(fixture, 'not-described')).toContain('照合していない participant が 1 個');
    expect(
      element(fixture).querySelector('[data-testid="design-outcome"]')?.getAttribute('data-result'),
    ).toBe('passed');
    (element(fixture).querySelector('[data-testid="describe-again"]') as HTMLButtonElement).click();
    TestBed.tick();
    const described = http.expectOne('api/describe');
    expect(described.request.body).toMatchObject({ file: 'bad-lines.yaml' });
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

  it('asks to be opened from the server URL when there is no token', async () => {
    sessionStorage.clear();
    const fixture = TestBed.createComponent(DesignPage);
    await fixture.whenStable();
    expect(text(fixture, 'no-token')).toContain('#token=');
  });
});
