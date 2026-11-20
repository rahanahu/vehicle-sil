import { expect, test, type WebSocketRoute } from '@playwright/test';
import { copyFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
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
  'timeline',
].join(',');

test('workbench detached views stay connected to a real launcher', async ({ page }) => {
  const launcher = process.env['VEHICLE_SIL_WORKBENCH_REAL_LAUNCHER'];
  const examples = process.env['VEHICLE_SIL_WORKBENCH_REAL_EXAMPLES'];
  test.skip(!launcher || !examples || test.info().project.name !== 'static');
  test.setTimeout(120_000);
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
  try {
    const requestUrls: string[] = [];
    page.context().on('request', (request) => requestUrls.push(request.url()));
    await page.goto(server.url);
    await page.getByRole('button', { name: 'Start', exact: true }).click();
    const throttle = page.getByRole('spinbutton', { name: 'engine.throttle', exact: true });
    await expect(throttle).toBeEnabled({ timeout: RUN_TIMEOUT });
    await page.locator('app-io-row').filter({ hasText: 'engine.speed' }).hover();
    await page.getByRole('button', { name: 'Add engine.speed to waveforms' }).click();
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
    const detachedThrottle = ecu.getByRole('spinbutton', { name: 'engine.throttle', exact: true });
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
  } finally {
    await server.stop();
    rmSync(root, { recursive: true, force: true });
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
    await expect(page.getByRole('button', { name: 'Results', exact: true })).toBeVisible();
    const example = JSON.parse(
      readFileSync(join(REPOSITORY, 'gui/protocol/examples/session-ended.json'), 'utf8'),
    );
    const session = example.response.session;
    session.id = 1000;
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
    await page.getByRole('button', { name: 'Results', exact: true }).click();
    await expect(page.getByRole('heading', { name: 'Run #1000 · failed' })).toBeVisible();
    await expect(page.getByText('Scenario: passed', { exact: true })).toBeVisible();
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
    await page.route(
      '**/api/run',
      async (route) => {
        const response = await route.fetch();
        await new Promise((resolve) => setTimeout(resolve, 5500));
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
    await page.getByRole('button', { name: 'Record & start' }).click();
    expect((await start).postDataJSON()).not.toHaveProperty('stop_at_ms');
    await started;
    await expect(page.getByText('Recording', { exact: false }).first()).toBeVisible();
    await expect(observer.getByText('Network: forever.yaml', { exact: true })).toBeVisible();
    await expect(observer.getByText('Scenario: Free run', { exact: true })).toBeVisible();
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
    await page.getByRole('button', { name: 'Scenario', exact: true }).click();
    await page.getByRole('button', { name: 'Load recording' }).click();
    await expect(page.getByRole('textbox', { name: 'Scenario YAML' })).toHaveValue(
      /engine\.demand.*70/,
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
    await page.waitForTimeout(11_000);
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
