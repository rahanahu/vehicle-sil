// ダッシュボードのファイルと持ち込みのパネルを、本物のランチャー（Control Server つき）と例の ECU で通す。構成
// ファイル example.yaml の隣に、examples/ecu_main の simulation.dashboard.yaml と panels/rpm-bar.js の写しがある
// （gui/server/tests/real_configs.py）。環境変数 VEHICLE_SIL_GUI_SHOTS にディレクトリを書くと、幅 1280 で撮る。
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

test('the example dashboard file: its layout, and its panel shows the engine speed', async ({
  page,
}) => {
  const target: Target = { base: `http://127.0.0.1:${GUI_PORT}/`, token: guiToken() };
  const app = await page.request.get(new URL('api/app', target.base).href, {
    headers: { Authorization: `Bearer ${target.token}` },
  });
  const variables = ((await app.json()) as { variables?: { available: boolean } }).variables;
  test.skip(!variables?.available, 'the launcher has no control server');

  await stopRun(page, target, RUN_TIMEOUT);
  await startRun(page, target, 'example.yaml');
  try {
    await openVariables(page, target);
    await expect(page.getByTestId('dashboard-file')).toContainText(
      'example.dashboard.yaml のとおりに並べています',
      { timeout: RUN_TIMEOUT },
    );
    await expect(page.getByTestId('dashboard-title')).toHaveText('Engine and dashboard');
    // 読み込んだパネルのモジュールの名前が見える（パネルは画面の中でトークンを持って動くため）
    await expect(page.getByTestId('dashboard-modules')).toContainText('panels/rpm-bar.js');
    // まとまりは、ファイルの順に、auto が足した監視のまとまり（畳んである）を挟んで並ぶ
    await expect(page.getByTestId('dashboard-section')).toHaveText(
      [/^\s*Engine\s*$/, /Engine の監視/, /^\s*Dashboard\s*$/, /Dashboard の監視/],
      { timeout: RUN_TIMEOUT },
    );
    // ファイルが決めた部品と見出し
    const throttle = widget(page, 'Engine', 'engine.throttle');
    await expect(throttle.getByRole('slider', { name: 'Throttle' })).toBeAttached();
    await expect(throttle).toContainText('Throttle');
    await expect(widget(page, 'Engine', 'engine.speed')).toContainText('Engine speed');
    await expect(widget(page, 'Engine', 'engine.speed').getByTestId('widget-graph')).toBeAttached();
    await expect(widget(page, 'Dashboard', 'dashboard.engine_speed')).toContainText(
      'Speed on the dashboard',
    );
    // 持ち込みのパネル: 渡された client で回転数を watch して出す（スロットル 20 % で 1200 rpm）
    const panel = page.locator('[data-panel="rpm-bar"]');
    await expect(panel).toContainText('Engine speed (panel)');
    await expect(panel.getByTestId('panel-problem')).toHaveCount(0);
    await expect(panel.getByTestId('rpm-bar-value')).toHaveText('1200 / 4000', {
      timeout: RUN_TIMEOUT,
    });
    await expect
      .poll(() => numberOf(page, 'Engine', 'engine.speed'), { timeout: RUN_TIMEOUT })
      .toBe(1200);

    const directory = process.env['VEHICLE_SIL_GUI_SHOTS'];
    if (directory) {
      await page.setViewportSize({ width: 1280, height: 900 });
      await page.locator('mat-sidenav-content').evaluate((element) => element.scrollTo(0, 0));
      await page.waitForTimeout(800);
      await page.screenshot({ path: `${directory}/dashboard-1280.png` });
    }
  } finally {
    await stopRun(page, target, RUN_TIMEOUT);
  }
});
