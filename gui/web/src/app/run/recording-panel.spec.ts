import { provideHttpClient, withInterceptors } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { Recording } from '@vehicle-sil/gui-client';
import { FakeWebSockets } from '@vehicle-sil/gui-client/testing';
import { authInterceptor } from '../core/auth/auth-interceptor';
import { SOCKET_FACTORY } from '../core/session/gui-client';
import { RecordingPanel } from './recording-panel';

@Component({
  imports: [RecordingPanel],
  template: `<app-recording-panel
    [session]="session()"
    [running]="running()"
    [hasResult]="hasResult()"
  />`,
})
class Host {
  readonly session = signal('1');
  readonly running = signal(true);
  readonly hasResult = signal(false);
}

describe('RecordingPanel', () => {
  let fixture: ComponentFixture<Host>;
  let http: HttpTestingController;
  let fake: FakeWebSockets;

  beforeEach(async () => {
    vi.useFakeTimers();
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    fake = new FakeWebSockets();
    await TestBed.configureTestingModule({
      imports: [Host],
      providers: [
        provideHttpClient(withInterceptors([authInterceptor])),
        provideHttpClientTesting(),
        { provide: SOCKET_FACTORY, useValue: fake.factory },
      ],
    }).compileComponents();
    http = TestBed.inject(HttpTestingController);
    fixture = TestBed.createComponent(Host);
    TestBed.tick();
  });

  afterEach(() => {
    TestBed.resetTestingModule();
    vi.useRealTimers();
    sessionStorage.clear();
  });

  const settle = async () => {
    await vi.advanceTimersByTimeAsync(0);
    TestBed.tick();
  };

  const text = (testId: string) =>
    (fixture.nativeElement as HTMLElement)
      .querySelector(`[data-testid="${testId}"]`)
      ?.textContent?.trim() ?? null;

  function recording(extra: Partial<Recording> = {}): { recording: Recording } {
    return {
      recording: {
        run_id: '1',
        file: 'simulation.yaml',
        state: 'running',
        available: true,
        reason: null,
        writes: 1,
        omitted: 0,
        steps: 1,
        end_ms: null,
        yaml: 'vehicle_sil_scenario: 1\nsteps:\n  - at_ms: 100\n    set:\n      Engine.engine.demand: 50\n',
        ...extra,
      },
    };
  }

  it('omits end_ms notice mid-run and refreshes on result arrival to display end_ms', async () => {
    http.expectOne('api/recording').flush(recording({ end_ms: null }));
    await settle();
    expect(text('recording-count')).toContain('1 件');
    expect(text('recording-end-ms')).toBeNull();

    // Result arrives while launcher may still be running/finishing -> refreshes GET api/recording
    fixture.componentInstance.hasResult.set(true);
    TestBed.tick();
    http.expectOne('api/recording').flush(
      recording({
        state: 'ended',
        end_ms: 1500,
        yaml: 'vehicle_sil_scenario: 1\nend_ms: 1500\nsteps: []\n',
      }),
    );
    await settle();
    expect(text('recording-end-ms')).toContain('end_ms: 1500 ms');

    // Launcher finishes too -> refreshes again cleanly
    fixture.componentInstance.running.set(false);
    TestBed.tick();
    http.expectOne('api/recording').flush(recording({ state: 'ended', end_ms: 1500 }));
    await settle();
    expect(text('recording-end-ms')).toContain('end_ms: 1500 ms');
  });
});
