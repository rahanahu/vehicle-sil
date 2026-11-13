// シナリオを選んで回す、操作をシナリオとして保存する、を流し直すランチャーで通す。流し直すランチャーに Control Server と
// scenario-control を持たせる（シナリオの実行中も変数を読める）ので、この試験だけの GUI サーバーを起動する。
// VEHICLE_SIL_GUI_SHOTS にディレクトリを書くと、シナリオが失敗で終わった実行の画面（幅 1280）も撮る。
import { expect, test } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import {
  CONFIGS,
  REPLAY_LAUNCHER,
  GUI_PORT,
  GuiServer,
  launchGuiServer,
} from './support/gui-server';
import { RUN_TIMEOUT } from './support/run-page';
import {
  Target,
  openVariables,
  startRun,
  stopRun,
  waitForVariables,
  widget,
} from './support/variables-page';

const FEATURES =
  'events,stdin-commands,check-variables,speed,start-paused,time-commands,control-links,scenario,' +
  'control-server,scenario-control';
const shots = process.env['VEHICLE_SIL_GUI_SHOTS'];

let server: GuiServer;
let target: Target;

test.beforeAll(async () => {
  test.skip(test.info().project.name !== 'static', 'only with the built screen');
  server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    env: { REPLAY_TICK_MS: '100', REPLAY_FEATURES: FEATURES },
    port: GUI_PORT + 400,
  });
  target = { base: new URL('/', server.url).href, token: server.token };
});

test.afterAll(async () => {
  await server?.stop();
});

test.afterEach(async ({ page }) => {
  if (target) await stopRun(page, target);
});

/** 構成ファイルとシナリオを選び、そのシナリオつきの照合が出るまで待つ。 */
async function chooseScenario(page: import('@playwright/test').Page, scenario: string) {
  await page.goto(`${target.base}#token=${target.token}`);
  const config = page.getByRole('combobox', { name: '構成ファイル' });
  await config.click();
  await page.getByRole('option', { name: 'simulation.yaml', exact: true }).click();
  const checked = page.waitForResponse(
    (r) => r.url().endsWith('/api/check') && r.request().postDataJSON().scenario === scenario,
  );
  await page.getByRole('combobox', { name: 'シナリオ' }).click();
  await page.getByRole('option', { name: scenario.split('/').pop(), exact: true }).click();
  await checked;
  await expect(page.getByTestId('check-scenario')).toContainText(scenario);
  await expect(page.getByTestId('start-paused')).toHaveClass(/mat-mdc-checkbox-disabled/);
}

async function startOnPage(page: import('@playwright/test').Page) {
  const start = page.getByTestId('start');
  await expect(start).toHaveAttribute('data-armed', 'true');
  const response = page.waitForResponse((r) => r.url().endsWith('/api/run'));
  await start.click();
  expect((await response).status()).toBe(200);
}

test('runs a scenario that passes, and one that fails, with its steps', async ({ page }) => {
  await chooseScenario(page, 'scenarios/throttle.yaml');
  await startOnPage(page);
  const card = page.getByTestId('scenario-card');
  await expect(card.getByTestId('scenario-outcome')).toHaveText(/シナリオは通りました/, {
    timeout: RUN_TIMEOUT,
  });
  await expect(card.getByTestId('scenario-step')).toHaveCount(3);
  await expect(page.getByTestId('result-card')).toContainText('シナリオが終わった');

  await chooseScenario(page, 'scenarios/fails.yaml');
  await startOnPage(page);
  await expect(card.getByTestId('scenario-outcome')).toHaveText(/シナリオは失敗しました/, {
    timeout: RUN_TIMEOUT,
  });
  const steps = card.getByTestId('scenario-step');
  await expect(steps.nth(1)).toContainText('scenario-failed');
  await expect(steps.nth(2)).toContainText('動かなかった');
  await expect(page.getByTestId('result-card')).toContainText('scenarios/fails.yaml:7');
  if (shots) {
    await page.setViewportSize({ width: 1280, height: 900 });
    await page.locator('mat-sidenav-content').evaluate((element) => element.scrollTo(0, 0));
    await page.waitForTimeout(500);
    await page.screenshot({ path: `${shots}/run-scenario-1280.png` });
  }
});

test('while a scenario runs, the variables can be read but not written', async ({ page }) => {
  await openVariables(page, target);
  // 速さ 0.001: シナリオの 2 つ目の手順（500 ms の待ち）が何分もかかり、シナリオは確かめ終わるまで終わらない
  // （速さを指定しないと、3 つの手順は 1 秒足らずで終わり、遅い機械では、確かめる前に実行が終わる）
  const response = await page.request.post(new URL('api/run', target.base).href, {
    headers: { Authorization: `Bearer ${target.token}`, Origin: new URL(target.base).origin },
    data: { file: 'forever.yaml', disable: [], scenario: 'scenarios/throttle.yaml', speed: 0.001 },
  });
  expect(response.status()).toBe(200);
  await expect(page.getByTestId('variables-notice')).toContainText('シナリオの実行中です', {
    timeout: RUN_TIMEOUT,
  });
  const demand = widget(page, 'Engine', 'engine.demand');
  await expect(demand.getByRole('slider', { name: 'engine.demand' })).toBeDisabled();
  await expect(demand).toContainText('シナリオの実行中は書けません');
});

test('saves the writes of a run as a scenario', async ({ page }) => {
  await startRun(page, target, 'forever.yaml');
  await page.goto(`${target.base}#token=${target.token}`);
  const card = page.getByTestId('recording-card');
  await expect(card.getByTestId('recording-empty')).toBeVisible();
  // 画面を通さずに書く（部品の操作は variables.spec.ts が確かめる）。変数が書ける状態になってから
  const session = await waitForVariables(page, target, 'Engine', RUN_TIMEOUT);
  const write = await page.request.post(new URL('api/write', target.base).href, {
    headers: { Authorization: `Bearer ${target.token}`, Origin: new URL(target.base).origin },
    data: { session, writes: { Engine: { 'engine.demand': 30 } } },
  });
  expect(write.status(), await write.text()).toBe(200);
  await expect(card.getByTestId('recording-count')).toContainText('1 件', { timeout: RUN_TIMEOUT });
  const download = page.waitForEvent('download');
  await card.getByTestId('recording-save').click();
  const file = await download;
  expect(file.suggestedFilename()).toMatch(/^forever-run\d+\.yaml$/);
  const text = await readFile(await file.path(), 'utf8');
  expect(text).toContain('vehicle_sil_scenario: 1');
  expect(text).toContain('Engine.engine.demand: 30.0');
});
