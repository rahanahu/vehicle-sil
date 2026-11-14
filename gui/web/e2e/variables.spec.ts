// 変数の画面（自動のダッシュボード）を、流し直すランチャーと偽の Control Server で通す。流し直すランチャーに
// Control Server を持たせ、部品の種類が揃う変数（REPLAY_VARIABLES=more）を出させるので、この試験だけの
// GUI サーバーを起動する。
import { expect, test } from '@playwright/test';
import {
  CONFIGS,
  REPLAY_LAUNCHER,
  GUI_PORT,
  GuiServer,
  launchGuiServer,
} from './support/gui-server';
import { VIEWPORTS, findLayoutProblems } from './support/layout';
import {
  Target,
  numberOf,
  openVariables,
  startRun,
  stopRun,
  timeCommand,
  widget,
  widgetValue,
} from './support/variables-page';

const CONTROL_FEATURES =
  'events,stdin-commands,check-variables,speed,start-paused,time-commands,control-links,control-server';

let server: GuiServer;
let target: Target;

test.beforeAll(async () => {
  // 開発サーバーの形（e2e:dev）では回さない（同じ画面。サーバーを別に起動するので、ビルドした画面で 1 回）
  test.skip(test.info().project.name !== 'static', 'only with the built screen');
  server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    env: {
      REPLAY_TICK_MS: '50',
      REPLAY_FEATURES: CONTROL_FEATURES,
      REPLAY_VARIABLES: 'more',
    },
    port: GUI_PORT + 300,
  });
  target = { base: new URL('/', server.url).href, token: server.token };
});

test.afterAll(async () => {
  await server?.stop();
});

test.afterEach(async ({ page }) => {
  if (target) await stopRun(page, target);
});

test('shows the values, and the switch and the slider write', async ({ page }) => {
  await openVariables(page, target);
  await expect(page.getByTestId('variables-no-run')).toBeVisible();
  await startRun(page, target, 'forever.yaml');
  // ECU ごとのまとまりに、既定の部品が並ぶ
  await expect(page.getByTestId('dashboard-section')).toHaveText(['Engine', 'Dashboard']);
  // 6 つずつの変数と、見出しの「バスにつなぐ」（どの ECU にもある ecu.connected）
  await expect(page.getByTestId('variables-count')).toHaveText('14 個の変数');
  const speed = widget(page, 'Engine', 'engine.speed');
  await expect(speed.getByTestId('widget-graph')).toBeVisible();
  await expect(speed.getByTestId('widget-condition')).toHaveText(/s の値\s*$/);
  await expect(widgetValue(page, 'Engine', 'engine.warning')).toHaveText('オフ');

  // スイッチ: 押すと書き、反映されたと出る
  const enabled = widget(page, 'Engine', 'engine.enabled');
  const toggle = enabled.getByRole('switch', { name: 'engine.enabled' });
  await expect(toggle).toHaveAttribute('aria-checked', 'false');
  await toggle.click();
  await expect(enabled.getByTestId('widget-write')).toHaveText(/反映しました/);
  await expect(toggle).toHaveAttribute('aria-checked', 'true');

  // スライダー: キーボードで端へ動かすと書き、ランプが点き、回転数が上がる（speed = demand × 10 + 時刻）
  const demand = widget(page, 'Engine', 'engine.demand');
  const before = await numberOf(page, 'Engine', 'engine.speed');
  await demand.getByRole('slider', { name: 'engine.demand' }).focus();
  await page.keyboard.press('End');
  await expect(demand.getByTestId('widget-write')).toHaveText(/反映しました/);
  await expect(widgetValue(page, 'Engine', 'engine.demand')).toHaveText('100.00 %');
  await expect(widgetValue(page, 'Engine', 'engine.warning')).toHaveText('オン');
  await expect.poll(() => numberOf(page, 'Engine', 'engine.speed')).toBeGreaterThan(before + 1000);

  // バスにつなぐ: 見出しのスイッチで外すと、外れていることを印と文で出す。つなぎ直すと消える
  const bus = page.locator('[data-testid="bus-connection"][data-variable="Engine/ecu.connected"]');
  const busToggle = bus.getByRole('switch', { name: 'Engine をバスにつなぐ' });
  await expect(busToggle).toHaveAttribute('aria-checked', 'true');
  await busToggle.click();
  await expect(bus).toHaveAttribute('data-connected', 'false');
  await expect(bus.getByTestId('bus-state')).toContainText('この ECU はバスから外れています');
  await busToggle.click();
  await expect(bus).toHaveAttribute('data-connected', 'true');
  await expect(bus.getByTestId('bus-state')).toHaveText('');

  // 部品が並んだ画面も、狭い窓から広い窓まで崩れない
  for (const viewport of VIEWPORTS.filter((v) => [360, 1024, 1920].includes(v.width))) {
    await page.setViewportSize({ width: viewport.width, height: viewport.height });
    await expect(page.getByTestId('nav')).toHaveAttribute('data-nav-mode', viewport.navMode);
    // 窓を変えた直後は、ナビゲーションの幅が動いている途中なので、落ち着くまで待つ
    await expect.poll(() => findLayoutProblems(page)).toEqual([]);
  }

  // 絞り込み
  await page.getByTestId('variables-filter').fill('warning');
  await expect(page.getByTestId('variables-count')).toHaveText('14 個中 2 個');
  await expect(page.locator('[data-variable]')).toHaveCount(2);
  await page.getByTestId('variables-filter').fill('dashboard warn');
  await expect(page.locator('[data-variable]')).toHaveCount(1);
});

test('started paused: a parameter can be written before the first step, and a write without an answer is shown, not sent again', async ({
  page,
}) => {
  await startRun(page, target, 'forever.yaml', true);
  await openVariables(page, target);
  await expect(page.getByTestId('variables-notice')).toContainText(
    '最初のステップの前で止まっています',
  );
  // パラメーターの選択肢: 書ける。止まっている間は受け付けたまま、再開すると時刻 0 のステップで反映される
  const mode = widget(page, 'Engine', 'engine.mode');
  // 閉じたままのキーでは書かない（Tab で渡る途中や、スクロールのつもりのキー）
  const written: string[] = [];
  page.on('request', (r) => {
    if (r.url().endsWith('/api/write')) written.push(r.postData() ?? '');
  });
  await mode.getByRole('combobox', { name: 'engine.mode' }).focus();
  await page.keyboard.press('ArrowDown');
  await page.keyboard.press('ArrowDown');
  await expect(mode.getByTestId('widget-write')).toHaveText('');
  expect(written).toEqual([]);
  await mode.getByRole('combobox', { name: 'engine.mode' }).click();
  await page.getByRole('option', { name: 'on', exact: true }).click();
  await expect(mode.getByTestId('widget-write')).toHaveText(/受け付けました/);

  // 書き込みに答えが来ない: 「答えが来なかった」と出し、送り直さない
  let writes = 0;
  await page.route('**/api/write', async (route) => {
    writes += 1;
    await route.fulfill({
      status: 504,
      contentType: 'application/json',
      body: JSON.stringify({
        error: {
          code: 'write-timeout',
          message: 'the control server did not answer the write in time',
          detail: null,
        },
      }),
    });
  });
  const enabled = widget(page, 'Engine', 'engine.enabled');
  await enabled.getByRole('switch', { name: 'engine.enabled' }).click();
  await expect(enabled.getByTestId('widget-error')).toContainText('答えが来ませんでした');
  await expect(enabled.getByRole('switch')).toHaveAttribute('aria-checked', 'false');
  await page.unroute('**/api/write');

  await timeCommand(page, target, 'resume');
  await expect(mode.getByTestId('widget-write')).toHaveText(/0\.000 s のステップで反映しました/);
  await expect(mode.getByRole('combobox')).toContainText('on');
  // 再開の後は、パラメーターは書けない（理由を出す）
  await expect(mode.getByTestId('widget-why')).toHaveText(
    'パラメーターは、止めて始めた実行の最初のステップの前だけ書けます',
  );
  expect(writes).toBe(1);
});

test.describe('in English', () => {
  test.use({ locale: 'en-US' });

  test('the variables page has no Japanese left, with and without a run', async ({ page }) => {
    await page.goto(`${target.base}variables#token=${target.token}`);
    await expect(page.getByRole('heading', { level: 1, name: 'Variables' })).toBeVisible();
    await expect(page).toHaveTitle('Variables - vehicle-sil');
    const content = page.locator('app-variables-page');
    await expect(page.getByTestId('variables-no-run')).toBeVisible();
    expect(await content.innerText()).not.toMatch(/[぀-ヿ一-鿿]/);

    await startRun(page, target, 'forever.yaml');
    await expect(page.getByTestId('dashboard-section')).toHaveText(['Engine', 'Dashboard']);
    await expect(widgetValue(page, 'Engine', 'engine.speed')).not.toHaveText('');
    // 変数の名前、型、値はランチャーと ECU のもの（英字）
    expect(await content.innerText()).not.toMatch(/[぀-ヿ一-鿿]/);
  });
});
