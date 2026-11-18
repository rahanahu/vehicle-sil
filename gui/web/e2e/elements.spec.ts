// A screen made only of the parts (elements-page/: no script of its own except one line that calls connect()), served by
// the GUI server's --web, which loads the two bundles from the server's own vsil/ (built by npm run build:client), against
// the replaying launcher.
// The parts must start, pause, step and stop a run, write a variable and show it, and the page's script and the parts
// must share one connection (one WebSocket).
import { Locator, expect, test } from '@playwright/test';
import { cpSync, existsSync, mkdtempSync, rmSync } from 'node:fs';
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
  'events,stdin-commands,request,stdin-run-until,state,check-variables,speed,start-paused,time-commands,control-links,control-server,' +
  'scenario,timeline';
const BUILD = join(REPOSITORY, 'build', 'gui-client');
const BUNDLES = ['vehicle-sil-gui-client.mjs', 'vehicle-sil-gui-elements.mjs'];

/** The sentence that describes a button (aria-describedby), hidden or not. */
const described = (button: Locator) =>
  button.evaluate(
    (element) => document.getElementById(element.getAttribute('aria-describedby')!)?.textContent,
  );

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
  // The page copies nothing: the server serves the bundles from here (and says what to run when they are not there)
  for (const name of BUNDLES) {
    if (!existsSync(join(BUILD, name))) {
      throw new Error(`${join(BUILD, name)} is missing: run npm run build:client in gui/web first`);
    }
  }
});

/** A GUI server that serves the screen, with `config` as the file that a start without a file runs. */
async function serve(
  config: string,
  features?: string,
  env: Record<string, string> = {},
): Promise<GuiServer> {
  return launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    config: join(CONFIGS, config),
    env: {
      REPLAY_TICK_MS: '50',
      REPLAY_VARIABLES: 'more',
      ...(features ? { REPLAY_FEATURES: features } : {}),
      ...env,
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

  // Nothing runs: these cannot be used. The reason is in the description (hidden: the status part shows the state)
  await expect(button('time', 'Pause')).toHaveAttribute('aria-disabled', 'true');
  await expect(button('run', 'Stop')).toHaveAttribute('aria-disabled', 'true');
  expect(await described(button('run', 'Stop'))).toMatch(/\w+/);
  await expect(page.getByTestId('time').locator('select')).toHaveAttribute('aria-disabled', 'true');

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

  // Moving the slider writes the number, and the value part shows it
  const slider = page.getByTestId('slider').locator('input');
  await expect(slider).toBeEnabled();
  await slider.fill('60');
  await expect(page.getByTestId('demand').locator('output')).toHaveText(/^60 /);
  await expect(slider).toHaveValue('60');
  // A button writes the same variable
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

// A launcher without a Control Server: there is no recording yet of a scenario run with a Control Server and a
// timeline (the GUI server asks for both when the launcher has both), and this test writes no variable.
const SCENARIO_FEATURES = CONTROL_FEATURES.replace(',control-server', '');

test('the start button runs the scenario that the scenario part chose, and the result part shows it', async ({
  page,
}) => {
  const server = await serve('simulation.yaml', SCENARIO_FEATURES);
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
  await expect(page.locator('body')).toHaveAttribute('data-own', 'connected');
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

test('the resume part button resumes a run that started paused', async ({ page }) => {
  const server = await serve('simulation.yaml');
  cleanups.push(() => server.stop());
  await page.goto(server.url.replace('#', '?paused#'));
  await expect(page.locator('body')).toHaveAttribute('data-own', 'connected');
  const status = page.getByTestId('status');
  const button = (testid: string, name: string) =>
    page.getByTestId(testid).getByRole('button', { name, exact: true });
  await button('run', 'Start').click();
  await expect(status).toHaveAttribute('data-run', 'paused', { timeout: RUN_TIMEOUT });
  await button('time', 'Resume').click();
  // the run goes on to its end by itself
  await expect(status).toHaveAttribute('data-run', 'ended', { timeout: RUN_TIMEOUT });
});

test('the time part steps a paused run, then resumes it and pauses it again', async ({ page }) => {
  const server = await serve('forever.yaml');
  cleanups.push(() => server.stop());
  await page.goto(server.url.replace('#', '?paused#'));
  await expect(page.locator('body')).toHaveAttribute('data-own', 'connected');
  const status = page.getByTestId('status');
  const button = (testid: string, name: string) =>
    page.getByTestId(testid).getByRole('button', { name, exact: true });
  await expect(status).toHaveAttribute('data-link', 'online');
  await button('run', 'Start').click();
  await expect(status).toHaveAttribute('data-run', 'paused', { timeout: RUN_TIMEOUT });
  await expect(status.locator('.vsil-time')).toHaveText(/^0 ms/);

  // A step moves the time on (the launcher rounds the amount up to its tick) and the run is paused again
  await page.getByTestId('time').getByRole('spinbutton', { name: 'Step (ms)' }).fill('25');
  await button('time', 'Step').click();
  await expect(status.locator('.vsil-time')).toHaveText(/^30 ms/);
  await expect(status).toHaveAttribute('data-run', 'paused');

  // The resume button can be pressed again, and the run goes on
  await expect(button('time', 'Resume')).toHaveAttribute('aria-disabled', 'false');
  await button('time', 'Resume').click();
  await expect(status).toHaveAttribute('data-run', 'running', { timeout: RUN_TIMEOUT });
  await expect(button('time', 'Resume')).toHaveAttribute('aria-disabled', 'true');
  await expect(button('time', 'Step')).toHaveAttribute('aria-disabled', 'true');
  await expect(status.locator('.vsil-time')).not.toHaveText(/^30 ms/);

  // The pause button stops it: the time stays where it is
  await expect(button('time', 'Pause')).toHaveAttribute('aria-disabled', 'false');
  await button('time', 'Pause').click();
  await expect(status).toHaveAttribute('data-run', 'paused', { timeout: RUN_TIMEOUT });
  await expect(button('time', 'Resume')).toHaveAttribute('aria-disabled', 'false');

  // The rest of what the recording expects: a speed that the page offers, then the stop
  await page.getByTestId('time').evaluate((element) => element.setAttribute('speeds', '1 2 max'));
  await page.getByTestId('time').getByRole('combobox').selectOption({ label: '2×' });
  await expect(status).toHaveAttribute('data-run', 'paused');
  await button('run', 'Stop').click();
  await expect(status).not.toHaveAttribute('data-run', 'paused', { timeout: RUN_TIMEOUT });
});

test('the connect part lists its network only when it is opened, and the run it starts shows the connected row', async ({
  page,
}) => {
  // At the recorded pace the run of the recording is connected for about 10 s: long enough to see its row
  const server = await serve('connect.yaml', undefined, { REPLAY_TICK_MS: '1000' });
  cleanups.push(() => server.stop());
  test.setTimeout(120_000);
  const posted: { path: string; body: unknown }[] = [];
  page.on('request', (request) => {
    if (request.method() === 'POST')
      posted.push({ path: new URL(request.url()).pathname, body: request.postDataJSON() });
  });
  const listed = () => posted.filter((p) => p.path === '/api/connections');
  await page.goto(server.url);
  await expect(page.locator('body')).toHaveAttribute('data-own', 'connected');
  const status = page.getByTestId('status');
  await expect(status).toHaveAttribute('data-link', 'online');
  const connect = page.getByTestId('connect');
  const select = connect.getByRole('combobox');
  await expect(connect).toHaveAttribute('data-chosen', 'none');
  await expect(select).toHaveValue('none');
  // Opening the page asks for no list (no bridge starts on a computer that only runs SIL Kit)
  expect(listed()).toEqual([]);

  // Passing it by with Tab does not either
  await select.focus();
  await select.press('Tab');
  await expect(connect).toHaveAttribute('data-chosen', 'none');
  expect(listed()).toEqual([]);

  // Opening it does
  await select.click();
  await expect(select.locator('option[value="fake0"]')).toHaveCount(1);
  expect(listed()).toEqual([
    { path: '/api/connections', body: { file: 'connect.yaml', network: 'ETH_EXT' } },
  ]);
  // A target that the launcher says cannot be used cannot be chosen, and says why
  await expect(select.locator('option[value="tap:lo"]')).toBeDisabled();
  await expect(select.locator('option[value="tap:lo"]')).toContainText('not Ethernet');

  await select.selectOption('fake0');
  await expect(connect).toHaveAttribute('data-chosen', 'fake0');
  // The check says, in the launcher's words, that the run keeps real time
  await expect(connect.locator('.vsil-findings li[data-code="outside-speed-set"]')).toHaveText(
    /real time/,
  );
  await expect(page.getByTestId('outside').locator('li')).toHaveCount(0);

  await page.getByTestId('run').getByRole('button', { name: 'Start', exact: true }).click();
  const row = page.getByTestId('outside').locator('li');
  await expect(row).toHaveAttribute('data-network', 'ETH_EXT', { timeout: RUN_TIMEOUT });
  await expect(row).toContainText('fake0');
  expect(posted.filter((p) => p.path === '/api/run')).toEqual([
    { path: '/api/run', body: expect.objectContaining({ connect: { ETH_EXT: 'fake0' } }) },
  ]);
  // The run goes to its end, and the row goes with it
  await expect(status).toHaveAttribute('data-run', 'ended', { timeout: 3 * RUN_TIMEOUT });
  await expect(page.getByTestId('outside').locator('li')).toHaveCount(0);
  expect(listed()).toHaveLength(1);
});
