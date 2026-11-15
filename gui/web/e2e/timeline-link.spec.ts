// The timeline of a scenario run from the GUI: the GUI server gives the launcher --timeline (the replaying
// launcher has the feature timeline, and its recording of the failing scenario has the file), the scenario card
// links the timeline page, and the page opens in a new tab in the GUI's language with the run's file, which it
// reads with the token of the GUI's tab. VEHICLE_SIL_GUI_SHOTS=<directory> also saves screenshots of the link
// and of the page in Japanese.
import { Page, expect, test } from '@playwright/test';
import { CONFIGS, REPLAY_LAUNCHER, GuiServer, launchGuiServer } from './support/gui-server';
import { RUN_TIMEOUT } from './support/run-page';

const shots = process.env['VEHICLE_SIL_GUI_SHOTS'];
const FAILS = 'scenarios/fails.yaml';

let server: GuiServer;
let base: string;

test.beforeAll(async () => {
  test.skip(test.info().project.name !== 'static', 'only with the built screen');
  server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    env: { REPLAY_TICK_MS: '10' },
    port: 0,
  });
  base = new URL('/', server.url).href;
});

test.afterAll(async () => {
  await server?.stop();
});

/** Chooses simulation.yaml with the failing scenario on the run page (in Japanese) and starts it. */
async function runFailingScenario(page: Page): Promise<void> {
  await page.goto(`${base}#token=${server.token}`);
  await page.getByRole('combobox', { name: '構成ファイル' }).click();
  await page.getByRole('option', { name: 'simulation.yaml', exact: true }).click();
  const checked = page.waitForResponse(
    (r) => r.url().endsWith('/api/check') && r.request().postDataJSON().scenario === FAILS,
  );
  await page.getByRole('combobox', { name: 'シナリオ' }).click();
  await page.getByRole('option', { name: 'fails.yaml', exact: true }).click();
  await checked;
  const start = page.getByTestId('start');
  await expect(start).toHaveAttribute('data-armed', 'true');
  const response = page.waitForResponse((r) => r.url().endsWith('/api/run'));
  await start.click();
  expect((await response).status()).toBe(200);
}

test('timeline: a failed scenario run links its timeline, which opens in the GUI language', async ({
  page,
  browser,
}) => {
  await runFailingScenario(page);
  const card = page.getByTestId('scenario-card');
  await expect(card.getByTestId('scenario-outcome')).toHaveText(/シナリオは失敗しました/, {
    timeout: RUN_TIMEOUT,
  });
  const link = card.getByTestId('scenario-timeline');
  await expect(link).toHaveText('時間の軸');
  const href = (await link.getAttribute('href')) ?? '';
  expect(href).toMatch(/^timeline\/\?src=%2Fapi%2Ftimeline%2F\d+&lang=ja$/);
  expect(href).not.toContain(server.token);
  if (shots) {
    await page.setViewportSize({ width: 1280, height: 900 });
    await link.scrollIntoViewIfNeeded();
    await card.screenshot({ path: `${shots}/t2b-link-ja.png` });
  }

  const opened = page.context().waitForEvent('page');
  await link.click();
  const timeline = await opened;
  const problems: string[] = [];
  timeline.on('console', (m) => {
    if (m.type() === 'error' || m.type() === 'warning') problems.push(m.text());
  });
  await expect(timeline.locator('html')).toHaveAttribute('lang', 'ja');
  const failure = timeline.getByTestId('failure');
  await expect(failure.getByRole('heading', { level: 2 })).toHaveText(
    /^手順 2 が [\d.]+ ms で失敗しました$/,
    { timeout: RUN_TIMEOUT },
  );
  await expect(timeline.locator('#facts')).toContainText('fails.timeline.json');
  await expect(timeline.getByRole('button', { name: 'ファイルを開く' })).toBeVisible();
  if (shots) {
    await timeline.setViewportSize({ width: 1280, height: 900 });
    await timeline.screenshot({ path: `${shots}/t2b-page-ja.png`, fullPage: true });
  }
  expect(problems).toEqual([]);

  // Without the GUI's tab (no token in this browser), the server refuses the file, and the page says what to do.
  const stranger = await browser.newContext({ locale: 'ja-JP' });
  try {
    const alone = await stranger.newPage();
    await alone.goto(new URL(href, base).href);
    await expect(alone.getByRole('alert')).toContainText('HTTP 401');
    await expect(alone.getByRole('alert')).toContainText(
      '「時間の軸」のリンクから開き直してください',
    );
  } finally {
    await stranger.close();
  }
});

test('timeline: in an English GUI the link says Timeline and the page is in English', async ({
  page,
}) => {
  // The GUI's choice beats the browser's language (ja-JP here): the link passes it on with lang=.
  await page.addInitScript(() => window.localStorage.setItem('vehicle-sil-gui-language', 'en'));
  const run = await page.request.post(new URL('api/run', base).href, {
    headers: { Authorization: `Bearer ${server.token}`, Origin: new URL(base).origin },
    data: { file: 'simulation.yaml', scenario: FAILS },
  });
  expect(run.status(), await run.text()).toBe(200);
  await page.goto(`${base}#token=${server.token}`);
  const link = page.getByTestId('scenario-card').getByTestId('scenario-timeline');
  await expect(link).toHaveText('Timeline', { timeout: RUN_TIMEOUT });
  const opened = page.context().waitForEvent('page');
  await link.click();
  const timeline = await opened;
  await expect(timeline.locator('html')).toHaveAttribute('lang', 'en');
  await expect(timeline.getByTestId('failure').getByRole('heading', { level: 2 })).toHaveText(
    /^Step 2 failed at [\d.]+ ms$/,
    { timeout: RUN_TIMEOUT },
  );
});
