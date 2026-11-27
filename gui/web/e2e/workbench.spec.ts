import { expect, test, type WebSocketRoute } from '@playwright/test';
import { copyFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { createServer } from 'node:net';
import { join } from 'node:path';
import { CONFIGS, launchGuiServer, REPLAY_LAUNCHER, REPOSITORY } from './support/gui-server';
import { RUN_TIMEOUT } from './support/run-page';

const features = [
  'events',
  'stdin-commands',
  'stdin-run-until',
  'request',
  'state',
  'check-variables',
  'speed',
  'start-paused',
  'run-stop-at',
  'time-commands',
  'manual-participants',
  'control-links',
  'control-server',
  'scenario',
  'scenario-end-ms',
  'scenario-handover',
  'timeline',
].join(',');

for (const storage of ['available', 'blocked', 'corrupt'] as const) {
  test(`workbench detached views stay connected to a real launcher (${storage} storage)`, async ({
    page,
  }) => {
    const launcher = process.env['VEHICLE_SIL_WORKBENCH_REAL_LAUNCHER'];
    const examples = process.env['VEHICLE_SIL_WORKBENCH_REAL_EXAMPLES'];
    test.skip(!launcher || !examples || test.info().project.name !== 'static');
    test.setTimeout(150_000);
    const root = mkdtempSync(join(tmpdir(), 'vehicle-sil-real-workbench-'));
    const config = join(root, 'simulation.yaml');
    writeFileSync(
      config,
      [
        'vehicle_sil_simulation: 2',
        'networks:',
        '  - { name: J1939_CAN, kind: can }',
        'participants:',
        '  - name: Engine',
        `    command: [${JSON.stringify(join(examples!, 'engine', 'vehicle_sil_engine_ecu'))}]`,
        '    step_ms: 10',
        '  - name: Dashboard',
        `    command: [${JSON.stringify(join(examples!, 'dashboard', 'vehicle_sil_dashboard_ecu'))}]`,
        '    step_ms: 5',
        '',
      ].join('\n'),
    );
    const server = await launchGuiServer({
      launcher: launcher!,
      root,
      config,
      port: 0,
      web: join(REPOSITORY, 'build', 'gui-workbench', 'browser'),
    });
    let restarted: Awaited<ReturnType<typeof launchGuiServer>> | null = null;
    try {
      let socket: WebSocketRoute | undefined;
      let variables:
        | { run_id: string; variables: { peers: { name: string }[]; [key: string]: unknown } }
        | undefined;
      if (storage === 'blocked') {
        await page.routeWebSocket(/\/ws$/, (client) => {
          socket = client;
          const upstream = client.connectToServer();
          upstream.onMessage((message) => {
            if (typeof message === 'string') {
              const packet = JSON.parse(message);
              if (packet.type === 'variables' && packet.variables?.peers?.length >= 2)
                variables = packet;
            }
            client.send(message);
          });
        });
      }
      await page.context().addInitScript((mode) => {
        if (mode === 'blocked') {
          for (const key of ['sessionStorage', 'localStorage']) {
            Object.defineProperty(window, key, {
              get() {
                throw new DOMException('Storage is unavailable', 'SecurityError');
              },
            });
          }
          const NativeChannel = BroadcastChannel;
          const delayed: (() => void)[] = [];
          const control = window as typeof window & {
            holdSelections: boolean;
            releaseSelections: () => void;
          };
          control.holdSelections = false;
          control.releaseSelections = () => {
            control.holdSelections = false;
            delayed.splice(0).forEach((send) => send());
          };
          window.BroadcastChannel = class extends NativeChannel {
            override postMessage(message: unknown) {
              if (control.holdSelections && (message as { kind?: string })?.kind === 'selection') {
                delayed.push(() => super.postMessage(message));
              } else super.postMessage(message);
            }
          };
        } else if (
          mode === 'corrupt' &&
          location.protocol === 'http:' &&
          !new URLSearchParams(location.search).has('view')
        ) {
          const original = Storage.prototype.getItem;
          Storage.prototype.getItem = function (key) {
            return key.startsWith('vehicle-sil-workbench-waves:')
              ? '{invalid'
              : original.call(this, key);
          };
          localStorage.setItem('vehicle-sil-workbench-pins', '[null,42]');
        } else if (
          mode === 'available' &&
          location.protocol === 'http:' &&
          !new URLSearchParams(location.search).has('view') &&
          localStorage.getItem('vehicle-sil-workbench-waves') === null
        ) {
          // The previous Workbench stored an unscoped array; migrate it on first use.
          localStorage.setItem(
            'vehicle-sil-workbench-waves',
            JSON.stringify([{ peer: 'Engine', name: 'engine.throttle', type: 'real', unit: '%' }]),
          );
        }
      }, storage);
      const requestUrls: string[] = [];
      page.context().on('request', (request) => requestUrls.push(request.url()));
      if (storage === 'available') {
        await page.clock.install();
        await page.clock.resume();
      }
      await page.goto(server.url);
      await page.evaluate(() => {
        window.dispatchEvent(
          new StorageEvent('storage', { key: 'vehicle-sil-workbench-waves', newValue: '{invalid' }),
        );
        window.dispatchEvent(
          new StorageEvent('storage', { key: 'vehicle-sil-workbench-pins', newValue: '[null,42]' }),
        );
      });
      await page.getByRole('button', { name: 'Start', exact: true }).click();
      const peer = page.getByRole('combobox', { name: 'ECU', exact: true });
      await expect(peer.locator('option[value="Engine"]')).toHaveCount(1, { timeout: RUN_TIMEOUT });
      await expect(peer.locator('option[value="Dashboard"]')).toHaveCount(1, {
        timeout: RUN_TIMEOUT,
      });
      await peer.selectOption('Engine');
      const throttle = page.getByRole('spinbutton', { name: 'engine.throttle', exact: true });
      await expect(throttle).toBeEnabled({ timeout: RUN_TIMEOUT });
      if (storage === 'blocked') {
        await expect.poll(() => Boolean(socket && variables)).toBe(true);
        const complete = variables!;
        // A new run first reports no peers, then another ECU can appear before the selected one.
        socket!.send(
          JSON.stringify({
            type: 'variables',
            run_id: complete.run_id,
            variables: { ...complete.variables, state: 'connecting', peers: [] },
          }),
        );
        await expect(page.getByText('0 / 0 signals')).toBeVisible();
        socket!.send(
          JSON.stringify({
            type: 'variables',
            run_id: complete.run_id,
            variables: {
              ...complete.variables,
              peers: complete.variables.peers.filter((p) => p.name === 'Dashboard'),
            },
          }),
        );
        await expect(peer.locator('option[value="Dashboard"]')).toHaveCount(1);
        await expect(peer).toHaveValue('Engine');
        await expect(peer.getByRole('option', { name: 'Engine (waiting)' })).toHaveCount(1);
        socket!.send(
          JSON.stringify({
            type: 'variables',
            run_id: complete.run_id,
            variables: complete.variables,
          }),
        );
        await expect(peer).toHaveValue('Engine');
        await expect(throttle).toBeEnabled({ timeout: RUN_TIMEOUT });
      }
      let releaseHeld: (() => void) | undefined;
      let heldRequests = 0;
      let instance: string | null = null;
      if (storage === 'available') {
        await expect
          .poll(() =>
            page.evaluate(() => localStorage.getItem('vehicle-sil-workbench-waves:last-instance')),
          )
          .not.toBeNull();
        instance = await page.evaluate(() =>
          localStorage.getItem('vehicle-sil-workbench-waves:last-instance'),
        );
        await page.evaluate(async () => {
          const hold = new Promise<void>((resolve) => {
            (window as typeof window & { releaseWaveLock: () => void }).releaseWaveLock = resolve;
          });
          await new Promise<void>((acquired) => {
            void navigator.locks.request('vehicle-sil-workbench-wave-head', async () => {
              acquired();
              await hold;
            });
          });
          localStorage.setItem('vehicle-sil-workbench-waves:last-instance', 'stale-instance');
        });
        await page.locator('app-io-row').filter({ hasText: 'ecu.connected' }).hover();
        await page.getByRole('button', { name: 'Add ecu.connected to waveforms' }).click();
        // Hold the lock past one attempt's timeout so it aborts, then release and advance past the
        // retry delay without making any further selection changes.
        await expect
          .poll(() =>
            page.evaluate(
              async () =>
                (await navigator.locks.query()).pending.filter(
                  (lock) => lock.name === 'vehicle-sil-workbench-wave-head',
                ).length,
            ),
          )
          .toBe(1);
        await page.clock.fastForward(3_000);
        await expect
          .poll(() =>
            page.evaluate(
              async () =>
                (await navigator.locks.query()).pending.filter(
                  (lock) => lock.name === 'vehicle-sil-workbench-wave-head',
                ).length,
            ),
          )
          .toBe(0);
        await page.evaluate(() =>
          (window as typeof window & { releaseWaveLock: () => void }).releaseWaveLock(),
        );
        await page.clock.fastForward(1_000);
        await expect
          .poll(
            () =>
              page.evaluate(() =>
                localStorage.getItem('vehicle-sil-workbench-waves:last-instance'),
              ),
            { timeout: RUN_TIMEOUT },
          )
          .toBe(instance);
        for (const rejected of ['unauthorized', 'other-instance'] as const) {
          let attempts = 0;
          await page.route('**/api/app', async (route) => {
            attempts++;
            await route.fulfill(
              rejected === 'unauthorized'
                ? { status: 401, body: '' }
                : { status: 200, json: { instance: 'different-instance' } },
            );
          });
          await page.evaluate(() =>
            localStorage.setItem('vehicle-sil-workbench-waves:last-instance', 'stale-instance'),
          );
          await page.evaluate(
            ({ current, name }) => {
              const bus = new BroadcastChannel('vehicle-sil-workbench-waves');
              bus.postMessage({
                kind: 'selection',
                instance: current,
                entries: [
                  {
                    key: JSON.stringify(['Engine', name]),
                    trace: { peer: 'Engine', name, type: 'real', unit: '' },
                    revision: 100,
                    actor: 'terminal-test',
                  },
                ],
              });
              bus.close();
            },
            { current: instance, name: `test.${rejected}` },
          );
          await expect.poll(() => attempts).toBe(1);
          await page.evaluate(() =>
            navigator.locks.request('vehicle-sil-workbench-wave-head', () => undefined),
          );
          await page.clock.fastForward(4_500);
          await page.evaluate(() =>
            navigator.locks.request('vehicle-sil-workbench-wave-head', () => undefined),
          );
          expect(attempts).toBe(1);
          await page.unroute('**/api/app');
        }
        const held = new Promise<void>((resolve) => (releaseHeld = resolve));
        await page.route('**/api/app', async (route) => {
          heldRequests++;
          if (heldRequests === 1) await held;
          await route.continue().catch(() => {
            // The request can already have been aborted by the browser.
          });
        });
        await page.evaluate(() =>
          localStorage.setItem('vehicle-sil-workbench-waves:last-instance', 'stale-instance'),
        );
      }
      await page.locator('app-io-row').filter({ hasText: 'engine.speed' }).hover();
      await page.getByRole('button', { name: 'Add engine.speed to waveforms' }).click();
      if (storage === 'available') {
        await expect.poll(() => heldRequests).toBe(1);
        await page.evaluate((current) => {
          const bus = new BroadcastChannel('vehicle-sil-workbench-waves');
          bus.postMessage({
            kind: 'selection',
            instance: current,
            entries: [
              {
                key: JSON.stringify(['Engine', 'test.pending']),
                trace: { peer: 'Engine', name: 'test.pending', type: 'real', unit: '' },
                revision: 101,
                actor: 'terminal-test',
              },
            ],
          });
          bus.close();
        }, instance);
        expect(heldRequests).toBe(1);
        // Another tab must be able to acquire the shared lock even if this response never arrives.
        const acquired = page.evaluate(() =>
          navigator.locks.request('vehicle-sil-workbench-wave-head', () => true),
        );
        await page.clock.fastForward(3_000);
        try {
          expect(await acquired).toBe(true);
        } finally {
          releaseHeld!();
        }
        await page.unroute('**/api/app');
      }
      // Some browsers block sessionStorage or do not copy it into the new tab. The child must
      // still authenticate when opened during the existing run, without restarting it.
      await page.context().addInitScript(() => {
        if (new URLSearchParams(location.search).has('view')) {
          Object.defineProperty(window, 'sessionStorage', {
            get() {
              throw new DOMException('Storage is unavailable', 'SecurityError');
            },
          });
        }
      });
      const [ecu] = await Promise.all([
        page.context().waitForEvent('page'),
        page.getByRole('button', { name: 'Open ECU in new tab' }).click(),
      ]);
      await ecu.bringToFront();
      const detachedThrottle = ecu.getByRole('spinbutton', {
        name: 'engine.throttle',
        exact: true,
      });
      await expect(detachedThrottle).toBeEnabled({ timeout: RUN_TIMEOUT });
      expect(new URL(ecu.url()).hash).toBe('');
      await detachedThrottle.fill('50');
      await detachedThrottle.press('Enter');
      await expect(throttle).toHaveValue('50', { timeout: RUN_TIMEOUT });
      await page.bringToFront();
      await page.getByRole('button', { name: 'Waveforms', exact: true }).click();
      const [wave] = await Promise.all([
        page.context().waitForEvent('page'),
        page.getByRole('button', { name: 'Open waveforms in new tab' }).click(),
      ]);
      await wave.bringToFront();
      const path = wave.getByRole('img', { name: 'engine.speed waveform' }).locator('path.trace');
      await expect(path).toHaveCount(1, { timeout: RUN_TIMEOUT });
      if (storage === 'available') {
        await expect(wave.getByRole('img', { name: 'engine.throttle waveform' })).toBeVisible();
      }
      expect(new URL(wave.url()).hash).toBe('');
      const first = await path.getAttribute('d');
      await ecu.bringToFront();
      await detachedThrottle.fill('80');
      await detachedThrottle.press('Enter');
      await page.bringToFront();
      await page.getByRole('button', { name: 'ECU I/O', exact: true }).click();
      await expect(throttle).toHaveValue('80', { timeout: RUN_TIMEOUT });
      await wave.bringToFront();
      await expect.poll(() => path.getAttribute('d'), { timeout: RUN_TIMEOUT }).not.toBe(first);
      if (storage === 'blocked') {
        const app = await page.request.get(new URL('/api/app', server.url).href, {
          headers: { Authorization: `Bearer ${server.token}` },
        });
        const instance = (await app.json()).instance as string;
        // An old server reuses session 1. Its selection and history must not cross into this run.
        // The final message is from the current instance but an earlier run: selection still merges.
        await page.evaluate((currentInstance) => {
          const bus = new BroadcastChannel('vehicle-sil-workbench-waves');
          const key = JSON.stringify(['Engine', 'engine.speed']);
          bus.postMessage({
            kind: 'history',
            instance: 'old-server',
            session: '1',
            histories: [
              [key, Array.from({ length: 600 }, (_, i) => ({ sampleMs: i - 600, value: 999999 }))],
            ],
          });
          bus.postMessage({
            kind: 'selection',
            instance: 'old-server',
            session: '1',
            entries: [{ key, trace: null, revision: 9999, actor: 'old' }],
          });
          bus.postMessage({
            kind: 'selection',
            instance: currentInstance,
            session: '0',
            entries: [
              {
                key: JSON.stringify(['Engine', 'test.marker']),
                trace: { peer: 'Engine', name: 'test.marker', type: 'real', unit: '' },
                revision: 1,
                actor: 'test',
              },
            ],
          });
          bus.close();
        }, instance);
        await expect(wave.getByRole('img', { name: 'test.marker waveform' })).toBeVisible({
          timeout: RUN_TIMEOUT,
        });
        await expect(path).toHaveCount(1);
        await expect(wave.getByText('999,999', { exact: true })).toHaveCount(0);
        // Deliver the two concurrent updates only after both tabs have made their choice.
        await page.evaluate(() => {
          (window as typeof window & { holdSelections: boolean }).holdSelections = true;
        });
        await ecu.evaluate(() => {
          (window as typeof window & { holdSelections: boolean }).holdSelections = true;
        });
        await page.bringToFront();
        await page.locator('app-io-row').filter({ hasText: 'engine.throttle' }).hover();
        await page.getByRole('button', { name: 'Add engine.throttle to waveforms' }).click();
        await ecu.bringToFront();
        await ecu.locator('app-io-row').filter({ hasText: 'ecu.connected' }).hover();
        await ecu.getByRole('button', { name: 'Add ecu.connected to waveforms' }).click();
        await Promise.all([
          page.evaluate(() => {
            (window as typeof window & { releaseSelections: () => void }).releaseSelections();
          }),
          ecu.evaluate(() => {
            (window as typeof window & { releaseSelections: () => void }).releaseSelections();
          }),
        ]);
        await wave.bringToFront();
        const added = wave.getByRole('img', { name: 'engine.throttle waveform' });
        await expect(added).toBeVisible({ timeout: RUN_TIMEOUT });
        await expect(wave.getByRole('img', { name: 'ecu.connected waveform' })).toBeVisible({
          timeout: RUN_TIMEOUT,
        });
        await added.locator('..').getByRole('button', { name: 'Remove' }).click();
        await page.bringToFront();
        await page.getByRole('button', { name: 'Waveforms', exact: true }).click();
        await expect(page.getByRole('img', { name: 'engine.throttle waveform' })).toHaveCount(0);
        await page.getByRole('button', { name: 'ECU I/O', exact: true }).click();
      }
      await page.bringToFront();
      await page.evaluate(() => {
        window.open = () => null;
      });
      await page.getByRole('button', { name: 'Open ECU in new tab' }).click();
      await expect(page.getByRole('alert')).toHaveText('Allow pop-ups to open another view.');
      expect(requestUrls.every((url) => new URL(url).hash === '')).toBe(true);
      await page.getByRole('button', { name: 'Stop', exact: true }).click();
      await expect(page.getByText('Run ended', { exact: false }).first()).toBeVisible({
        timeout: RUN_TIMEOUT,
      });
      await expect(ecu.getByText('Run ended', { exact: false }).first()).toBeVisible({
        timeout: RUN_TIMEOUT,
      });
      await expect(wave.getByText('Run ended', { exact: false }).first()).toBeVisible({
        timeout: RUN_TIMEOUT,
      });
      await wave.close();
      await ecu.close();
      if (storage === 'available') {
        // Restart the server on the same origin. A fresh authenticated instance restores the
        // selection, while the old tab can only write to its former instance's storage.
        const oldApp = await page.request.get(new URL('/api/app', server.url).href, {
          headers: { Authorization: `Bearer ${server.token}` },
        });
        const oldInstance = (await oldApp.json()).instance as string;
        await expect
          .poll(() =>
            page.evaluate(() => localStorage.getItem('vehicle-sil-workbench-waves:last-instance')),
          )
          .toBe(oldInstance);
        const port = Number(new URL(server.url).port);
        // Let the browser close its WebSocket first so the server can bind the same port again.
        await page.close();
        await server.stop();
        // The server does not reuse an address still in TCP TIME_WAIT. Wait until the same
        // origin can be bound again instead of assuming the operating system releases it at once.
        await expect
          .poll(
            () =>
              new Promise<boolean>((resolve) => {
                const probe = createServer();
                probe.once('error', () => resolve(false));
                probe.listen(port, '127.0.0.1', () => probe.close(() => resolve(true)));
              }),
            { timeout: 80_000, intervals: [1000] },
          )
          .toBe(true);
        restarted = await launchGuiServer({
          launcher: launcher!,
          root,
          config,
          port,
          web: join(REPOSITORY, 'build', 'gui-workbench', 'browser'),
        });
        const fresh = await page.context().newPage();
        await fresh.goto(restarted.url);
        await fresh.getByRole('button', { name: 'Waveforms', exact: true }).click();
        await expect(fresh.getByRole('img', { name: 'engine.speed waveform' })).toBeVisible({
          timeout: RUN_TIMEOUT,
        });
        await expect(fresh.getByRole('img', { name: 'engine.throttle waveform' })).toBeVisible();
        await expect(fresh.getByRole('img', { name: 'ecu.connected waveform' })).toBeVisible();
        await fresh.getByRole('button', { name: 'Start', exact: true }).click();
        await expect(fresh.getByText('Running', { exact: true }).first()).toBeVisible({
          timeout: RUN_TIMEOUT,
        });
        const newApp = await fresh.request.get(new URL('/api/app', restarted.url).href, {
          headers: { Authorization: `Bearer ${restarted.token}` },
        });
        const newInstance = (await newApp.json()).instance as string;
        await expect
          .poll(() =>
            fresh.evaluate(() => localStorage.getItem('vehicle-sil-workbench-waves:last-instance')),
          )
          .toBe(newInstance);
        await fresh.evaluate(
          ({ old, current }) => {
            localStorage.removeItem(`vehicle-sil-workbench-waves:${old}`);
            const bus = new BroadcastChannel('vehicle-sil-workbench-waves');
            bus.postMessage({
              kind: 'selection',
              instance: old,
              entries: [
                {
                  key: JSON.stringify(['Engine', 'engine.speed']),
                  trace: null,
                  revision: 10000,
                  actor: 'old',
                },
              ],
            });
            bus.postMessage({
              kind: 'selection',
              instance: current,
              entries: [
                {
                  key: JSON.stringify(['Engine', 'test.barrier']),
                  trace: { peer: 'Engine', name: 'test.barrier', type: 'real', unit: '' },
                  revision: 1,
                  actor: 'test',
                },
              ],
            });
            bus.close();
          },
          { old: oldInstance, current: newInstance },
        );
        await expect(fresh.getByRole('img', { name: 'test.barrier waveform' })).toBeVisible({
          timeout: RUN_TIMEOUT,
        });
        await expect(fresh.getByRole('img', { name: 'engine.speed waveform' })).toBeVisible();
        // An old tab whose instance key was never created receives a late hello after the new
        // instance has claimed the head. Its authoritative app recheck must reject the claim.
        const stale = await page.context().newPage();
        await stale.clock.install();
        await stale.clock.resume();
        let appReplies = 0;
        let staleSocket: WebSocketRoute | undefined;
        let currentHello = '';
        stale.on('response', (response) => {
          if (response.url().endsWith('/api/app')) appReplies++;
        });
        await stale.routeWebSocket(/\/ws$/, (socket) => {
          staleSocket = socket;
          const upstream = socket.connectToServer();
          upstream.onMessage((message) => {
            if (typeof message !== 'string') return socket.send(message);
            const packet = JSON.parse(message);
            if (packet.type === 'hello') currentHello = message;
            socket.send(
              JSON.stringify(
                packet.type === 'hello' ? { ...packet, instance: oldInstance } : packet,
              ),
            );
          });
        });
        await stale.goto(restarted.url);
        await expect(stale.getByText('Running', { exact: true }).first()).toBeVisible({
          timeout: RUN_TIMEOUT,
        });
        await stale.getByRole('button', { name: 'Waveforms', exact: true }).click();
        await expect(stale.getByRole('img', { name: 'engine.speed waveform' })).toBeVisible({
          timeout: RUN_TIMEOUT,
        });
        await expect.poll(() => appReplies).toBeGreaterThanOrEqual(2);
        await fresh.evaluate(() =>
          navigator.locks.request('vehicle-sil-workbench-wave-head', () => undefined),
        );
        await expect
          .poll(() =>
            fresh.evaluate(() => localStorage.getItem('vehicle-sil-workbench-waves:last-instance')),
          )
          .toBe(newInstance);
        let failures = 0;
        await stale.route('**/api/app', async (route) => {
          failures++;
          await route.fulfill({ status: 503, body: '' });
        });
        await stale.evaluate((old) => {
          localStorage.setItem('vehicle-sil-workbench-waves:last-instance', 'stale-instance');
          const bus = new BroadcastChannel('vehicle-sil-workbench-waves');
          bus.postMessage({
            kind: 'selection',
            instance: old,
            entries: [
              {
                key: JSON.stringify(['Engine', 'test.old-retry']),
                trace: { peer: 'Engine', name: 'test.old-retry', type: 'real', unit: '' },
                revision: 10000,
                actor: 'retry-test',
              },
            ],
          });
          bus.close();
        }, oldInstance);
        await expect.poll(() => failures, { timeout: RUN_TIMEOUT }).toBeGreaterThanOrEqual(1);
        await stale.evaluate(() =>
          navigator.locks.request('vehicle-sil-workbench-wave-head', () => undefined),
        );
        await stale.clock.fastForward(1_000);
        await expect.poll(() => failures, { timeout: RUN_TIMEOUT }).toBeGreaterThanOrEqual(2);
        await stale.evaluate(() =>
          navigator.locks.request('vehicle-sil-workbench-wave-head', () => undefined),
        );
        await stale.clock.fastForward(2_000);
        await expect.poll(() => failures, { timeout: RUN_TIMEOUT }).toBeGreaterThanOrEqual(3);
        await stale.unroute('**/api/app');
        staleSocket!.send(currentHello);
        await expect
          .poll(
            () =>
              stale.evaluate(() =>
                localStorage.getItem('vehicle-sil-workbench-waves:last-instance'),
              ),
            { timeout: 2000 },
          )
          .toBe(newInstance);
        await stale.close();
        // Quota failures during the copy must not move the head to a key that was not saved.
        const quota = await page.context().newPage();
        const quotaInstance = 'old-quota-instance';
        await quota.addInitScript((blockedKey) => {
          const set = Storage.prototype.setItem;
          Storage.prototype.setItem = function (key, value) {
            if (key === blockedKey) throw new DOMException('Quota exceeded', 'QuotaExceededError');
            return set.call(this, key, value);
          };
        }, `vehicle-sil-workbench-waves:${quotaInstance}`);
        await quota.routeWebSocket(/\/ws$/, (socket) => {
          const upstream = socket.connectToServer();
          upstream.onMessage((message) => {
            if (typeof message !== 'string') return socket.send(message);
            const packet = JSON.parse(message);
            socket.send(
              JSON.stringify(
                packet.type === 'hello' ? { ...packet, instance: quotaInstance } : packet,
              ),
            );
          });
        });
        await quota.goto(restarted.url);
        await expect(quota.getByText('Running', { exact: true }).first()).toBeVisible({
          timeout: RUN_TIMEOUT,
        });
        await quota.getByRole('button', { name: 'Waveforms', exact: true }).click();
        await expect(quota.getByRole('img', { name: 'engine.speed waveform' })).toBeVisible({
          timeout: RUN_TIMEOUT,
        });
        await fresh.evaluate(() =>
          navigator.locks.request('vehicle-sil-workbench-wave-head', () => undefined),
        );
        expect(
          await fresh.evaluate(() =>
            localStorage.getItem('vehicle-sil-workbench-waves:last-instance'),
          ),
        ).toBe(newInstance);
        expect(
          await fresh.evaluate(
            (id) => localStorage.getItem(`vehicle-sil-workbench-waves:${id}`),
            quotaInstance,
          ),
        ).toBeNull();
        await quota.close();
        await fresh.getByRole('button', { name: 'Stop', exact: true }).click();
        await expect(fresh.getByText('Run ended', { exact: false }).first()).toBeVisible({
          timeout: RUN_TIMEOUT,
        });
        await fresh.close();
      }
    } finally {
      if (restarted) await restarted.stop();
      await server.stop();
      rmSync(root, { recursive: true, force: true });
    }
  });
}

test('workbench shows the active stop time and restores the next run input', async ({ page }) => {
  test.skip(
    process.env['VEHICLE_SIL_VERIFY_WORKBENCH'] !== '1' || test.info().project.name !== 'static',
  );
  const server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    config: join(CONFIGS, 'forever.yaml'),
    env: { REPLAY_FEATURES: features },
    port: 0,
    web: join(REPOSITORY, 'build', 'gui-workbench', 'browser'),
  });
  try {
    let socket: WebSocketRoute | undefined;
    await page.routeWebSocket(/\/ws$/, (ws) => {
      ws.connectToServer();
      socket = ws;
    });
    await page.goto(server.url);
    await expect(page.getByRole('status').filter({ hasText: /^Ready$/ })).toBeVisible();
    const draft = page.getByRole('textbox', { name: 'Stop at (ms)' });
    await draft.fill('4321');
    const summary = (state: string) =>
      JSON.parse(
        readFileSync(join(REPOSITORY, `gui/protocol/examples/session-${state}.json`), 'utf8'),
      ).response.session;
    const running = summary('running');
    const send = (session: object) => socket!.send(JSON.stringify({ type: 'session', session }));

    send({ ...running, check: null });
    await expect(page.getByText('Stop at (ms): Awaiting check', { exact: true })).toBeVisible();
    await expect(draft).toHaveCount(0);
    send({
      ...running,
      check: null,
      stop_at_ms: 10000,
    });
    await expect(page.getByText('Stop at (ms): 10000', { exact: true })).toBeVisible();
    send({ ...summary('ended') });
    await expect(draft).toHaveValue('4321');

    send({ ...running, id: '2' });
    await expect(page.getByText('Stop at (ms): No limit', { exact: true })).toBeVisible();
  } finally {
    await server.stop();
  }
});

test('workbench shows the overall failure when the scenario passed', async ({ page }) => {
  test.skip(
    process.env['VEHICLE_SIL_VERIFY_WORKBENCH'] !== '1' || test.info().project.name !== 'static',
  );
  const server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    config: join(CONFIGS, 'forever.yaml'),
    env: { REPLAY_FEATURES: features },
    port: 0,
    web: join(REPOSITORY, 'build', 'gui-workbench', 'browser'),
  });
  try {
    let socket: WebSocketRoute | undefined;
    await page.routeWebSocket(/\/ws$/, (ws) => {
      ws.connectToServer();
      socket = ws;
    });
    await page.goto(server.url);
    await expect.poll(() => Boolean(socket)).toBe(true);
    // The server's initial hello replaces a session sent before the replay is complete.
    await expect(page.getByRole('status').filter({ hasText: /^Ready$/ })).toBeVisible();
    const example = JSON.parse(
      readFileSync(join(REPOSITORY, 'gui/protocol/examples/session-ended.json'), 'utf8'),
    );
    const session = example.response.session;
    session.id = '1000';
    session.result = {
      ...session.result,
      result: 'failed',
      ok: false,
      message: 'A participant failed',
      scenario: {
        file: 'scenarios/throttle.yaml',
        name: null,
        status: 'passed',
        over_at_ms: 100,
        steps: [],
      },
    };
    socket!.send(JSON.stringify({ type: 'session', session }));
    await expect(page.getByText(/Run #\d+/)).toHaveCount(0);
    await page.getByRole('button', { name: 'Results', exact: true }).click();
    await expect(page.getByRole('heading', { name: 'simulation.yaml · failed' })).toBeVisible();
    await expect(page.getByText(/Run #\d+/)).toHaveCount(0);
    await expect(page.getByText('Scenario: passed', { exact: true })).toBeVisible();
  } finally {
    await server.stop();
  }
});

test('workbench preserves waveform history when another trace is added or removed', async ({
  page,
}) => {
  test.skip(
    process.env['VEHICLE_SIL_VERIFY_WORKBENCH'] !== '1' || test.info().project.name !== 'static',
    'run with verify-web.py --workbench',
  );
  const server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    config: join(CONFIGS, 'forever.yaml'),
    env: { REPLAY_TICK_MS: '50', REPLAY_FEATURES: features, REPLAY_VARIABLES: 'more' },
    port: 0,
    web: join(REPOSITORY, 'build', 'gui-workbench', 'browser'),
  });
  try {
    await page.goto(server.url);
    await page.getByRole('button', { name: 'Start', exact: true }).click();
    await page.locator('app-io-row').filter({ hasText: 'engine.demand' }).hover();
    await page.getByRole('button', { name: 'Add engine.demand to waveforms' }).click();
    const showWaveforms = page.getByRole('button', { name: 'Waveforms', exact: true });
    await showWaveforms.click();
    const demand = page.getByRole('img', { name: 'engine.demand waveform' }).locator('path.trace');
    await expect(demand).toHaveAttribute('d', /L /, { timeout: RUN_TIMEOUT });

    // Freeze sampling so new data cannot replace the original history during either assertion.
    await page.getByRole('button', { name: 'Stop', exact: true }).click();
    await expect(page.getByText('Run ended', { exact: false }).first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    await page.getByRole('button', { name: 'ECU I/O', exact: true }).click();
    await page.locator('app-io-row').filter({ hasText: 'engine.enabled' }).hover();
    await page.getByRole('button', { name: 'Add engine.enabled to waveforms' }).click();
    await showWaveforms.click();
    const enabled = page.getByRole('img', { name: 'engine.enabled waveform' });
    await expect(enabled).toBeVisible();
    await expect(demand).toHaveAttribute('d', /L /);
    await enabled.locator('..').getByRole('button', { name: 'Remove' }).click();
    await expect(enabled).toHaveCount(0);
    await expect(demand).toHaveAttribute('d', /L /);
  } finally {
    await server.stop();
  }
});

test('workbench writes an ECU input, detaches the ECU, and loads the recorded YAML', async ({
  page,
}) => {
  test.skip(
    process.env['VEHICLE_SIL_VERIFY_WORKBENCH'] !== '1' || test.info().project.name !== 'static',
    'run with verify-web.py --workbench',
  );
  test.setTimeout(120_000);
  const server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    config: join(CONFIGS, 'forever.yaml'),
    env: { REPLAY_TICK_MS: '50', REPLAY_FEATURES: features, REPLAY_VARIABLES: 'more' },
    port: 0,
    web: join(REPOSITORY, 'build', 'gui-workbench', 'browser'),
  });
  try {
    const errors: string[] = [];
    page.on('pageerror', (error) => errors.push(error.message));
    await page.clock.install();
    await page.clock.resume();
    await page.goto(server.url);
    await expect(page.getByRole('button', { name: 'Record & start' })).toBeEnabled();
    const network = page.getByRole('combobox', { name: 'Network', exact: true });
    const scenario = page.getByRole('combobox', { name: 'Scenario', exact: true });
    await expect(network).toHaveValue('forever.yaml');
    await expect(network.locator('option[value="scenarios/throttle.yaml"]')).toHaveCount(0);
    await expect(scenario.locator('option[value="scenarios/throttle.yaml"]')).toHaveCount(1);
    await page.getByRole('button', { name: 'Network Designer', exact: true }).click();
    await expect(page.getByRole('textbox', { name: 'Network YAML' })).toHaveValue(
      /vehicle_sil_simulation/,
    );
    await page.getByRole('button', { name: 'Settings', exact: true }).click();
    await expect(page.getByRole('heading', { name: 'Scenario folder' })).toBeVisible();
    const folder = mkdtempSync(join(tmpdir(), 'vehicle-sil-scenarios-'));
    try {
      copyFileSync(join(CONFIGS, 'forever.yaml'), join(folder, 'network.yaml'));
      copyFileSync(join(CONFIGS, 'scenarios', 'throttle.yaml'), join(folder, 'throttle.yaml'));
      await page.locator('input[webkitdirectory]').setInputFiles(folder);
      await page.getByRole('button', { name: 'Scenario', exact: true }).click();
      const folderFiles = page.getByRole('combobox', { name: 'Scenario folder files' });
      await expect(folderFiles.getByRole('option')).toHaveText(['Folder files', 'throttle.yaml']);
      await folderFiles.selectOption('throttle.yaml');
      await expect(page.getByRole('textbox', { name: 'Scenario YAML' })).toHaveValue(
        /vehicle_sil_scenario/,
      );
    } finally {
      rmSync(folder, { recursive: true, force: true });
    }
    await page.getByRole('button', { name: 'ECU I/O', exact: true }).click();
    await page.getByRole('combobox', { name: 'Speed', exact: true }).selectOption('max');
    const observer = await page.context().newPage();
    await observer.goto(server.url);
    await observer
      .getByRole('combobox', { name: 'Network', exact: true })
      .selectOption('simulation.yaml');
    await expect(
      observer
        .getByRole('combobox', { name: 'Scenario', exact: true })
        .locator('option[value="scenarios/throttle.yaml"]'),
    ).toHaveCount(1);
    await observer
      .getByRole('combobox', { name: 'Scenario', exact: true })
      .selectOption('scenarios/throttle.yaml');
    // The session can arrive over the socket before the start response. Delay that response
    // beyond the old five-second limit: its session ID must still identify this recording.
    let releaseStart!: () => void;
    const holdStart = new Promise<void>((resolve) => {
      releaseStart = resolve;
    });
    let markStartFetched!: () => void;
    const startFetched = new Promise<void>((resolve) => {
      markStartFetched = resolve;
    });
    await page.route(
      '**/api/run',
      async (route) => {
        const response = await route.fetch();
        markStartFetched();
        await holdStart;
        await route.fulfill({ response });
      },
      { times: 1 },
    );
    const start = page.waitForRequest(
      (request) => request.url().endsWith('/api/run') && request.method() === 'POST',
    );
    const started = page.waitForResponse(
      (response) => response.url().endsWith('/api/run') && response.request().method() === 'POST',
    );
    try {
      await page.getByRole('button', { name: 'Record & start' }).click();
      expect((await start).postDataJSON()).not.toHaveProperty('stop_at_ms');
      await startFetched;
      await expect(page.getByText('Starting recording…')).toBeVisible();
      await expect(page.getByText('Network: forever.yaml', { exact: true })).toBeVisible();
      await expect(observer.getByText('Network: forever.yaml', { exact: true })).toBeVisible();
      await expect(observer.getByText('Scenario: Free run', { exact: true })).toBeVisible();
      await page.clock.fastForward(5_500);
    } finally {
      releaseStart();
    }
    await started;
    await expect(page.getByText('Recording', { exact: false }).first()).toBeVisible();
    const demand = page.getByRole('spinbutton', { name: 'engine.demand', exact: true });
    await expect(demand).toBeEnabled({ timeout: RUN_TIMEOUT });
    expect(
      (await page.getByRole('slider', { name: 'Adjust engine.demand' }).boundingBox())?.width,
    ).toBeGreaterThanOrEqual(84);
    await demand.fill('70');
    await demand.press('Enter');
    await expect(
      page
        .locator('app-io-row')
        .filter({ hasText: 'engine.warning' })
        .getByRole('img', { name: 'ON', exact: true }),
    ).toBeVisible();

    const enabled = page.getByRole('switch', { name: 'engine.enabled', exact: true });
    await expect(enabled).toBeEnabled();
    await expect(enabled).not.toBeChecked();
    let forwardWrite!: () => void;
    const holdWrite = new Promise<void>((resolve) => {
      forwardWrite = resolve;
    });
    await page.route(
      '**/api/write',
      async (route) => {
        await holdWrite;
        await route.continue();
      },
      { times: 1 },
    );
    try {
      await enabled.click();
      await expect(enabled).toHaveAttribute('aria-busy', 'true');
      await expect(enabled).not.toBeChecked();
    } finally {
      forwardWrite();
    }
    await expect(enabled).toBeChecked();
    await expect(enabled).toBeEnabled();

    const [ecu] = await Promise.all([
      page.context().waitForEvent('page'),
      page.getByRole('button', { name: 'Open ECU in new tab' }).click(),
    ]);
    await expect(ecu.getByRole('combobox', { name: 'ECU', exact: true })).toHaveValue('Engine');
    await expect(ecu.getByRole('spinbutton', { name: 'engine.demand', exact: true })).toHaveValue(
      '70',
    );
    await ecu.bringToFront();
    const detachedDemand = ecu.getByRole('spinbutton', { name: 'engine.demand', exact: true });
    await detachedDemand.fill('60');
    await detachedDemand.press('Enter');
    await expect(demand).toHaveValue('60', { timeout: RUN_TIMEOUT });
    await page.bringToFront();
    await page.locator('app-io-row').filter({ hasText: 'engine.demand' }).hover();
    await page.getByRole('button', { name: 'Add engine.demand to waveforms' }).click();
    await page.getByRole('button', { name: 'Waveforms', exact: true }).click();
    const [wave] = await Promise.all([
      page.context().waitForEvent('page'),
      page.getByRole('button', { name: 'Open waveforms in new tab' }).click(),
    ]);
    await wave.bringToFront();
    const trace = wave.getByRole('img', { name: 'engine.demand waveform' });
    await expect(trace.locator('path.trace')).toHaveCount(1, { timeout: RUN_TIMEOUT });
    await ecu.bringToFront();
    await detachedDemand.fill('80');
    await detachedDemand.press('Enter');
    await expect(page.getByText('Running', { exact: true }).first()).toBeVisible();
    await wave.bringToFront();
    await expect(trace.locator('path.trace')).toHaveAttribute('d', /[MLH]/);
    await wave.close();
    await ecu.close();
    await page.bringToFront();

    // Mid-run Load recording omits end_ms and shows mid-run notice
    await page.getByRole('button', { name: 'Scenario', exact: true }).click();
    await page.getByRole('button', { name: 'Load recording' }).click();
    await expect(page.getByRole('textbox', { name: 'Scenario YAML' })).toHaveValue(
      /engine\.demand.*70/,
    );
    await expect(page.getByRole('textbox', { name: 'Scenario YAML' })).not.toHaveValue(/^end_ms:/m);
    await expect(page.getByTestId('workbench-recording-meta')).toContainText(
      'mid-run: end_ms omitted until run ends',
    );

    await page.getByRole('button', { name: 'Stop', exact: true }).click();
    await expect(page.getByText('Run ended', { exact: false }).first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    await expect(observer.getByRole('combobox', { name: 'Network', exact: true })).toHaveValue(
      'simulation.yaml',
    );
    await expect(observer.getByRole('combobox', { name: 'Scenario', exact: true })).toHaveValue(
      'scenarios/throttle.yaml',
    );
    await observer.close();
    // After Stop, Load recording carries finalized end_ms
    await page.getByRole('button', { name: 'Load recording' }).click();
    await expect(page.getByRole('textbox', { name: 'Scenario YAML' })).toHaveValue(
      /engine\.demand.*70/,
    );
    await expect(page.getByRole('textbox', { name: 'Scenario YAML' })).toHaveValue(/end_ms: \d+/);
    await expect(page.getByTestId('workbench-recording-meta')).toContainText(/end_ms: \d+ ms/);
    await expect(page.getByRole('textbox', { name: 'Scenario YAML' })).not.toHaveValue(
      /# {3}- expect:/,
    );
    // A refused next start did not create a session and must leave the previous recording available.
    await page.route(
      '**/api/run',
      (route) => route.fulfill({ status: 409, json: { error: 'refused' } }),
      { times: 1 },
    );
    const rejected = page.waitForResponse(
      (response) => response.url().endsWith('/api/run') && response.status() === 409,
    );
    await page.getByRole('button', { name: 'Start', exact: true }).click();
    await rejected;
    await expect(page.getByRole('button', { name: 'Load recording' })).toBeVisible();
    await page.getByRole('button', { name: 'Load recording' }).click();
    await expect(page.getByRole('textbox', { name: 'Scenario YAML' })).toHaveValue(
      /engine\.demand.*70/,
    );
    // Starting a second, unrecorded run must not attribute the first run's recording to it.
    await page.getByRole('combobox', { name: 'Speed', exact: true }).selectOption('1');
    await page.getByRole('button', { name: 'Start', exact: true }).click();
    await expect(page.getByText('Running', { exact: true }).first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    await page.getByRole('button', { name: 'Stop', exact: true }).click();
    await expect(page.getByText('Run ended', { exact: false }).first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    await expect(page.getByRole('button', { name: 'Load recording' })).toHaveCount(0);
    await page.getByRole('button', { name: 'ECU I/O', exact: true }).click();
    await page.getByRole('checkbox', { name: 'Start paused' }).check();
    await page.getByRole('button', { name: 'Start', exact: true }).click();
    await expect(page.getByRole('button', { name: 'Resume', exact: true })).toBeEnabled({
      timeout: RUN_TIMEOUT,
    });
    const pausedDemand = page.getByRole('spinbutton', { name: 'engine.demand', exact: true });
    await pausedDemand.fill('40');
    await pausedDemand.press('Enter');
    const row = page.locator('app-io-row').filter({ hasText: 'engine.demand' });
    await expect(row.getByText('Write pending')).toBeVisible();
    await page.clock.fastForward(11_000);
    await expect(row.getByText('Write pending')).toBeVisible();
    await expect(row.getByText('Write failed')).toHaveCount(0);
    await page.getByRole('button', { name: 'Resume', exact: true }).click();
    await expect(row.getByText('Write pending')).toHaveCount(0, { timeout: RUN_TIMEOUT });
    await page.getByRole('button', { name: 'Stop', exact: true }).click();
    await expect(page.getByText('Run ended', { exact: false }).first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    expect(errors).toEqual([]);
  } finally {
    await server.stop();
  }
});

test('workbench handover option disables Stop at (ms) and omits stop_at_ms from check and run', async ({
  page,
}) => {
  test.skip(
    process.env['VEHICLE_SIL_VERIFY_WORKBENCH'] !== '1' || test.info().project.name !== 'static',
  );
  const server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    config: join(CONFIGS, 'simulation.yaml'),
    env: { REPLAY_FEATURES: features },
    port: 0,
    web: join(REPOSITORY, 'build', 'gui-workbench', 'browser'),
  });
  try {
    await page.goto(server.url);
    await expect(page.getByRole('status').filter({ hasText: /^Ready$/ })).toBeVisible();
    const stopInput = page.getByRole('textbox', { name: 'Stop at (ms)' });
    await stopInput.fill('4321');
    const scenario = page.getByRole('combobox', { name: 'Scenario', exact: true });
    await expect(scenario.locator('option[value="scenarios/throttle.yaml"]')).toHaveCount(1);
    await scenario.selectOption('scenarios/throttle.yaml');

    const after = page.getByRole('combobox', { name: 'After scenario', exact: true });
    await expect(after).toBeVisible();
    await after.selectOption('pause');
    await expect(stopInput).toBeDisabled();
    await expect(stopInput).toHaveValue('');

    const checkReqPromise = page.waitForRequest(
      (r) => r.url().endsWith('/api/check') && r.method() === 'POST',
    );
    await page.getByRole('button', { name: 'Scenario', exact: true }).click();
    await page.getByRole('button', { name: 'Check run settings', exact: true }).click();
    const checkBody = (await checkReqPromise).postDataJSON() as Record<string, unknown>;
    expect(checkBody['scenario']).toBe('scenarios/throttle.yaml');
    expect(checkBody['after_scenario']).toBe('pause');
    expect(checkBody['stop_at_ms']).toBeUndefined();

    // Start also sends after_scenario: 'pause' and omits stop_at_ms
    await page.getByRole('button', { name: 'ECU I/O', exact: true }).click();
    await page.getByRole('combobox', { name: 'Speed', exact: true }).selectOption('max');
    const runReqPromise = page.waitForRequest(
      (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
    );
    await page.getByRole('button', { name: 'Start', exact: true }).click();
    const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
    expect(runBody['scenario']).toBe('scenarios/throttle.yaml');
    expect(runBody['after_scenario']).toBe('pause');
    expect(runBody['stop_at_ms']).toBeUndefined();
    await expect(page.getByTestId('handover-notice')).toContainText('1000 ms', {
      timeout: RUN_TIMEOUT,
    });
    await page.getByRole('button', { name: 'Stop', exact: true }).click();
    await expect(page.getByTestId('handover-notice')).toHaveCount(0);
    await expect(page.getByText('Run ended', { exact: false }).first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    await expect(page.getByTestId('handover-notice')).toHaveCount(0);

    // Switching back to 'stop' restores Stop at (ms) draft
    await after.selectOption('stop');
    await expect(stopInput).toBeEnabled();
    await expect(stopInput).toHaveValue('4321');
  } finally {
    await server.stop();
  }
});

test('workbench displays replay write lock, handover notice on pause, and hides handover on non-handover outcomes', async ({
  page,
}) => {
  test.skip(
    process.env['VEHICLE_SIL_VERIFY_WORKBENCH'] !== '1' || test.info().project.name !== 'static',
  );
  const server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    config: join(CONFIGS, 'forever.yaml'),
    env: { REPLAY_FEATURES: features },
    port: 0,
    web: join(REPOSITORY, 'build', 'gui-workbench', 'browser'),
  });
  try {
    let socket: WebSocketRoute | undefined;
    await page.routeWebSocket(/\/ws$/, (ws) => {
      ws.connectToServer();
      socket = ws;
    });
    await page.goto(server.url);
    await expect.poll(() => Boolean(socket)).toBe(true);
    await expect(page.getByRole('status').filter({ hasText: /^Ready$/ })).toBeVisible();

    const runningSession = JSON.parse(
      readFileSync(join(REPOSITORY, 'gui/protocol/examples/session-running.json'), 'utf8'),
    ).response.session;
    const endedSession = JSON.parse(
      readFileSync(join(REPOSITORY, 'gui/protocol/examples/session-ended.json'), 'utf8'),
    ).response.session;

    const ok = { ok: true, reason: null, message: null, caution: null };
    const refused = (reason: string, message: string) => ({
      ok: false,
      reason,
      message,
      caution: null,
    });
    const baseState = {
      rev: 1,
      seq: 1,
      phase: 'running',
      paused: false,
      paused_at_ms: null,
      holding: false,
      speed: 'max',
      pacing: 'on-time',
      stop: { requests: 0, by: null },
      scenario: true,
      handed_over: false,
      scenario_over_at_ms: null,
      allowed: {
        pause: ok,
        resume: refused('not-paused', 'not paused'),
        step: { ...refused('not-paused', 'not paused'), min_ms: 10, max_ms: 5000 },
        speed: { ...ok, min: 0.1, max: 100 },
        run_until: ok,
        stop: ok,
        write: refused('scenario-running', 'scenario plays'),
      },
      outside: [],
    };
    const scenarioSummary = {
      file: 'scenarios/throttle.yaml',
      name: 'throttle',
      step_count: 3,
      steps: [],
      steps_omitted: 0,
      status: null,
      over_at_ms: null,
      timeline_url: null,
    };

    socket!.send(
      JSON.stringify({
        type: 'session',
        session: {
          ...runningSession,
          id: '50',
          launcher_state: baseState,
          scenario: scenarioSummary,
        },
      }),
    );
    socket!.send(
      JSON.stringify({
        type: 'variables',
        run_id: '50',
        variables: {
          state: 'ready',
          reason: null,
          peers: [
            {
              name: 'Engine',
              connected: true,
              variables: [
                {
                  name: 'engine.demand',
                  type: 'real',
                  role: 'input',
                  writable: true,
                  builtin: false,
                  unit: '%',
                  min: 0,
                  max: 100,
                  step: null,
                  description: '',
                  enum: null,
                },
              ],
            },
          ],
          peers_omitted: 0,
          variables_omitted: 0,
          operations: [],
        },
      }),
    );

    await expect(page.getByTestId('scenario-write-lock')).toBeVisible();
    await expect(page.getByTestId('handover-notice')).toHaveCount(0);
    await expect(
      page.getByRole('spinbutton', { name: 'engine.demand', exact: true }),
    ).toBeDisabled();

    // Handover state arrives on the same run
    socket!.send(
      JSON.stringify({
        type: 'event',
        run_id: '50',
        event: {
          ...baseState,
          event: 'state',
          seq: 10,
          t_ms: 1000,
          rev: 2,
          paused: true,
          paused_at_ms: 1000,
          pacing: 'not-paced',
          handed_over: true,
          scenario_over_at_ms: 1000,
          allowed: {
            ...baseState.allowed,
            pause: refused('paused', 'paused'),
            resume: ok,
            step: { ...ok, min_ms: 10, max_ms: 5000 },
            write: ok,
          },
        },
      }),
    );
    await expect(page.getByTestId('handover-notice')).toContainText('1000 ms');
    await expect(page.getByTestId('scenario-write-lock')).toHaveCount(0);
    await expect(page.getByRole('button', { name: 'Resume', exact: true })).toBeEnabled();
    await expect(page.getByRole('button', { name: '1 step', exact: true })).toBeEnabled();
    await expect(
      page.getByRole('spinbutton', { name: 'engine.demand', exact: true }),
    ).toBeEnabled();

    // Stopping state on the same handed-over run clears handover-notice immediately
    socket!.send(
      JSON.stringify({
        type: 'event',
        run_id: '50',
        event: {
          ...baseState,
          event: 'state',
          seq: 11,
          t_ms: 1010,
          rev: 3,
          paused: false,
          stop: { requests: 1, by: 'operator' },
          handed_over: true,
          scenario_over_at_ms: 1000,
          allowed: {
            ...baseState.allowed,
            pause: refused('stopping', 'stopping'),
            resume: refused('stopping', 'stopping'),
            step: { ...refused('stopping', 'stopping'), min_ms: 10, max_ms: 5000 },
            write: refused('stopping', 'stopping'),
          },
        },
      }),
    );
    await expect(page.getByRole('status').filter({ hasText: /^Stopping…$/ })).toBeVisible();
    await expect(page.getByTestId('handover-notice')).toHaveCount(0);
    await expect(page.getByTestId('scenario-write-lock')).toHaveCount(0);

    // Crashed handed-over session never shows handover-notice, even while Start is pending
    socket!.send(
      JSON.stringify({
        type: 'session',
        session: {
          ...runningSession,
          id: '50',
          state: 'ended',
          launcher: { running: false, exit_code: 1, signal: null, silent_s: null },
          result: null,
          launcher_state: {
            ...baseState,
            rev: 4,
            paused: true,
            handed_over: true,
            scenario_over_at_ms: 1000,
          },
          scenario: { ...scenarioSummary, status: 'passed', handed_over: true },
        },
      }),
    );
    await expect(page.getByTestId('handover-notice')).toHaveCount(0);

    // Non-handover ended outcome clears handover notice and must not flash lock while Start is pending
    socket!.send(
      JSON.stringify({
        type: 'session',
        session: {
          ...endedSession,
          id: '51',
          scenario: { ...scenarioSummary, status: 'failed', handed_over: false },
        },
      }),
    );
    await expect(page.getByTestId('handover-notice')).toHaveCount(0);
    await expect(page.getByTestId('scenario-write-lock')).toHaveCount(0);

    let releaseStart!: () => void;
    const holdStart = new Promise<void>((resolve) => {
      releaseStart = resolve;
    });
    await page.route(
      '**/api/run',
      async (route) => {
        await holdStart;
        await route.fulfill({ status: 409, json: { error: 'cancelled' } });
      },
      { times: 1 },
    );
    try {
      await page.getByRole('button', { name: 'Start', exact: true }).click();
      await expect(page.getByRole('status').filter({ hasText: /^Starting…$/ })).toBeVisible();
      await expect(page.getByTestId('handover-notice')).toHaveCount(0);
      await expect(page.getByTestId('scenario-write-lock')).toHaveCount(0);
    } finally {
      releaseStart();
    }
    await expect(page.getByTestId('handover-notice')).toHaveCount(0);
    await expect(page.getByTestId('scenario-write-lock')).toHaveCount(0);
  } finally {
    await server.stop();
  }
});

test('workbench runs scenario handover at max speed, unlocks writes, and steps on the same run', async ({
  page,
}) => {
  test.skip(
    process.env['VEHICLE_SIL_VERIFY_WORKBENCH'] !== '1' || test.info().project.name !== 'static',
    'run with verify-web.py --workbench',
  );
  test.setTimeout(120_000);
  const server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    config: join(CONFIGS, 'simulation.yaml'),
    env: { REPLAY_TICK_MS: '25', REPLAY_FEATURES: features, REPLAY_VARIABLES: 'more' },
    port: 0,
    web: join(REPOSITORY, 'build', 'gui-workbench', 'browser'),
  });
  try {
    await page.goto(server.url);
    const scenario = page.getByRole('combobox', { name: 'Scenario', exact: true });
    await expect(scenario.locator('option[value="scenarios/throttle.yaml"]')).toHaveCount(1);
    await scenario.selectOption('scenarios/throttle.yaml');
    await page.getByRole('combobox', { name: 'Speed', exact: true }).selectOption('max');
    await page.getByRole('combobox', { name: 'After scenario', exact: true }).selectOption('pause');

    await page.getByRole('button', { name: 'Start', exact: true }).click();
    await expect(page.getByTestId('handover-notice')).toContainText('1000 ms', {
      timeout: RUN_TIMEOUT,
    });
    await expect(page.getByText('Sim 1.000 s')).toBeVisible();
    await page.waitForTimeout(300);
    await expect(page.getByText('Sim 1.000 s')).toBeVisible();

    const demand = page.getByRole('spinbutton', { name: 'engine.demand', exact: true });
    await expect(demand).toBeEnabled();
    await demand.fill('70');
    await demand.press('Enter');
    const row = page.locator('app-io-row').filter({ hasText: 'engine.demand' });
    await expect(row.getByText('Write pending')).toBeVisible();

    await page.getByRole('button', { name: '1 step', exact: true }).click();
    await expect(row.getByText('Write pending')).toHaveCount(0, { timeout: RUN_TIMEOUT });
    await expect(page.getByText('Sim 1.000 s')).toHaveCount(0);

    await page.getByRole('button', { name: 'Stop', exact: true }).click();
    await expect(page.getByTestId('handover-notice')).toHaveCount(0);
    await expect(page.getByText('Run ended', { exact: false }).first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    await expect(page.getByTestId('handover-notice')).toHaveCount(0);

    // Failing scenario with handover selected does NOT display handover notice
    await scenario.selectOption('scenarios/fails.yaml');
    await page.getByRole('combobox', { name: 'After scenario', exact: true }).selectOption('pause');
    await page.getByRole('button', { name: 'Start', exact: true }).click();
    await expect(page.getByText('Run ended', { exact: false }).first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    await expect(page.getByTestId('handover-notice')).toHaveCount(0);
    await page.getByRole('button', { name: 'Results', exact: true }).click();
    await expect(page.getByText('Scenario: failed', { exact: true })).toBeVisible();
  } finally {
    await server.stop();
  }
});
