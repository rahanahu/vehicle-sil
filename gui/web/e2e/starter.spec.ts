// The starter of a screen of your own (examples/gui_starter), served as it is handed out: copied to a directory and
// served with --web, with no copy of the library next to it (the page loads the parts and the library from the
// server's vsil/, which npm run build:client builds). The replaying launcher and a fake Control Server are the other
// side, so the variables are those of the fake (the starter names those of examples/ecu_main), and the test renames
// them the way README.md tells a user to rename them for their own ECU.
// What it must do: the parts run and stop, write a variable and show it; the script of its own (main.js) shows a value,
// reports a refused operation and shows the timeline of a scenario run; the page makes one connection.
import { expect, test } from '@playwright/test';
import {
  cpSync,
  existsSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
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

const CONTROL_FEATURES =
  'events,stdin-commands,check-variables,speed,start-paused,time-commands,control-links,control-server,' +
  'scenario,timeline';
const BUILD = join(REPOSITORY, 'build', 'gui-client');
// The starter's variables, and the fake's (README.md: change the variable attributes to your own)
const RENAME: [string, string][] = [
  ['Engine.engine.throttle', 'Engine.engine.demand'],
  ['Dashboard.dashboard.engine_lost', 'Engine.engine.warning'],
];

let screen: string;
const cleanups: (() => Promise<void>)[] = [];

test.afterEach(async () => {
  for (const cleanup of cleanups.splice(0)) await cleanup();
});

test.beforeAll(() => {
  // Not in the development server's form (e2e:dev): the starter does not use the sample screens
  test.skip(test.info().project.name !== 'static', 'only once');
  for (const name of ['vehicle-sil-gui-client.mjs', 'vehicle-sil-gui-elements.mjs']) {
    if (!existsSync(join(BUILD, name))) {
      throw new Error(`${join(BUILD, name)} is missing: run npm run build:client in gui/web first`);
    }
  }
  screen = mkdtempSync(join(tmpdir(), 'vehicle-sil-gui-starter-'));
  cpSync(join(REPOSITORY, 'examples', 'gui_starter'), screen, { recursive: true });
  for (const name of ['index.html', 'main.js']) {
    let text = readFileSync(join(screen, name), 'utf8');
    for (const [from, to] of RENAME) text = text.replaceAll(from, to);
    writeFileSync(join(screen, name), text);
  }
});

test.afterAll(() => {
  if (screen) rmSync(screen, { recursive: true, force: true });
});

async function serve(config: string): Promise<GuiServer> {
  const server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    config: join(CONFIGS, config),
    // more: the variables that a slider and a lamp need (engine.demand has a range; engine.warning is true above 50)
    env: { REPLAY_TICK_MS: '50', REPLAY_FEATURES: CONTROL_FEATURES, REPLAY_VARIABLES: 'more' },
    port: 0,
    web: screen,
  });
  cleanups.push(() => server.stop());
  return server;
}

test('the starter runs and stops, writes and shows variables, and reports a refused operation of its own', async ({
  page,
}) => {
  test.setTimeout(120_000);
  // The starter has no copy of the library: it comes from the server
  expect(readdirSync(screen).filter((name) => name.endsWith('.mjs'))).toEqual([]);
  const server = await serve('forever.yaml');
  const errors: string[] = [];
  page.on('pageerror', (error) => errors.push(error.message));
  page.on('console', (message) => {
    if (message.type() === 'error') errors.push(message.text());
  });
  const sockets: string[] = [];
  page.on('websocket', (socket) => sockets.push(socket.url()));
  await page.goto(server.url);
  const status = page.locator('vsil-status');
  await expect(status).toHaveAttribute('data-link', 'online');
  expect(new URL(page.url()).hash).toBe(''); // the token left the URL
  await expect(status).toHaveAttribute('data-run', 'none');
  // The attribute of the time part reaches its list of speeds
  await expect(page.locator('vsil-time-controls select option')).toHaveText([
    '-',
    '1×',
    '5×',
    'fastest',
  ]);
  const button = (name: string) => page.getByRole('button', { name, exact: true });

  // Nothing runs: the button of its own is refused, and says so next to itself (the parts keep to their own words)
  await button('Throttle to 100 %').click();
  await expect(page.locator('#floor-error')).toHaveText(/\w+/);

  await button('Start').click();
  await expect(status).toHaveAttribute('data-run', 'running', { timeout: RUN_TIMEOUT });
  await expect(page.locator('#floor-error')).toHaveText(/\w+/); // still the old sentence: it is the script's to clear

  // A part writes: the slider. The fake ECU raises the warning above 50, which another part shows
  const slider = page.locator('vsil-slider input');
  const warning = page.locator('vsil-value[variable="Engine.engine.warning"]');
  await expect(slider).toBeEnabled();
  await expect(warning).toHaveAttribute('data-on', 'false');
  await slider.fill('70');
  await expect(warning).toHaveAttribute('data-on', 'true');
  await slider.fill('30');
  await expect(warning).toHaveAttribute('data-on', 'false');
  // The button of its own writes through the same connection, and the parts show it
  await button('Throttle to 100 %').click();
  await expect(slider).toHaveValue('100');
  await expect(warning).toHaveAttribute('data-on', 'true');
  await expect(page.locator('#floor-error')).toHaveText('');
  // The display of its own
  await expect(page.locator('#rpm-text')).toHaveText(/^\d+ rpm$/);
  await expect(page.locator('#rpm')).toHaveAttribute('data-stale', 'false');
  // A part writes: the button
  await button('Cut engine from the bus').click();
  await expect(button('Cut engine from the bus')).toHaveAttribute('aria-disabled', 'true');

  await button('Stop').click();
  await expect(status).not.toHaveAttribute('data-run', 'running', { timeout: RUN_TIMEOUT });
  await expect(slider).toBeDisabled();
  await expect(status).toHaveAttribute('data-run', 'ended');
  // What is not a live value any more is marked (style.css dims it), also in the display of its own
  await expect(page.locator('#rpm')).toHaveAttribute('data-stale', 'true');
  expect(sockets).toHaveLength(1);
  expect(errors).toEqual([]);
});

test('the starter runs a scenario and shows its timeline', async ({ page }) => {
  test.setTimeout(120_000);
  const server = await serve('simulation.yaml');
  const errors: string[] = [];
  page.on('pageerror', (error) => errors.push(error.message));
  page.on('console', (message) => {
    if (message.type() === 'error') errors.push(message.text());
  });
  await page.goto(server.url);
  await expect(page.locator('vsil-status')).toHaveAttribute('data-link', 'online');
  const timeline = page.locator('vehicle-sil-timeline');
  await expect(timeline).toBeHidden(); // no scenario run yet
  const scenario = page.locator('vsil-scenario-select select');
  await expect(scenario.locator('option', { hasText: 'fails.yaml' })).toHaveCount(1);
  await scenario.selectOption({ label: 'fails.yaml' });
  await page.getByRole('button', { name: 'Start', exact: true }).click();
  await expect(page.locator('vsil-scenario-result')).toHaveAttribute('data-status', 'failed', {
    timeout: RUN_TIMEOUT,
  });
  await expect(timeline.getByTestId('failure').getByRole('heading', { level: 3 })).toHaveText(
    /^Step 2 failed at [\d.]+ ms$/,
    { timeout: RUN_TIMEOUT },
  );
  await expect(timeline).toBeVisible();
  await expect(timeline.locator('#help')).toBeHidden();
  const shots = process.env['VEHICLE_SIL_GUI_SHOTS'];
  if (shots) {
    await page.setViewportSize({ width: 1280, height: 900 });
    await page.screenshot({ path: `${shots}/element-starter-light-1280.png`, fullPage: true });
  }
  expect(errors).toEqual([]);
});
