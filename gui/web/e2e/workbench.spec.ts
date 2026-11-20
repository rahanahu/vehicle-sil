import { expect, test } from '@playwright/test';
import { copyFileSync, mkdtempSync, rmSync } from 'node:fs';
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
    await ecu.close();

    await page.getByRole('button', { name: 'Stop', exact: true }).click();
    await expect(page.getByText('Run ended', { exact: false }).first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    await page.getByRole('button', { name: 'Scenario', exact: true }).click();
    await page.getByRole('button', { name: 'Load recording' }).click();
    await expect(page.getByRole('textbox', { name: 'Scenario YAML' })).toHaveValue(
      /engine\.demand.*70/,
    );
    // Starting a second, unrecorded run must not attribute the first run's recording to it.
    await page.getByRole('button', { name: 'Start', exact: true }).click();
    await expect(page.getByText('Running', { exact: true }).first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    await page.getByRole('button', { name: 'Stop', exact: true }).click();
    await expect(page.getByText('Run ended', { exact: false }).first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    await expect(page.getByRole('button', { name: 'Load recording' })).toHaveCount(0);
    expect(errors).toEqual([]);
  } finally {
    await server.stop();
  }
});
