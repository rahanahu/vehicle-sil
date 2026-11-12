// 実行の画面を、本物の GUI サーバー、本物のランチャー（vehicle_sil_launch）、例の ECU（Engine と Dashboard）で通す。
// 構成ファイルは gui/server/tests/real_configs.py が書いたもの（e2e/support/real-server.ts）。
// 環境変数 VEHICLE_SIL_GUI_SHOTS にディレクトリを書くと、4 つの状態を 3 つの幅で撮る（窓は伸ばさない）。
import { Page, expect, test } from '@playwright/test';
import { VIEWPORTS } from '../support/layout';
import {
  RUN_TIMEOUT,
  choose,
  openRun,
  press,
  result,
  start,
  startButton,
  stopButton,
  stopIfRunning,
} from '../support/run-page';

test.beforeEach(async ({ page }) => {
  await openRun(page);
  await stopIfRunning(page);
});

test('the real check: the participants, their steps and their variables', async ({ page }) => {
  await choose(page, 'simulation.yaml');
  await expect(page.getByTestId('check-outcome')).toContainText('通った');
  // 例の ECU が --describe で申告するもの（Engine は変数 2 つ、Dashboard は 1 つ）
  for (const [name, variables] of [
    ['Engine', 2],
    ['Dashboard', 1],
  ] as const) {
    const row = page.getByTestId(`check-participant-${name}`);
    await expect(row).toContainText('照合した');
    await expect(row).toContainText('J1939_CAN');
    await expect(row).toContainText(new RegExp(`変数:\\s*${variables}`));
  }
});

test('a real run that passes', async ({ page }) => {
  await choose(page, 'simulation.yaml');
  const session = await start(page);
  await expect(result(page, session).getByTestId('result')).toContainText('成功', {
    timeout: RUN_TIMEOUT,
  });
  await expect(page.getByTestId('outcome')).toHaveAttribute('data-tone', 'good');
  for (const name of ['Engine', 'Dashboard']) {
    await expect(page.getByTestId(`status-${name}`)).toContainText('終了した');
    await expect(page.getByTestId(`status-${name}`)).toContainText('終了コード 0');
  }
  // 仮想時刻は stop_at_ms（1000 ms）、ログには ECU の行
  await expect(page.getByTestId('sim-time')).toHaveText('1.000 s');
  await expect(
    page.getByTestId('log-line').filter({ hasText: 'engine speed' }).first(),
  ).toBeAttached();
  await expect(page.getByTestId('problems-card')).toHaveCount(0);
  await expect(startButton(page)).toHaveText(/もう一度実行/);
});

test('a real run with a participant left out', async ({ page }) => {
  await choose(page, 'simulation.yaml');
  const recheck = page.waitForResponse(
    (r) =>
      r.url().endsWith('/api/check') && r.request().postDataJSON().disable.includes('Dashboard'),
  );
  await page.getByRole('switch', { name: 'Dashboard を使う' }).click();
  await recheck;
  await expect(page.getByTestId('check-participant-Dashboard')).toContainText('外した');
  await expect(page.getByTestId('check-outcome')).toContainText('警告 1');
  const session = await start(page);
  await expect(result(page, session)).toContainText('成功', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('status-Dashboard')).toContainText('外した');
  await expect(page.getByTestId('status-Engine')).toContainText('終了コード 0');
});

test('stopping a real run that does not end by itself', async ({ page }) => {
  await choose(page, 'forever.yaml');
  const session = await start(page);
  await expect(page.getByTestId('outcome')).toContainText('実行中', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('status-Engine')).toContainText('参加した');
  await expect(page.getByTestId('log-line').first()).toBeVisible();
  // ランチャーは動いている間も仮想時刻を知らせる（time の事象に paused がある）: 出して、進む
  const simSeconds = async () =>
    Number((await page.getByTestId('sim-time').textContent())?.match(/[\d.]+/)?.[0] ?? NaN);
  await expect(page.getByTestId('sim-time')).toBeVisible({ timeout: RUN_TIMEOUT });
  const first = await simSeconds();
  await expect.poll(simSeconds, { timeout: RUN_TIMEOUT }).toBeGreaterThan(first);
  const stop = page.waitForRequest((r) => r.url().endsWith('/api/stop'));
  await press(stopButton(page));
  expect((await stop).postDataJSON()).toEqual({ force: false });
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
  await expect(result(page, session).getByTestId('result')).toHaveAttribute('data-tone', 'neutral');
  await expect(result(page, session)).toContainText('停止の命令');
  await expect(page.getByTestId('outcome')).not.toContainText('✕');
  for (const name of ['Engine', 'Dashboard']) {
    await expect(page.getByTestId(`status-${name}`)).toContainText('終了コード 0');
  }
});

test('configurations that fail the real check', async ({ page }) => {
  for (const [file, code] of [
    ['missing-executable.yaml', 'executable'],
    ['wrong-step.yaml', 'step-mismatch'],
  ]) {
    await choose(page, file);
    await expect(page.getByTestId('check-outcome')).toContainText('誤りがある');
    await expect(page.getByTestId('check-card').getByTestId('finding').first()).toContainText(code);
    await expect(startButton(page)).toHaveAttribute('aria-disabled', 'true');
  }
  await choose(page, 'broken.yaml');
  await expect(page.getByTestId('check-outcome')).toContainText('構成ファイルを読めない');
});

test('a real run that fails: a participant that never joins', async ({ page }) => {
  await choose(page, 'not-joined.yaml');
  const session = await start(page);
  await expect(result(page, session)).toContainText('失敗', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('first-error')).toContainText('not-joined');
  await expect(page.getByTestId('outcome')).toHaveAttribute('data-tone', 'bad');
  // ランチャーが止めた participant は、失敗の色にしない（失敗の理由は Logger が参加しなかったこと）
  for (const name of ['Engine', 'Logger']) {
    await expect(page.getByTestId(`status-${name}`)).toHaveAttribute('data-state', 'stopped');
  }
});

/** 撮る状態: 照合の結果、実行中、成功で終わった、失敗で終わった。 */
async function reach(page: Page, state: string): Promise<void> {
  if (state === 'chosen') {
    await choose(page, 'simulation.yaml');
    return;
  }
  if (state === 'running') {
    await choose(page, 'forever.yaml');
    await start(page);
    await expect(page.getByTestId('outcome')).toContainText('実行中', { timeout: RUN_TIMEOUT });
    await expect(page.getByTestId('log-line').nth(5)).toBeVisible();
    return;
  }
  await choose(page, state === 'ended' ? 'simulation.yaml' : 'not-joined.yaml');
  await start(page);
  await expect(startButton(page)).toHaveText(/もう一度実行/, { timeout: RUN_TIMEOUT });
}

test.describe('screenshots', () => {
  const directory = process.env['VEHICLE_SIL_GUI_SHOTS'];
  test.skip(!directory, 'VEHICLE_SIL_GUI_SHOTS is not set');
  for (const state of ['chosen', 'running', 'ended', 'failed']) {
    test(state, async ({ page }) => {
      await reach(page, state);
      const content = page.locator('mat-sidenav-content');
      for (const viewport of VIEWPORTS.filter((v) => [360, 1024, 1920].includes(v.width))) {
        await page.setViewportSize({ width: viewport.width, height: viewport.height });
        await expect(page.getByTestId('nav')).toHaveAttribute('data-nav-mode', viewport.navMode);
        await content.evaluate((element) => element.scrollTo(0, 0));
        await page.waitForTimeout(500);
        await page.screenshot({ path: `${directory}/real-${state}-${viewport.width}.png` });
      }
      await stopIfRunning(page);
    });
  }
});

test('started paused: the screen resumes the run from before its first step, and it passes', async ({
  page,
}) => {
  // Control Server を持つランチャーは、最初のステップの前で待つ（time の事象を書かない）。持たないランチャーは、
  // 時刻 0 のステップの後に止まる。どちらでも、画面の「再開」で最後まで走る
  await choose(page, 'simulation.yaml');
  await page.getByTestId('start-paused').getByRole('checkbox').check();
  const session = await start(page);
  await expect(page.getByTestId('resume')).not.toHaveAttribute('aria-disabled', 'true', {
    timeout: RUN_TIMEOUT,
  });
  const sent = page.waitForRequest((r) => r.url().endsWith('/api/time'));
  await page.getByTestId('resume').click();
  expect((await sent).postDataJSON()).toEqual({ command: 'resume' });
  await expect(result(page, session).getByTestId('result')).toContainText('成功', {
    timeout: RUN_TIMEOUT,
  });
  await expect(page.getByTestId('sim-time')).toHaveText('1.000 s');
});
