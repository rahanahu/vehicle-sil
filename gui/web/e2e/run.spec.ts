// 実行の画面を、本物の GUI サーバーと偽のランチャー（gui/server/tests/fake_launcher.py）で通す。
// 構成ファイルは gui/server/tests/configs/ で、ファイルごとに偽のランチャーの筋書きが決まっている。
// サーバーは 1 つのセッションしか動かさないので、試験は順に動き、動かしたものは止めてから終わる。
import { Page, WebSocketRoute, expect, test } from '@playwright/test';
import { guiToken } from './support/gui-server';
import { VIEWPORTS, findLayoutProblems } from './support/layout';

/** ランチャーの 1 回の実行の上限（偽のランチャーは 1 ステップ 50 ms、stop_at_ms まで 10 ステップ）。 */
const RUN_TIMEOUT = 20_000;

async function openRun(page: Page): Promise<void> {
  await page.goto(`/#token=${guiToken()}`);
  await expect(page.getByRole('heading', { level: 1, name: '実行' })).toBeVisible();
  await expect(page).toHaveTitle('実行 - vehicle-sil');
}

/** 状態の帯の主なボタン（開始 / 停止 / 止めています / もう一度実行。1 つの要素のまま変わる）。 */
function primary(page: Page) {
  return page.getByTestId('primary');
}

/** 選んでいる構成ファイル（mat-select の表示。選択肢の一覧も mat-select の中に描かれるので、表示の部分だけ）。 */
function chosenFile(page: Page) {
  return page.getByRole('combobox', { name: '構成ファイル' }).locator('.mat-mdc-select-value-text');
}

/** 構成ファイルを選び、その照合の結果が出るまで待つ。 */
async function choose(page: Page, file: string): Promise<void> {
  const select = page.getByRole('combobox', { name: '構成ファイル' });
  await expect(select).toBeEnabled();
  if ((await chosenFile(page).textContent())?.trim() !== file) {
    const checked = page.waitForResponse(
      (r) => r.url().endsWith('/api/check') && r.request().postDataJSON().file === file,
    );
    await select.click();
    await page.getByRole('option', { name: file, exact: true }).click();
    await checked;
  }
  await expect(chosenFile(page)).toHaveText(file);
  await expect(page.getByTestId('check-outcome')).toBeVisible();
  await expect(page.getByRole('button', { name: '照合し直す' })).toBeEnabled();
}

/** 始めて、サーバーが付けたセッションの番号を返す。 */
async function start(page: Page): Promise<number> {
  await expect(primary(page)).not.toHaveAttribute('aria-disabled', 'true');
  const response = page.waitForResponse((r) => r.url().endsWith('/api/run'));
  await primary(page).click();
  const answer = await response;
  expect(answer.status()).toBe(200);
  return ((await answer.json()) as { session: { id: number } }).session.id;
}

function result(page: Page, session: number) {
  return page.locator(`[data-testid="result-card"][data-session="${session}"]`);
}

/** 前の試験のセッションが動いていたら止める（順に止め、止まらなければ待たずに止める）。 */
async function stopIfRunning(page: Page): Promise<void> {
  await expect(primary(page)).toBeVisible();
  // つないで揃うまでは、帯が「開始」のままのことがある
  await expect(page.getByTestId('connection')).toHaveText('', { timeout: 10_000 });
  await expect(page.getByTestId('blocked').filter({ hasText: 'サーバーと揃うまで' })).toHaveCount(
    0,
  );
  if ((await primary(page).getAttribute('data-action')) === 'stop') {
    await primary(page).click();
  }
  const force = page.getByTestId('force-stop');
  await expect(primary(page)).toHaveAttribute('data-action', /start|again|stopping/);
  if ((await primary(page).getAttribute('data-action')) === 'stopping') {
    try {
      await expect(primary(page)).toHaveAttribute('data-action', /start|again/, { timeout: 3000 });
    } catch {
      await force.click();
      await page.getByTestId('force-stop-confirm').click();
    }
  }
  await expect(primary(page)).toHaveAttribute('data-action', /start|again/, {
    timeout: RUN_TIMEOUT,
  });
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
  await expect(page.getByTestId('outcome')).toContainText('成功');
  await expect(
    page.getByRole('list', { name: '段階' }).locator('[aria-current="step"]'),
  ).toHaveText(/終了/);
  await expect(page.getByTestId('status-Engine')).toContainText('終了した');
  await expect(page.getByTestId('sim-time')).toHaveText(/^\d+\.\d{3} s$/);
  await expect(page.getByTestId('log-line').first()).toBeVisible();
  await expect(primary(page)).toHaveText(/もう一度実行/);
});

test('a check with an error: the findings, and no start', async ({ page }) => {
  await choose(page, 'check-error.yaml');
  await expect(page.getByTestId('check-outcome')).toContainText('誤りがある');
  await expect(page.getByTestId('check-card').getByTestId('finding').first()).toContainText(
    'step-mismatch',
  );
  await expect(primary(page)).toHaveAttribute('aria-disabled', 'true');
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

test('quick switches are checked once, without a busy check', async ({ page }) => {
  // 前の実行で外した participant を引き継がないファイル
  await choose(page, 'forever.yaml');
  await expect(page.getByRole('switch', { name: 'Dashboard を使う' })).toBeChecked();
  const checks: unknown[] = [];
  page.on('request', (r) => {
    if (r.url().endsWith('/api/check')) checks.push(r.postDataJSON());
  });
  const failures: number[] = [];
  page.on('response', (r) => {
    if (r.url().endsWith('/api/check') && r.status() !== 200) failures.push(r.status());
  });
  await page.getByRole('switch', { name: 'Dashboard を使う' }).click();
  await page.getByRole('switch', { name: 'Logger を使う' }).click();
  await page.getByRole('switch', { name: 'Logger を使う' }).click();
  await expect(page.getByTestId('check-participant-Dashboard')).toContainText('外した');
  await expect(primary(page)).not.toHaveAttribute('aria-disabled', 'true');
  expect(checks).toEqual([{ file: 'forever.yaml', disable: ['Dashboard'] }]);
  expect(failures).toEqual([]);
});

test('stopping a run that does not end by itself', async ({ page }) => {
  await choose(page, 'forever.yaml');
  const session = await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });
  const stop = page.waitForRequest((r) => r.url().endsWith('/api/stop'));
  await primary(page).click();
  expect((await stop).postDataJSON()).toEqual({ force: false });
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
});

test('a double click on stop does not stop without waiting', async ({ page }) => {
  await choose(page, 'ignore-stop.yaml');
  const session = await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });
  const bodies: unknown[] = [];
  page.on('request', (r) => {
    if (r.url().endsWith('/api/stop')) bodies.push(r.postDataJSON());
  });
  await primary(page).dblclick();
  await expect(page.getByTestId('outcome')).toContainText('止めています');
  await expect(primary(page)).toHaveText(/止めています/);
  // しばらく待っても、待たずに止めてはいない
  await page.waitForTimeout(1500);
  expect(bodies.every((body) => (body as { force: boolean }).force === false)).toBe(true);
  await expect(page.getByTestId('status-Engine')).not.toContainText('強制終了');

  // 待たずに止める: 確かめの「やめる」では止めない
  const force = page.getByTestId('force-stop');
  await expect(force).toBeVisible();
  await force.click();
  await expect(page.getByRole('dialog')).toContainText('待たずに止めますか');
  await expect(page.getByRole('button', { name: 'やめる' })).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(page.getByRole('dialog')).toBeHidden();
  await expect(force).toBeFocused();
  expect(bodies.some((body) => (body as { force: boolean }).force)).toBe(false);

  await force.click();
  const forced = page.waitForRequest((r) => r.url().endsWith('/api/stop'));
  await page.getByTestId('force-stop-confirm').click();
  expect((await forced).postDataJSON()).toEqual({ force: true });
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('status-Engine')).toContainText('強制終了した');
});

test('a participant that fails: the result, the reason and its exit code', async ({ page }) => {
  await choose(page, 'participant-crash.yaml');
  const session = await start(page);
  await expect(result(page, session)).toContainText('失敗', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('outcome')).toContainText('失敗');
  await expect(page.getByTestId('first-error')).toContainText('participant-failed');
  await expect(result(page, session).getByTestId('finding')).toContainText('participant-failed');
  await expect(page.getByTestId('status-Dashboard')).toContainText('終了コード 3');
});

test('a launcher that dies without a result: an abnormal end, and where to look', async ({
  page,
}) => {
  await choose(page, 'launcher-crash.yaml');
  await start(page);
  await expect(page.getByTestId('outcome')).toContainText('異常終了（終了コード 70）', {
    timeout: RUN_TIMEOUT,
  });
  await expect(page.getByTestId('crash')).toContainText('終了コード 70');
  await expect(
    page.getByRole('list', { name: '段階' }).locator('[aria-current="step"]'),
  ).toHaveText(/終了/);
  await expect(page.getByRole('list', { name: '段階' })).toContainText('（中断）');
  await expect(page.getByTestId('problems-card')).toContainText('no-result');
  // ランチャーの標準エラーは、サーバーの知らせとは別のまとまりに
  await expect(page.getByTestId('stderr-last')).toContainText('dying on purpose');
  await expect(page.getByTestId('problems-card')).not.toContainText('launcher-stderr');
  // 終わりを知らせてこなかった participant は「参加した」のままにしない
  await expect(page.getByTestId('participants-unknown')).toBeVisible();
  for (const name of ['Engine', 'Dashboard']) {
    await expect(page.getByTestId(`status-${name}`)).toContainText('不明');
  }
  // 次に見るところ: ランチャーのログ
  await page.getByRole('button', { name: 'ランチャーのログを見る' }).click();
  await expect(page.getByRole('heading', { name: 'ログ' })).toBeFocused();
  await expect(page.getByTestId('log-source')).toContainText('vehicle_sil_launch');
  await expect(primary(page)).toHaveText(/もう一度実行/);
});

test('a launcher that runs on after its result can still be stopped', async ({ page }) => {
  // 偽のランチャーの筋書き result-then-wait: 結果を書いた後も、止めるよう頼まれるまで終わらない
  await choose(page, 'result-then-wait.yaml');
  const session = await start(page);
  await expect(result(page, session)).toContainText('成功', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('outcome')).toContainText('ランチャーが終わるのを待っています');
  await expect(page.getByTestId('finishing')).toBeVisible();
  await expect(primary(page)).toHaveAttribute('data-action', 'stop');
  await expect(primary(page)).toHaveText(/停止/);
  // しばらく待っても、ランチャーは終わらず、停止を出したまま
  await page.waitForTimeout(1000);
  await expect(primary(page)).toHaveAttribute('data-action', 'stop');
  const stop = page.waitForRequest((r) => r.url().endsWith('/api/stop'));
  await primary(page).click();
  expect((await stop).postDataJSON()).toEqual({ force: false });
  await expect(primary(page)).toHaveText(/もう一度実行/, { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('outcome')).toContainText('成功');
  await expect(page.getByTestId('finishing')).toHaveCount(0);
});

test('a reload and a second tab show the same run', async ({ page, context }) => {
  await choose(page, 'forever.yaml');
  await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });

  // 再読み込み: トークンは sessionStorage に残り、URL には残らない
  await page.reload();
  expect(page.url()).not.toContain('token');
  await expect(page.getByTestId('session-file')).toHaveText('forever.yaml');
  await expect(primary(page)).toHaveText(/停止/);
  await expect(page.getByTestId('status-Engine')).toContainText('参加した');
  await expect(page.getByTestId('log-line').first()).toBeVisible();

  // 別のタブ（サーバーの URL から開く）
  const other = await context.newPage();
  await openRun(other);
  await expect(other.getByTestId('session-file')).toHaveText('forever.yaml');
  await expect(other.getByTestId('log-line').first()).toBeVisible();
  // 別のタブで止めても、こちらに届く
  await primary(other).click();
  await expect(page.getByTestId('result')).toContainText('中断', { timeout: RUN_TIMEOUT });
  await other.close();
});

test('a dropped connection is shown, joined again, and the log goes on without a gap', async ({
  page,
}) => {
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
  await expect(page.getByTestId('connection')).toHaveText('');
  await choose(page, 'forever.yaml');
  const session = await start(page);
  await expect(page.getByTestId('log-line').nth(3)).toBeVisible({ timeout: RUN_TIMEOUT });
  expect(drop).toBeDefined();
  refuse = true;
  await drop?.();
  await expect(page.getByTestId('connection')).toContainText(/接続が切れました|つないでいます/);
  // 切れている間は止められない（古い表示で操作しない）
  await expect(primary(page)).toHaveAttribute('aria-disabled', 'true');
  await expect(page.getByTestId('blocked')).toContainText('サーバーと揃うまで');
  await page.waitForTimeout(1000);
  refuse = false;
  await expect(page.getByTestId('connection')).toHaveText('', { timeout: 10_000 });
  await expect(primary(page)).not.toHaveAttribute('aria-disabled', 'true');
  await primary(page).click();
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });

  // Engine の行は「step 1」から「step N」まで、重ならず抜けずに N 行
  await page.getByTestId('log-source').click();
  await page.getByRole('option', { name: 'Engine', exact: true }).click();
  await expect(page.getByTestId('log-count')).toContainText('全');
  // 絞った行の数と、最後の行の step の番号が同じ（重なりも抜けもない）
  await expect
    .poll(async () => {
      const count = Number((await page.getByTestId('log-count').textContent())?.match(/\d+/)?.[0]);
      const last = await page.getByTestId('log-line').last().textContent();
      return Number(last?.match(/step (\d+)/)?.[1]) - count;
    })
    .toBe(0);
  await expect(page.getByTestId('log-gap')).toHaveCount(0);
});

test('a flood of lines keeps the lines of quiet participants, and counts what was dropped', async ({
  page,
}) => {
  await choose(page, 'flood.yaml');
  const session = await start(page);
  await expect(result(page, session)).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('log-dropped')).toContainText('Engine');
  await page.getByTestId('log-source').click();
  await page.getByRole('option', { name: 'Dashboard', exact: true }).click();
  await page.getByTestId('log').evaluate((element) => (element.scrollTop = 0));
  await expect(page.getByTestId('log-line').first()).toContainText('step 1:');
  await expect(page.getByTestId('log-dropped')).toBeHidden();
});

test('a log read while lines keep coming does not move', async ({ page }) => {
  // 行を試験から足すために、WebSocket を間に入れる
  let route: WebSocketRoute | undefined;
  await page.routeWebSocket(/\/ws$/, (ws) => {
    ws.connectToServer();
    route = ws;
  });
  await page.reload();
  await choose(page, 'forever.yaml');
  const session = await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });
  let seq = 1_000_000;
  const send = (count: number) => {
    for (let i = 0; i < count; i++, seq++) {
      route?.send(
        JSON.stringify({
          type: 'event',
          session,
          event: { event: 'log', seq, t_ms: 1000, source: 'Engine', line: `injected ${seq}` },
        }),
      );
    }
  };
  send(6000);
  const log = page.getByTestId('log');
  await expect(page.getByTestId('log-dropped')).toBeVisible();
  // 途中まで戻して読む（追従をやめる）
  await log.evaluate((element) => (element.scrollTop = element.scrollHeight / 2));
  await log.evaluate((element) => element.dispatchEvent(new Event('scroll')));
  await expect(page.getByTestId('log-follow')).not.toHaveClass(/checked/);
  const firstVisible = () =>
    log.evaluate((element) => {
      const top = element.getBoundingClientRect().top;
      const lines = [...element.querySelectorAll<HTMLElement>('[data-testid="log-line"]')];
      return lines.find((line) => line.getBoundingClientRect().bottom > top + 1)?.textContent;
    });
  const before = await firstVisible();
  send(300);
  await expect(page.getByTestId('log-to-end')).toBeVisible();
  await page.waitForTimeout(300);
  expect(await firstVisible()).toBe(before);
  // 末尾へ戻ると、また追従する
  await page.getByTestId('log-to-end').click();
  await expect(page.getByTestId('log-follow')).toHaveClass(/checked/);
  await expect(page.getByTestId('log-line').last()).toContainText(`injected ${seq - 1}`);
  await stopIfRunning(page);
});

test('the screen asks for the server URL without a token', async ({ browser }) => {
  const page = await browser.newPage();
  await page.goto('/');
  await expect(page.getByTestId('no-token')).toContainText('#token=');
  await page.close();
});

test('everything can be done with the keyboard, and the focus stays on the button', async ({
  page,
}) => {
  await page.setViewportSize({ width: 1280, height: 900 });
  const select = page.getByRole('combobox', { name: '構成ファイル' });
  await select.focus();
  // 開いて、名前の先頭を打って選ぶ
  await page.keyboard.press('Enter');
  await expect(page.getByRole('listbox')).toBeVisible();
  await page.keyboard.type('forever');
  await expect(page.getByRole('option', { name: 'forever.yaml', exact: true })).toHaveClass(
    /mat-mdc-option-active/,
  );
  await page.keyboard.press('Enter');
  await expect(chosenFile(page)).toHaveText('forever.yaml');
  await expect(page.getByTestId('check-outcome')).toContainText('通った');

  const toggle = page.getByRole('switch', { name: 'Logger を使う' });
  await toggle.focus();
  await page.keyboard.press('Space');
  await expect(toggle).not.toBeChecked();
  await expect(page.getByTestId('check-participant-Logger')).toContainText('外した');
  await expect(primary(page)).not.toHaveAttribute('aria-disabled', 'true');

  // 帯の主なボタンへ、キーボードだけでたどる（帯はカードより前にある）
  for (
    let i = 0;
    i < 40 && !(await primary(page).evaluate((b) => b === document.activeElement));
    i++
  ) {
    await page.keyboard.press('Shift+Tab');
  }
  await expect(primary(page)).toBeFocused();
  const response = page.waitForResponse((r) => r.url().endsWith('/api/run'));
  await page.keyboard.press('Enter');
  const session = ((await (await response).json()) as { session: { id: number } }).session.id;
  // 始めた後も、フォーカスは同じボタンにあり、Enter で止められる
  await expect(primary(page)).toHaveText(/停止/);
  await expect(primary(page)).toBeFocused();
  await expect(page.getByTestId('outcome')).toContainText('実行中');
  await page.keyboard.press('Enter');
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
  await expect(primary(page)).toBeFocused();

  // ログは Tab で届き、矢印で動かせる
  const log = page.getByTestId('log');
  await log.focus();
  await expect(log).toBeFocused();
});

test.describe('the screen at every width', () => {
  /**
   * 状態: ファイルを選んで所見がある、実行中でログが流れる、止めるよう頼んだ（待たずに止めるボタンが出る）、
   * 失敗で終わった、ランチャーが異常終了した。
   */
  const STATES = [
    { name: 'chosen', file: 'check-error.yaml', run: false },
    { name: 'running', file: 'forever.yaml', run: true },
    { name: 'stopping', file: 'ignore-stop.yaml', run: true },
    { name: 'failed', file: 'participant-crash.yaml', run: true },
    { name: 'crashed', file: 'launcher-crash.yaml', run: true },
  ] as const;
  const SCREENSHOT_WIDTHS = [360, 1024, 1920];

  for (const state of STATES) {
    test(`${state.name}: the status bar stays in view, no overflow or overlap`, async ({
      page,
    }, testInfo) => {
      await choose(page, state.file);
      if (state.run) {
        const session = await start(page);
        if (state.name === 'running' || state.name === 'stopping') {
          await expect(page.getByTestId('log-line').nth(5)).toBeVisible({ timeout: RUN_TIMEOUT });
          if (state.name === 'stopping') {
            await primary(page).click();
            await expect(page.getByTestId('force-stop')).toBeVisible();
          }
        } else {
          await expect(primary(page)).toHaveText(/もう一度実行/, { timeout: RUN_TIMEOUT });
          await expect(page.locator(`[data-session="${session}"]`).first()).toBeVisible();
        }
      }
      const content = page.locator('mat-sidenav-content');
      for (const viewport of VIEWPORTS) {
        // 窓は伸ばさない（利用者が見る大きさのまま）
        await page.setViewportSize({ width: viewport.width, height: viewport.height });
        await expect(page.getByTestId('nav')).toHaveAttribute('data-nav-mode', viewport.navMode);
        await content.evaluate((element) => element.scrollTo(0, 0));
        await expect
          .poll(() => findLayoutProblems(page), { message: `${viewport.width} px` })
          .toEqual([]);
        // 操作と結果は、最初の画面に入っていて、下までスクロールしても見えている
        for (const where of ['top', 'bottom'] as const) {
          await content.evaluate(
            (element, to) => element.scrollTo(0, to === 'top' ? 0 : element.scrollHeight),
            where,
          );
          await expect(primary(page), `${viewport.width} px, ${where}`).toBeInViewport({
            ratio: 1,
          });
          await expect(
            page.getByTestId('outcome'),
            `${viewport.width} px, ${where}`,
          ).toBeInViewport({ ratio: 1 });
        }
        if (SCREENSHOT_WIDTHS.includes(viewport.width)) {
          await content.evaluate((element) => element.scrollTo(0, 0));
          // ナビゲーションの動き（幅の区分の切り替え）が終わってから撮る
          await page.waitForTimeout(500);
          await page.screenshot({
            path: testInfo.outputPath(`run-${state.name}-${viewport.width}.png`),
          });
        }
      }
      await stopIfRunning(page);
    });
  }

  test('failed, in the dark color scheme', async ({ page }, testInfo) => {
    await page.emulateMedia({ colorScheme: 'dark' });
    await page.setViewportSize({ width: 1024, height: 768 });
    await choose(page, 'participant-crash.yaml');
    await start(page);
    await expect(primary(page)).toHaveText(/もう一度実行/, { timeout: RUN_TIMEOUT });
    const background = await page.evaluate(() => getComputedStyle(document.body).backgroundColor);
    const [r, g, b] = (background.match(/\d+/g) ?? []).map(Number);
    expect(r + g + b).toBeLessThan(3 * 80);
    await expect.poll(() => findLayoutProblems(page)).toEqual([]);
    await page.waitForTimeout(500);
    await page.screenshot({ path: testInfo.outputPath('run-failed-1024-dark.png') });
  });
});
