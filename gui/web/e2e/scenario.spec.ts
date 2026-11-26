// シナリオを選んで回す、操作をシナリオとして保存する、を流し直すランチャーで通す。流し直すランチャーに Control Server を
// 持たせる（シナリオの実行中も変数を読める）ので、この試験だけの GUI サーバーを起動する。
// VEHICLE_SIL_GUI_SHOTS にディレクトリを書くと、シナリオが失敗で終わった実行の画面（幅 1280）も撮る。
import { expect, test } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { CONFIGS, REPLAY_LAUNCHER, GuiServer, launchGuiServer } from './support/gui-server';
import { RUN_TIMEOUT, inSync, press, stopButton } from './support/run-page';
import {
  Target,
  openVariables,
  startRun,
  stopRun,
  waitForVariables,
  widget,
} from './support/variables-page';

const FEATURES =
  'events,stdin-commands,request,stdin-run-until,state,check-variables,speed,start-paused,manual-participants,time-commands,control-links,scenario,' +
  'control-server,scenario-control,scenario-end-ms,scenario-handover';
const shots = process.env['VEHICLE_SIL_GUI_SHOTS'];

let server: GuiServer;
let target: Target;

test.beforeAll(async () => {
  test.skip(test.info().project.name !== 'static', 'only with the built screen');
  server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    env: { REPLAY_TICK_MS: '25', REPLAY_FEATURES: FEATURES },
    port: 0,
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
  await inSync(page);
  const config = page.getByRole('combobox', { name: '構成ファイル' });
  await expect(config).toBeEnabled();
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
  await expect(page.getByTestId('variables-notice')).toContainText('シナリオの実行中は書けません', {
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
    data: { run_id: session, writes: { Engine: { 'engine.demand': 30 } } },
  });
  expect(write.status(), await write.text()).toBe(200);
  await expect(card.getByTestId('recording-count')).toContainText('1 件', { timeout: RUN_TIMEOUT });
  await expect(card.getByTestId('recording-end-ms')).toHaveCount(0);

  const midRec = (await (
    await page.request.get(new URL('api/recording', target.base).href, {
      headers: { Authorization: `Bearer ${target.token}` },
    })
  ).json()) as { recording: { end_ms: number | null; yaml: string } };
  expect(midRec.recording.end_ms).toBeNull();
  expect(midRec.recording.yaml).not.toMatch(/^end_ms:/m);

  const downloadMid = page.waitForEvent('download');
  await card.getByTestId('recording-save').click();
  const fileMid = await downloadMid;
  expect(fileMid.suggestedFilename()).toMatch(/^forever-run.+\.yaml$/);
  const midText = await readFile(await fileMid.path(), 'utf8');
  expect(midText).toContain('vehicle_sil_scenario: 1');
  expect(midText).toContain('Engine.engine.demand: 30.0');
  expect(midText).not.toMatch(/^end_ms:/m);

  // Stop the run cleanly and verify finalized end_ms is included without dummy expect/hold_for_ms
  await press(stopButton(page));
  await expect(page.getByTestId('result-card')).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(card.getByTestId('recording-end-ms')).toBeVisible({ timeout: RUN_TIMEOUT });

  const sessionResp = (await (
    await page.request.get(new URL('api/session', target.base).href, {
      headers: { Authorization: `Bearer ${target.token}` },
    })
  ).json()) as { session: { result: { timing: { sim_ms: number } } } };
  const endedSimMs = sessionResp.session.result.timing.sim_ms;

  const endRec = (await (
    await page.request.get(new URL('api/recording', target.base).href, {
      headers: { Authorization: `Bearer ${target.token}` },
    })
  ).json()) as { recording: { end_ms: number | null; yaml: string } };
  expect(endRec.recording.end_ms).toBe(endedSimMs);

  const downloadEnd = page.waitForEvent('download');
  await card.getByTestId('recording-save').click();
  const fileEnd = await downloadEnd;
  const endText = await readFile(await fileEnd.path(), 'utf8');
  expect(endText).toContain(`end_ms: ${endedSimMs}`);
  expect(endText).toContain('Engine.engine.demand: 30.0');
  expect(endText).not.toContain('#   - expect:');
  expect(endText).not.toContain('hold_for_ms:');
});

test('normal replay with default after_scenario completes without handover', async ({ page }) => {
  await chooseScenario(page, 'scenarios/throttle.yaml');
  await expect(page.getByTestId('after-scenario-stop')).toHaveClass(/mat-mdc-radio-checked/);
  const runReqPromise = page.waitForRequest(
    (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
  );
  await startOnPage(page);
  const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
  expect(runBody['after_scenario']).toBeUndefined();
  await expect(page.getByTestId('result-card')).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('scenario-handover')).toHaveCount(0);
  const sessionResp = (await (
    await page.request.get(new URL('api/session', target.base).href, {
      headers: { Authorization: `Bearer ${target.token}` },
    })
  ).json()) as { session: { scenario: { status: string; handed_over: boolean } } };
  expect(sessionResp.session.scenario).toMatchObject({ status: 'passed', handed_over: false });
});

test('handover selection disables stop_at_ms, pauses at end even at speed=max, and allows manual write + step on same run', async ({
  page,
}) => {
  await page.goto(`${target.base}#token=${target.token}`);
  await inSync(page);
  await page.getByTestId('stop-at-ms').fill('1000');

  await page.getByRole('combobox', { name: '構成ファイル' }).click();
  await page.getByRole('option', { name: 'simulation.yaml', exact: true }).click();
  await page.getByRole('combobox', { name: 'シナリオ' }).click();
  await page.getByRole('option', { name: 'throttle.yaml', exact: true }).click();
  await expect(page.getByTestId('after-scenario')).toBeVisible();

  const pauseCheck = page.waitForRequest(
    (r) =>
      r.url().endsWith('/api/check') &&
      (r.postDataJSON() as Record<string, unknown>)['after_scenario'] === 'pause',
  );
  await page.getByTestId('after-scenario-pause').click();
  await expect(page.getByTestId('stop-at-ms')).toBeDisabled();
  await expect(page.getByTestId('stop-at-ms')).toHaveValue('');
  const checkBody = (await pauseCheck).postDataJSON() as Record<string, unknown>;
  expect(checkBody['after_scenario']).toBe('pause');
  expect(checkBody['stop_at_ms']).toBeUndefined();

  await page.getByTestId('config-card').getByRole('radio', { name: '最速', exact: true }).click();
  const runReqPromise = page.waitForRequest(
    (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
  );
  const runResPromise = page.waitForResponse(
    (r) => r.url().endsWith('/api/run') && r.request().method() === 'POST',
  );
  await startOnPage(page);
  const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
  expect(runBody['after_scenario']).toBe('pause');
  expect(runBody['speed']).toBe('max');
  expect(runBody['stop_at_ms']).toBeUndefined();
  const { run_id: runId } = (await (await runResPromise).json()) as { run_id: string };

  await expect(page.getByTestId('scenario-handover')).toContainText('1000 ms', {
    timeout: RUN_TIMEOUT,
  });
  await expect(page.getByTestId('outcome')).toContainText('一時停止中');
  await expect(page.getByTestId('result-card')).toHaveCount(0);
  await expect(page.getByTestId('sim-time')).toHaveText('1.000 s');
  await page.waitForTimeout(300);
  await expect(page.getByTestId('sim-time')).toHaveText('1.000 s');

  // Switch to Variables page on the same run: write is unlocked after handover
  await openVariables(page, target);
  await expect(page.getByTestId('variables-notice')).toContainText('1000 ms', {
    timeout: RUN_TIMEOUT,
  });
  const demand = widget(page, 'Engine', 'engine.demand');
  const demandSlider = demand.getByRole('slider', { name: 'engine.demand' });
  await expect(demandSlider).toBeEnabled();
  await demandSlider.focus();
  await page.keyboard.press('End');
  await expect(demand).toContainText('受け付けました', { timeout: RUN_TIMEOUT });

  // Return to Run page on the same run_id and Step
  await page.getByTestId('nav').getByRole('link', { name: '実行' }).click();
  await expect(page.getByTestId('scenario-handover')).toBeVisible();
  await page.getByTestId('step-ms').fill('100');
  await page.getByTestId('step').click();
  await expect(page.getByTestId('sim-time')).toHaveText('1.100 s', { timeout: RUN_TIMEOUT });

  const sessionResp = (await (
    await page.request.get(new URL('api/session', target.base).href, {
      headers: { Authorization: `Bearer ${target.token}` },
    })
  ).json()) as {
    session: { id: string; result: unknown; scenario: { handed_over: boolean } };
  };
  expect(sessionResp.session.id).toBe(runId);
  expect(sessionResp.session.result).toBeNull();
  expect(sessionResp.session.scenario.handed_over).toBe(true);

  // Clicking Stop on the same handed-over run clears the handover badge immediately
  await press(stopButton(page));
  await expect(page.getByTestId('scenario-handover')).toHaveCount(0);
  await expect(page.getByTestId('result-card')).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('scenario-handover')).toHaveCount(0);
});

test('failed and interrupted scenarios with handover selected never count as handover', async ({
  page,
}) => {
  await chooseScenario(page, 'scenarios/fails.yaml');
  await page.getByTestId('after-scenario-pause').click();
  await startOnPage(page);
  await expect(page.getByTestId('scenario-outcome')).toHaveText(/シナリオは失敗しました/, {
    timeout: RUN_TIMEOUT,
  });
  await expect(page.getByTestId('result-card')).toBeVisible();
  await expect(page.getByTestId('scenario-handover')).toHaveCount(0);

  // Mid-scenario stop on slow run with after_scenario: 'pause'
  const runResp = await page.request.post(new URL('api/run', target.base).href, {
    headers: { Authorization: `Bearer ${target.token}`, Origin: new URL(target.base).origin },
    data: {
      file: 'forever.yaml',
      disable: [],
      scenario: 'scenarios/throttle.yaml',
      after_scenario: 'pause',
      speed: 0.001,
    },
  });
  expect(runResp.status()).toBe(200);
  await expect(page.getByTestId('scenario-card')).toBeVisible({ timeout: RUN_TIMEOUT });
  await press(stopButton(page));
  await expect(page.getByTestId('scenario-outcome')).toHaveText(/終わる前に止まりました/, {
    timeout: RUN_TIMEOUT,
  });
  await expect(page.getByTestId('scenario-handover')).toHaveCount(0);
});
