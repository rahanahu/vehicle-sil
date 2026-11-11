// 実行の画面を、本物の GUI サーバーと偽のランチャー（gui/server/tests/fake_launcher.py）で通す。
// 構成ファイルは gui/server/tests/configs/ で、ファイルごとに偽のランチャーの筋書きが決まっている。
// サーバーは 1 つのセッションしか動かさないので、試験は順に動き、動かしたものは止めてから終わる。
import { Page, expect, test } from '@playwright/test';
import { guiToken } from './support/gui-server';
import { VIEWPORTS, findLayoutProblems } from './support/layout';

/** ランチャーの 1 回の実行の上限（偽のランチャーは 1 ステップ 50 ms、stop_at_ms まで 10 ステップ）。 */
const RUN_TIMEOUT = 20_000;

async function openRun(page: Page): Promise<void> {
  await page.goto(`/#token=${guiToken()}`);
  await expect(page.getByRole('heading', { level: 1, name: '実行' })).toBeVisible();
  await expect(page).toHaveTitle('実行 - vehicle-sil');
}

/** 選んでいる構成ファイル（mat-select の表示。選択肢の一覧も mat-select の中に描かれるので、表示の部分だけ）。 */
function chosenFile(page: Page) {
  return page.getByRole('combobox', { name: '構成ファイル' }).locator('.mat-mdc-select-value-text');
}

/** 構成ファイルを選び、その照合の結果が出るまで待つ。 */
async function choose(page: Page, file: string): Promise<void> {
  const select = page.getByRole('combobox', { name: '構成ファイル' });
  await expect(select).toBeEnabled();
  await select.click();
  await page.getByRole('option', { name: file, exact: true }).click();
  await expect(chosenFile(page)).toHaveText(file);
  await expect(page.getByTestId('check-outcome')).toBeVisible();
  await expect(page.getByRole('button', { name: '照合し直す' })).toBeEnabled();
}

/** 始めて、サーバーが付けたセッションの番号を返す。 */
async function start(page: Page): Promise<number> {
  const response = page.waitForResponse((r) => r.url().endsWith('/api/run'));
  await page.getByTestId('start').click();
  const answer = await response;
  expect(answer.status()).toBe(200);
  return ((await answer.json()) as { session: { id: number } }).session.id;
}

function result(page: Page, session: number) {
  return page.locator(`[data-testid="result-card"][data-session="${session}"]`);
}

/** 前の試験のセッションが動いていたら止める。 */
async function stopIfRunning(page: Page): Promise<void> {
  const stop = page.getByTestId('stop');
  for (let i = 0; i < 2 && (await stop.isVisible()); i++) {
    await stop.click();
    await page.waitForTimeout(500);
  }
  await expect(page.getByTestId('start')).toBeVisible({ timeout: RUN_TIMEOUT });
}

test.beforeEach(async ({ page }) => {
  await openRun(page);
  await stopIfRunning(page);
});

test('a run that passes: phases, participants, logs and the result', async ({ page }) => {
  await choose(page, 'simulation.yaml');
  await expect(page.getByTestId('check-outcome')).toContainText('通った');
  for (const name of ['Engine', 'Dashboard', 'Logger']) {
    await expect(page.getByTestId(`check-participant-${name}`)).toBeVisible();
  }
  const session = await start(page);
  await expect(result(page, session)).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(result(page, session).getByTestId('result')).toContainText('成功');
  await expect(
    page.getByRole('list', { name: '段階' }).locator('[aria-current="step"]'),
  ).toHaveText(/終了/);
  await expect(page.getByTestId('status-Engine')).toContainText('終了した');
  await expect(page.getByTestId('sim-time')).toHaveText(/^\d+\.\d{3} s$/);
  await expect(page.getByTestId('log-line').first()).toBeVisible();
  await expect(page.getByTestId('start')).toHaveText(/もう一度実行/);
});

test('a check with an error: the findings, and no start', async ({ page }) => {
  await choose(page, 'check-error.yaml');
  await expect(page.getByTestId('check-outcome')).toContainText('誤りがある');
  await expect(page.getByTestId('check-card').getByTestId('finding').first()).toContainText(
    'step-mismatch',
  );
  await expect(page.getByTestId('start')).toBeDisabled();
  await expect(page.getByTestId('blocked')).toContainText('直してから始めてください');
});

test('a participant switched off is checked again and left out of the run', async ({ page }) => {
  await choose(page, 'simulation.yaml');
  const recheck = page.waitForRequest(
    (r) => r.url().endsWith('/api/check') && r.postDataJSON().disable.includes('Dashboard'),
  );
  await page.getByRole('switch', { name: 'Dashboard を使う' }).click();
  await recheck;
  await expect(page.getByTestId('check-participant-Dashboard')).toContainText('外した');
  await expect(page.getByRole('switch', { name: 'Dashboard を使う' })).not.toBeChecked();

  const run = page.waitForRequest((r) => r.url().endsWith('/api/run'));
  const session = await start(page);
  expect((await run).postDataJSON()).toMatchObject({
    file: 'simulation.yaml',
    disable: ['Dashboard'],
  });
  await expect(result(page, session)).toContainText('成功', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('status-Dashboard')).toContainText('外した');
});

test('stopping a run that does not end by itself', async ({ page }) => {
  await choose(page, 'forever.yaml');
  const session = await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });
  await page.getByTestId('stop').click();
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
});

test('a run that does not stop at once: stopping, then stopping without waiting', async ({
  page,
}) => {
  await choose(page, 'ignore-stop.yaml');
  const session = await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });
  await page.getByTestId('stop').click();
  await expect(page.getByTestId('stopping')).toBeVisible();
  await expect(page.getByTestId('stop')).toHaveText(/待たずに止める/);
  await page.getByTestId('stop').click();
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('status-Engine')).toContainText('強制終了した');
});

test('a participant that fails: the result, the reason and its exit code', async ({ page }) => {
  await choose(page, 'participant-crash.yaml');
  const session = await start(page);
  await expect(result(page, session)).toContainText('失敗', { timeout: RUN_TIMEOUT });
  await expect(result(page, session).getByTestId('finding')).toContainText('participant-failed');
  await expect(page.getByTestId('status-Dashboard')).toContainText('終了コード 3');
});

test('a launcher that dies without a result: what the server noticed', async ({ page }) => {
  await choose(page, 'launcher-crash.yaml');
  await start(page);
  await expect(page.getByTestId('problems-card')).toContainText('no-result', {
    timeout: RUN_TIMEOUT,
  });
  await expect(page.getByTestId('start')).toBeVisible();
});

test('a reload and a second tab show the same run', async ({ page, context }) => {
  await choose(page, 'forever.yaml');
  await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });

  // 再読み込み: トークンは sessionStorage に残り、URL には残らない
  await page.reload();
  expect(page.url()).not.toContain('token');
  await expect(page.getByTestId('session-file')).toHaveText('forever.yaml');
  await expect(page.getByTestId('stop')).toBeVisible();
  await expect(page.getByTestId('status-Engine')).toContainText('参加した');
  await expect(page.getByTestId('log-line').first()).toBeVisible();

  // 別のタブ（サーバーの URL から開く）
  const other = await context.newPage();
  await openRun(other);
  await expect(other.getByTestId('session-file')).toHaveText('forever.yaml');
  await expect(other.getByTestId('log-line').first()).toBeVisible();
  // 別のタブで止めても、こちらに届く
  await other.getByTestId('stop').click();
  await expect(page.getByTestId('result')).toContainText('中断', { timeout: RUN_TIMEOUT });
  await other.close();
});

test('a dropped connection is shown and joined again', async ({ page }) => {
  // WebSocket を間に入れて、切る。切った後しばらくは、つなぎ直しも断る
  let refuse = false;
  let drop: (() => Promise<void>) | undefined;
  await page.routeWebSocket(/\/ws$/, (ws) => {
    if (refuse) {
      void ws.close();
      return;
    }
    ws.connectToServer();
    drop = () => ws.close();
  });
  // 前の試験で開いた接続は間を通らないので、開き直す（トークンは sessionStorage にある）
  await page.reload();
  await expect(page.getByTestId('connection')).toBeHidden();
  expect(drop).toBeDefined();
  refuse = true;
  await drop?.();
  await expect(page.getByTestId('connection')).toContainText(/接続が切れました|つないでいます/);
  refuse = false;
  await expect(page.getByTestId('connection')).toBeHidden({ timeout: 10_000 });
});

test('the screen asks for the server URL without a token', async ({ browser }) => {
  const page = await browser.newPage();
  await page.goto('/');
  await expect(page.getByTestId('no-token')).toContainText('#token=');
  await page.close();
});

test('everything can be done with the keyboard', async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 900 });
  const select = page.getByRole('combobox', { name: '構成ファイル' });
  await select.focus();
  // 開いて、名前の先頭を打って選ぶ
  await page.keyboard.press('Enter');
  await expect(page.getByRole('listbox')).toBeVisible();
  await page.keyboard.type('simulation');
  await expect(page.getByRole('option', { name: 'simulation.yaml', exact: true })).toHaveClass(
    /mat-mdc-option-active/,
  );
  await page.keyboard.press('Enter');
  await expect(chosenFile(page)).toHaveText('simulation.yaml');
  await expect(page.getByTestId('check-outcome')).toContainText('通った');

  const toggle = page.getByRole('switch', { name: 'Logger を使う' });
  await toggle.focus();
  await page.keyboard.press('Space');
  await expect(toggle).not.toBeChecked();
  await expect(page.getByTestId('check-participant-Logger')).toContainText('外した');

  const startButton = page.getByTestId('start');
  await startButton.focus();
  const response = page.waitForResponse((r) => r.url().endsWith('/api/run'));
  await page.keyboard.press('Enter');
  const session = ((await (await response).json()) as { session: { id: number } }).session.id;
  await expect(result(page, session)).toContainText('成功', { timeout: RUN_TIMEOUT });

  // ログは Tab で届き、矢印で動かせる
  const log = page.getByTestId('log');
  await log.focus();
  await expect(log).toBeFocused();
});

test.describe('the screen at every width', () => {
  /** 3 つの状態: ファイルを選んで所見がある、実行中でログが流れる、失敗で終わった。 */
  const STATES = [
    { name: 'chosen', file: 'check-error.yaml', run: false },
    { name: 'running', file: 'forever.yaml', run: true },
    { name: 'failed', file: 'participant-crash.yaml', run: true },
  ] as const;
  const SCREENSHOT_WIDTHS = [360, 1024, 1920];

  for (const state of STATES) {
    test(`${state.name}: no overflow or overlap from 360 px to full screen`, async ({
      page,
    }, testInfo) => {
      await choose(page, state.file);
      if (state.run) {
        const session = await start(page);
        if (state.name === 'failed') {
          await expect(result(page, session)).toBeVisible({ timeout: RUN_TIMEOUT });
        } else {
          await expect(page.getByTestId('log-line').nth(5)).toBeVisible({ timeout: RUN_TIMEOUT });
        }
      }
      for (const viewport of VIEWPORTS) {
        await page.setViewportSize({ width: viewport.width, height: viewport.height });
        await expect(page.getByTestId('nav')).toHaveAttribute('data-nav-mode', viewport.navMode);
        await expect
          .poll(() => findLayoutProblems(page), { message: `${viewport.width} px` })
          .toEqual([]);
        if (SCREENSHOT_WIDTHS.includes(viewport.width)) {
          // 中身は main の外側（mat-sidenav-content）でスクロールするので、窓を中身の高さまで伸ばして撮る
          const height = await page.evaluate(() => {
            document.querySelector('mat-sidenav-content')?.scrollTo(0, 0);
            return (document.querySelector('main')?.scrollHeight ?? 0) + 64;
          });
          await page.setViewportSize({
            width: viewport.width,
            height: Math.max(viewport.height, height),
          });
          // ナビゲーションの動き（幅の区分の切り替え）が終わってから撮る
          await page.waitForTimeout(500);
          await page.screenshot({
            path: testInfo.outputPath(`run-${state.name}-${viewport.width}.png`),
          });
          await page.setViewportSize({ width: viewport.width, height: viewport.height });
        }
      }
      await stopIfRunning(page);
    });
  }
});
