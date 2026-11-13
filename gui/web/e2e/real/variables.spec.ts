// 変数の画面を、本物のランチャー（Control Server つき）と例の ECU で通す。ランチャーが Control Server を
// 持たなければ飛ばす。環境変数 VEHICLE_SIL_GUI_SHOTS にディレクトリを書くと、値が並んだところを 2 つの幅で撮る。
import { expect, test } from '@playwright/test';
import { GUI_PORT, guiToken } from '../support/gui-server';
import { RUN_TIMEOUT } from '../support/run-page';
import {
  Target,
  numberOf,
  openVariables,
  startRun,
  stopRun,
  widget,
} from '../support/variables-page';

test('the example ECUs: writing the throttle changes the engine speed', async ({ page }) => {
  const target: Target = { base: `http://127.0.0.1:${GUI_PORT}/`, token: guiToken() };
  const app = await page.request.get(new URL('api/app', target.base).href, {
    headers: { Authorization: `Bearer ${target.token}` },
  });
  const variables = ((await app.json()) as { variables?: { available: boolean } }).variables;
  test.skip(!variables?.available, 'the launcher has no control server');

  await stopRun(page, target, RUN_TIMEOUT);
  await startRun(page, target, 'forever.yaml');
  try {
    await openVariables(page, target);
    // 例の ECU が宣言した変数（Engine の 2 つ、Dashboard の 1 つ）と、ECU の実行ファイルが自分で出す変数（peer.steps など）
    await expect(page.getByTestId('variables-count')).toHaveText(/^\s*\d+ 個の変数\s*$/, {
      timeout: RUN_TIMEOUT,
    });
    for (const [peer, name] of [
      ['Engine', 'engine.throttle'],
      ['Engine', 'engine.speed'],
      ['Dashboard', 'dashboard.engine_speed'],
    ]) {
      await expect(widget(page, peer, name)).toBeAttached();
    }
    await expect(
      page.getByRole('heading', { level: 2, name: 'Engine', exact: true }),
    ).toBeVisible();
    await expect(
      page.getByRole('heading', { level: 2, name: 'Dashboard', exact: true }),
    ).toBeVisible();
    // ECU が自分で足す監視の変数（peer.*、j1939.*）は、畳んだまとまり。絞り込みで当たれば開く
    await expect(
      page.getByTestId('dashboard-toggle').filter({ hasText: 'Engine の監視' }),
    ).toHaveAttribute('aria-expanded', 'false');
    await expect(widget(page, 'Engine', 'peer.steps')).toHaveCount(0);
    await page.getByTestId('variables-filter').fill('peer.steps');
    await expect(widget(page, 'Engine', 'peer.steps')).toBeAttached();
    await page.getByTestId('variables-filter').fill('');
    await expect(widget(page, 'Engine', 'peer.steps')).toHaveCount(0);
    // 例の Engine: 回転数は 800 + スロットル × 20 に近づく（スロットルの最初は 20 % なので 1200 rpm）
    await expect
      .poll(() => numberOf(page, 'Engine', 'engine.speed'), { timeout: RUN_TIMEOUT })
      .toBe(1200);
    const throttle = widget(page, 'Engine', 'engine.throttle');
    await throttle.getByRole('slider', { name: 'engine.throttle' }).focus();
    await page.keyboard.press('End');
    await expect(throttle.getByTestId('widget-write')).toHaveText(/反映しました/, {
      timeout: RUN_TIMEOUT,
    });
    await expect
      .poll(() => numberOf(page, 'Engine', 'engine.speed'), { timeout: RUN_TIMEOUT })
      .toBe(2800);
    // Dashboard は J1939 で受けた回転数を出す
    await expect
      .poll(() => numberOf(page, 'Dashboard', 'dashboard.engine_speed'), { timeout: RUN_TIMEOUT })
      .toBe(2800);

    const directory = process.env['VEHICLE_SIL_GUI_SHOTS'];
    if (directory) {
      for (const size of [
        { width: 360, height: 780 },
        { width: 1280, height: 800 },
      ]) {
        await page.setViewportSize(size);
        await page.locator('mat-sidenav-content').evaluate((element) => element.scrollTo(0, 0));
        await widget(page, 'Engine', 'engine.throttle').scrollIntoViewIfNeeded();
        await page.waitForTimeout(800);
        await page.screenshot({ path: `${directory}/variables-${size.width}.png` });
      }
    }
  } finally {
    await stopRun(page, target, RUN_TIMEOUT);
  }
});
