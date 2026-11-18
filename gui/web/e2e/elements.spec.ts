// A screen made only of the parts (elements-page/: no script of its own except one line that calls connect()), served by
// the GUI server's --web with the two bundles next to it (npm run build:client), against the replaying launcher.
// The parts must start, pause, step and stop a run, write a variable and show it, and the page's script and the parts
// must share one connection (one WebSocket).
import { expect, test } from '@playwright/test';
import { copyFileSync, cpSync, existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  CONFIGS,
  REPLAY_LAUNCHER,
  GuiServer,
  REPOSITORY,
  launchGuiServer,
} from './support/gui-server';
import { RUN_TIMEOUT } from './support/run-page';

// The launcher features that the variables and the scenario need (as the replaying launcher is told to say)
const CONTROL_FEATURES =
  'events,stdin-commands,check-variables,speed,start-paused,time-commands,control-links,control-server,' +
  'scenario,timeline';
const BUILD = join(REPOSITORY, 'build', 'gui-client');
const BUNDLES = ['vehicle-sil-gui-client.mjs', 'vehicle-sil-gui-elements.mjs'];

let screen: string;
const cleanups: (() => Promise<void>)[] = [];

test.afterEach(async () => {
  for (const cleanup of cleanups.splice(0)) await cleanup();
});

test.beforeAll(async () => {
  // Only once: the screen does not depend on the development server
  test.skip(test.info().project.name !== 'static', 'only once');
  screen = mkdtempSync(join(tmpdir(), 'vehicle-sil-gui-elements-'));
  cpSync(join(__dirname, 'elements-page'), screen, { recursive: true });
  for (const name of BUNDLES) {
    if (!existsSync(join(BUILD, name))) {
      throw new Error(`${join(BUILD, name)} is missing: run npm run build:client in gui/web first`);
    }
    copyFileSync(join(BUILD, name), join(screen, name));
  }
});

/** A GUI server that serves the screen, with `config` as the file that a start without a file runs. */
async function serve(config: string, features?: string): Promise<GuiServer> {
  return launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    config: join(CONFIGS, config),
    env: {
      REPLAY_TICK_MS: '50',
      REPLAY_VARIABLES: 'more',
      ...(features ? { REPLAY_FEATURES: features } : {}),
    },
    port: 0,
    web: screen,
  });
}

test.afterAll(async () => {
  if (screen) rmSync(screen, { recursive: true, force: true });
});

test('a screen of parts runs and stops, writes a variable and says why a button cannot be used', async ({
  page,
}) => {
  const server = await serve('forever.yaml', CONTROL_FEATURES);
  cleanups.push(() => server.stop());
  test.setTimeout(120_000);
  const errors: string[] = [];
  page.on('pageerror', (error) => errors.push(error.message));
  page.on('console', (message) => {
    if (message.type() === 'error') errors.push(message.text());
  });
  const sockets: string[] = [];
  page.on('websocket', (socket) => sockets.push(socket.url()));
  const runs: unknown[] = [];
  page.on('request', (request) => {
    if (request.url().endsWith('/api/run')) runs.push(request.postDataJSON());
  });

  await page.goto(server.url);
  await expect(page.locator('body')).toHaveAttribute('data-own', 'connected');
  const status = page.getByTestId('status');
  await expect(status).toHaveAttribute('data-link', 'online');
  await expect(status).toHaveAttribute('data-run', 'none');
  const button = (testid: string, name: string) =>
    page.getByTestId(testid).getByRole('button', { name, exact: true });

  // The scenarios next to the file are listed
  const scenario = page.getByTestId('scenario').getByRole('combobox');
  await expect(scenario.locator('option')).not.toHaveCount(1);
  await scenario.selectOption({ index: 1 });
  await scenario.selectOption({ index: 0 }); // no scenario: the replaying launcher has no recording of a check with one

  // Nothing runs: these cannot be used, and the part says why
  await expect(button('time', 'Pause')).toHaveAttribute('aria-disabled', 'true');
  await expect(button('run', 'Stop')).toHaveAttribute('aria-disabled', 'true');

  await button('run', 'Start').click();
  await expect(status).toHaveAttribute('data-run', 'running', { timeout: RUN_TIMEOUT });
  expect(runs).toHaveLength(1);
  expect(runs[0]).not.toHaveProperty('scenario');
  await expect(button('run', 'Start')).toHaveAttribute('aria-disabled', 'true');
  await expect(button('run', 'Start')).toHaveAttribute('data-reason', /.+/);
  await expect(button('run', 'Stop')).toHaveAttribute('aria-disabled', 'false');
  // A running run cannot be stepped: the button says so, and pressing it sends nothing
  await expect(button('time', 'Step')).toHaveAttribute('aria-disabled', 'true');
  await expect(button('time', 'Step')).toHaveAttribute('data-reason', /.+/);
  await expect(button('time', 'Pause')).toHaveAttribute('aria-disabled', 'false');
  await expect(status.locator('.vsil-time')).toHaveText(/^\d+ ms/);

  // A slider, a button and a value show the same variable
  await expect(page.getByTestId('slider').locator('input')).toBeEnabled();
  await button('write', 'Demand 80').click();
  await expect(page.getByTestId('demand').locator('output')).toHaveText(/^80 /);
  await expect(page.getByTestId('slider').locator('input')).toHaveValue('80');
  await expect(page.getByTestId('warning').locator('output')).toHaveText('WARNING');
  await expect(page.getByTestId('warning')).toHaveAttribute('data-on', 'true');
  await expect(button('write', 'Demand 80')).toHaveAttribute('aria-disabled', 'true');

  await button('run', 'Stop').click();
  await expect(status).not.toHaveAttribute('data-run', 'running', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('slider').locator('input')).toBeDisabled();

  // One connection for the page's script and every part
  expect(sockets).toHaveLength(1);
  expect(errors).toEqual([]);
});

test('a refusal of the page own button shows in the status part: the connection is shared', async ({
  page,
}) => {
  const server = await serve('forever.yaml', CONTROL_FEATURES);
  cleanups.push(() => server.stop());
  const sockets: string[] = [];
  page.on('websocket', (socket) => sockets.push(socket.url()));
  await page.goto(server.url);
  await expect(page.locator('body')).toHaveAttribute('data-own', 'connected');
  const status = page.getByTestId('status');
  await expect(status).toHaveAttribute('data-link', 'online');
  await page.getByTestId('own-pause').click();
  await expect(status.getByRole('alert')).not.toBeEmpty();
  expect(sockets).toHaveLength(1);
});

test('the start button runs the scenario that the scenario part chose, and the result part shows it', async ({
  page,
}) => {
  const server = await serve('simulation.yaml', CONTROL_FEATURES);
  cleanups.push(() => server.stop());
  const runs: unknown[] = [];
  page.on('request', (request) => {
    if (request.url().endsWith('/api/run')) runs.push(request.postDataJSON());
  });
  await page.goto(server.url);
  const scenario = page.getByTestId('scenario').getByRole('combobox');
  await expect(scenario.locator('option', { hasText: 'fails.yaml' })).toHaveCount(1);
  await scenario.selectOption({ label: 'fails.yaml' });
  await page.getByTestId('run').getByRole('button', { name: 'Start', exact: true }).click();
  await expect(page.getByTestId('scenario-result').locator('output')).toHaveText('failed', {
    timeout: RUN_TIMEOUT,
  });
  expect(runs).toEqual([expect.objectContaining({ scenario: 'scenarios/fails.yaml' })]);
  await expect(page.getByTestId('scenario-result').locator('li').first()).toBeVisible();
});

test('a run that starts paused, by the intent property of the start part: what can be pressed', async ({
  page,
}) => {
  const server = await serve('forever.yaml');
  cleanups.push(() => server.stop());
  const runs: unknown[] = [];
  page.on('request', (request) => {
    if (request.url().endsWith('/api/run')) runs.push(request.postDataJSON());
  });
  await page.goto(server.url.replace('#', '?paused#'));
  const status = page.getByTestId('status');
  const button = (testid: string, name: string) =>
    page.getByTestId(testid).getByRole('button', { name, exact: true });
  await expect(status).toHaveAttribute('data-link', 'online');
  await button('run', 'Start').click();
  await expect(status).toHaveAttribute('data-run', 'paused', { timeout: RUN_TIMEOUT });
  expect(runs).toEqual([expect.objectContaining({ start_paused: true })]);
  await expect(button('time', 'Pause')).toHaveAttribute('data-reason', 'paused');
  await expect(button('time', 'Resume')).toHaveAttribute('aria-disabled', 'false');
  await expect(button('time', 'Step')).toHaveAttribute('aria-disabled', 'false');
  await button('run', 'Stop').click();
  await expect(status).not.toHaveAttribute('data-run', 'paused', { timeout: RUN_TIMEOUT });
});
