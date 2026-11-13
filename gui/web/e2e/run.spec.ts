// 実行の画面を、本物の GUI サーバーと流し直すランチャー（gui/server/tests/replay_launcher.py）で通す。
// 構成ファイルは gui/server/tests/configs/ で、ファイルごとに本物のランチャーの出力を録ったもの（recordings/）がある。
// サーバーは 1 つのセッションしか動かさないので、試験は順に動き、動かしたものは止めてから終わる。
import { Locator, Page, WebSocketRoute, expect, test } from '@playwright/test';
import { VIEWPORTS, findLayoutProblems } from './support/layout';
import {
  RUN_TIMEOUT,
  ServerSession,
  WORDS,
  choose,
  chosenFile,
  openRun,
  press,
  protocolExample,
  languageOf,
  result,
  serverSession,
  start,
  startButton,
  stopButton,
  stopIfRunning,
  stopOnServer,
} from './support/run-page';
import { guiToken } from './support/gui-server';

test.beforeEach(async ({ page, locale }) => {
  await openRun(page, languageOf(locale));
  await stopIfRunning(page, languageOf(locale));
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
  await expect(startButton(page)).toHaveText(/もう一度実行/);
  await expect(stopButton(page)).toHaveAttribute('aria-disabled', 'true');
});

/** 状態の帯の一時停止と再開のボタン（別々のボタンで、実行している間、いつも開始と停止の後にある）。 */
const pauseButton = (page: Page) => page.getByTestId('pause');
const resumeButton = (page: Page) => page.getByTestId('resume');

/** 時刻の命令を送り、サーバーの応答を待つ（ランチャーが受けたかは、画面に command と time の事象で届く）。 */
async function command(page: Page, button: Locator, body: object): Promise<void> {
  const sent = page.waitForRequest((r) => r.url().endsWith('/api/time'));
  await expect(button).not.toHaveAttribute('aria-disabled', 'true');
  await button.click();
  expect((await sent).postDataJSON()).toEqual(body);
}

test('started paused: a step, resume, pause, a new speed, stop', async ({ page }) => {
  await choose(page, 'forever.yaml');
  await page.getByTestId('start-paused').getByRole('checkbox').check();
  const session = await start(page);
  // 仮想時間 0 で止まっている: 再開とステップ送りだけが押せる。一時停止と再開は別のボタンで、場所は変わらない
  await expect(page.getByTestId('outcome')).toContainText('一時停止中', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('time-state')).toHaveText('一時停止中');
  await expect(page.getByTestId('time-sim')).toHaveText('0.000 s');
  await expect(pauseButton(page)).toHaveAttribute('aria-disabled', 'true');
  await expect(resumeButton(page)).not.toHaveAttribute('aria-disabled', 'true');
  const place = await resumeButton(page).boundingBox();

  // ステップ送り: ランチャーが刻み（10 ms）の倍数に切り上げた、実際に進む量を出す
  // ダブルクリックしても 1 回だけ進む（ランチャーの答えが届くまで、次を送らない）
  await page.getByTestId('step-ms').fill('２５');
  const steps: unknown[] = [];
  page.on('request', (r) => {
    if (r.url().endsWith('/api/time')) steps.push(r.postDataJSON());
  });
  await expect(page.getByTestId('step')).not.toHaveAttribute('aria-disabled', 'true');
  await page.getByTestId('step').dblclick();
  await expect(page.getByTestId('step-hint')).toContainText('30 ms');
  await expect(page.getByTestId('time-sim')).toHaveText('0.030 s');
  await expect(page.getByTestId('time-state')).toHaveText('一時停止中');
  expect(steps).toEqual([{ command: 'step', ms: 25 }]);

  await command(page, resumeButton(page), { command: 'resume' });
  await expect(page.getByTestId('outcome')).toContainText('実行中');
  await expect(resumeButton(page)).toHaveAttribute('aria-disabled', 'true');
  await expect(pauseButton(page)).not.toHaveAttribute('aria-disabled', 'true');
  // 動いている間は、ステップ送りはできない（理由を出す）
  await expect(page.getByTestId('step')).toHaveAttribute('aria-disabled', 'true');
  await expect(page.getByTestId('step-hint')).toContainText('一時停止している間だけ');
  await expect
    .poll(async () =>
      Number((await page.getByTestId('time-sim').textContent())?.match(/[\d.]+/)?.[0]),
    )
    .toBeGreaterThan(0.1);

  await command(page, pauseButton(page), { command: 'pause' });
  await expect(page.getByTestId('outcome')).toContainText('一時停止中');
  expect(await resumeButton(page).boundingBox()).toEqual(place);

  // 速度を変える（一時停止はそのまま）
  const card = page.getByTestId('time-card');
  await card.getByRole('radio', { name: '倍率' }).click();
  await card.getByTestId('speed-factor').fill('2');
  await command(page, page.getByTestId('set-speed'), { command: 'speed', value: 2 });
  await expect(page.getByTestId('time-speed')).toHaveText('2 倍');
  await expect(page.getByTestId('speed')).toHaveText('2 倍');
  await expect(page.getByTestId('time-state')).toHaveText('一時停止中');

  await press(stopButton(page));
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
  // 終わった後: 時間の進み方の要約。止めた後の停止のボタンは「止めています…」のままにしない
  await expect(result(page, session).getByTestId('timing')).toContainText('2 倍');
  await expect(result(page, session).getByTestId('timing')).toContainText('一時停止していた時間');
  await expect(page.getByTestId('time-card')).toHaveCount(0);
  await expect(stopButton(page)).toHaveAttribute('aria-disabled', 'true');
  await expect(stopButton(page)).toHaveText(/停止/);
  await expect(stopButton(page)).not.toHaveText(/止めています/);
  // 実行が終わったら、一時停止と再開は出さない
  await expect(pauseButton(page)).toHaveCount(0);
  await expect(resumeButton(page)).toHaveCount(0);
});

test('a command the launcher refuses shows its reason', async ({ page }) => {
  await choose(page, 'forever.yaml');
  await start(page);
  await expect(pauseButton(page)).not.toHaveAttribute('aria-disabled', 'true', {
    timeout: RUN_TIMEOUT,
  });
  // 画面はステップ送りを押させない（動いている）。ほかの道具から送られた命令の結果も、同じ所に出る
  const headers = { Authorization: `Bearer ${guiToken()}`, Origin: new URL(page.url()).origin };
  await page.request.post('api/time', { headers, data: { command: 'step', ms: 10 } });
  await expect(page.getByTestId('time-refused')).toContainText('受け付けられませんでした');
  await expect(page.getByTestId('time-refused')).toContainText(
    'step works while the simulation is paused',
  );
  await stopOnServer(page);
});

test('a run behind real time says so in words, also after it ended', async ({ page }) => {
  // 構成ファイルの replay-fault: late: 速度 1 の録りで、時刻の事象の 1 つが 200 ms 遅れる
  await choose(page, 'late.yaml');
  const session = await start(page);
  await expect(page.getByTestId('late')).toContainText('実時間に遅れました', {
    timeout: RUN_TIMEOUT,
  });
  await expect(page.getByTestId('late')).toContainText('200 ms');
  // 読み上げるのは状態の帯の決まった文だけ（数は読み上げない）
  await expect(page.getByTestId('late-announce')).toHaveText('実時間に遅れました。');
  await expect(page.getByRole('status').filter({ hasText: '実時間に遅れ' })).toHaveCount(1);
  await expect(page.getByTestId('time-late')).toContainText('実時間に遅れました');
  await expect(page.getByTestId('time-lateness')).toContainText('200 ms');
  await press(stopButton(page));
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
  await expect(result(page, session).getByTestId('timing-late')).toContainText(
    '実時間に遅れました',
  );
  await expect(result(page, session).getByTestId('finding').first()).toContainText(
    'behind-real-time',
  );
  await expect(page.getByTestId('late')).toHaveText('');
});

test('a small lateness within the tolerance is not called late', async ({ page }) => {
  // 構成ファイルの replay-fault: jitter: 速度 1 の録りで、時刻の事象の 1 つおきが 3 ms 遅れる（許容の 10 ms の内）
  await choose(page, 'jitter.yaml');
  await start(page);
  await expect(page.getByTestId('time-lateness')).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect
    .poll(async () =>
      Number((await page.getByTestId('time-sim').textContent())?.match(/[\d.]+/)?.[0]),
    )
    .toBeGreaterThan(1);
  await expect(page.getByTestId('late')).toHaveText('');
  await expect(page.getByTestId('late-announce')).toHaveText('');
  await expect(page.getByTestId('time-late')).toHaveText('');
  await stopOnServer(page);
});

test('a launcher without the time features shows no time controls', async ({ page }) => {
  await page.route('**/api/app', async (route) => {
    const response = await route.fetch();
    const app = (await response.json()) as { launcher: { version: { features: string[] } } };
    app.launcher.version.features = ['events', 'stdin-commands', 'check-variables'];
    await route.fulfill({ response, json: app });
  });
  await page.reload();
  await choose(page, 'forever.yaml');
  await expect(page.getByTestId('check-outcome')).toContainText('通った');
  await expect(pauseButton(page)).toHaveCount(0);
  await expect(resumeButton(page)).toHaveCount(0);
  await expect(page.getByTestId('start-paused')).toHaveCount(0);
  await expect(page.getByRole('radio', { name: '等速' })).toHaveCount(0);
  const run = page.waitForRequest((r) => r.url().endsWith('/api/run'));
  await start(page);
  expect((await run).postDataJSON()).toMatchObject({ speed: null, start_paused: false });
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('time-card')).toHaveCount(0);
  await stopOnServer(page);
});

test('how each participant is linked to its variables, a trouble in words', async ({ page }) => {
  // 構成ファイルの replay-fault: control-trouble: Engine は hello が来ず、Dashboard は形式の版が違い、Gauge は口を出さない
  await choose(page, 'control-trouble.yaml');
  const session = await start(page);
  await expect(result(page, session)).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('control-Engine')).toContainText('hello が来なかった');
  await expect(page.getByTestId('control-Dashboard')).toContainText('形式の版 2 が違う');
  await expect(page.getByTestId('control-Gauge')).toContainText('読み書きの口を出していない');
  await expect(page.getByTestId('control-Logger')).toContainText('変数: なし');
  await expect(result(page, session).getByTestId('finding').first()).toContainText(
    'control-link-no-hello',
  );
});

test('a check with an error: the findings, and no start', async ({ page }) => {
  await choose(page, 'check-error.yaml');
  await expect(page.getByTestId('check-outcome')).toContainText('誤りがある');
  await expect(page.getByTestId('check-card').getByTestId('finding').first()).toContainText(
    'step-mismatch',
  );
  await expect(startButton(page)).toHaveAttribute('aria-disabled', 'true');
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
  await expect(startButton(page)).not.toHaveAttribute('aria-disabled', 'true');
  expect(checks).toEqual([{ file: 'forever.yaml', disable: ['Dashboard'], manual: [] }]);
  expect(failures).toEqual([]);
});

test('stopping a run that does not end by itself', async ({ page }) => {
  await choose(page, 'forever.yaml');
  const session = await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });
  const stop = page.waitForRequest((r) => r.url().endsWith('/api/stop'));
  await press(stopButton(page));
  expect((await stop).postDataJSON()).toEqual({ force: false });
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
  // 利用者が止めた中断は失敗の色と ✕ にしない
  await expect(result(page, session).getByTestId('result')).toHaveAttribute('data-tone', 'neutral');
  await expect(page.getByTestId('outcome')).toHaveAttribute('data-tone', 'neutral');
  await expect(page.getByTestId('outcome')).not.toContainText('✕');
});

test('a double click on stop does not stop without waiting', async ({ page }) => {
  await choose(page, 'ignore-stop.yaml');
  const session = await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });
  const bodies: unknown[] = [];
  page.on('request', (r) => {
    if (r.url().endsWith('/api/stop')) bodies.push(r.postDataJSON());
  });
  await expect(stopButton(page)).toHaveAttribute('data-armed', 'true');
  await stopButton(page).dblclick();
  // 2 回目のクリックより後に、サーバーが停止を受けた知らせが届いた
  await expect(page.getByTestId('outcome')).toContainText('participant が終わるのを待っています');
  await expect(stopButton(page)).toHaveText(/止めています/);
  // 待たずに止めてはいない（サーバーの要約でも）
  expect(bodies.every((body) => (body as { force: boolean }).force === false)).toBe(true);
  expect((await serverSession(page))?.stop).toBe('requested');
  await expect(page.getByTestId('status-Engine')).not.toContainText('強制終了');

  // 待たずに止める: 確かめの「やめる」では止めない
  const force = page.getByTestId('force-stop');
  await press(force);
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
  // 待たずに止めるボタンは消える: フォーカスは、いまの状態を言う帯の文へ移る（body に落ちない）
  await expect(force).toHaveCount(0);
  await expect(page.getByTestId('outcome')).toBeFocused();
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
  // Engine と Dashboard は頼まれて終わり、止まらない Logger（SIGTERM を無視する）は強制終了される
  await expect(page.getByTestId('status-Logger')).toContainText('強制終了した');
  await expect(page.getByTestId('status-Engine')).not.toContainText('強制終了');
});

test('the dialog to stop without waiting closes when the run ends meanwhile', async ({ page }) => {
  await choose(page, 'ignore-stop.yaml');
  const session = await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });
  await press(stopButton(page));
  await press(page.getByTestId('force-stop'));
  await expect(page.getByRole('dialog')).toContainText('待たずに止めますか');
  // 別のところ（ここでは API）で止めた
  await stopOnServer(page);
  await expect(page.getByRole('dialog')).toBeHidden();
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('outcome')).toBeFocused();
});

/** 開始と停止の要求を、送った順に記録する。 */
function recordActions(page: Page): string[] {
  const actions: string[] = [];
  page.on('request', (r) => {
    const match = /\/api\/(run|stop)$/.exec(r.url());
    if (match) actions.push(match[1]);
  });
  return actions;
}

/** 要素の真ん中（マウスで押す位置）。 */
async function centre(locator: Locator): Promise<{ x: number; y: number }> {
  const box = await locator.boundingBox();
  if (!box) throw new Error('the element is not visible');
  return { x: box.x + box.width / 2, y: box.y + box.height / 2 };
}

test('a double click on start starts one run and does not stop it', async ({ page }) => {
  await choose(page, 'forever.yaml');
  await expect(startButton(page)).toHaveAttribute('data-armed', 'true');
  const actions = recordActions(page);
  // 利用者の速い続けての押し（ダブルクリックの 2 回目、3 回目）: 応答の前後にまたがる
  const at = await centre(startButton(page));
  const reply = page.waitForResponse((r) => r.url().endsWith('/api/run'));
  for (let i = 0; i < 5; i++) {
    await page.mouse.click(at.x, at.y);
    await page.waitForTimeout(100);
  }
  const session = ((await (await reply).json()) as { session: { id: number } }).session.id;
  // 押した後も実行は続いている（押した時刻より後の行が届いた）
  await expect(page.getByTestId('log-line').nth(8)).toBeVisible({ timeout: RUN_TIMEOUT });
  expect(actions).toEqual(['run']);
  expect(await serverSession(page)).toMatchObject({ id: session, stop: 'none' });
});

test('repeated Enter on start starts one run and does not stop it', async ({ page }) => {
  await choose(page, 'forever.yaml');
  await expect(startButton(page)).toHaveAttribute('data-armed', 'true');
  const actions = recordActions(page);
  await startButton(page).focus();
  const reply = page.waitForResponse((r) => r.url().endsWith('/api/run'));
  for (let i = 0; i < 5; i++) {
    await page.keyboard.press('Enter');
    await page.waitForTimeout(100);
  }
  const session = ((await (await reply).json()) as { session: { id: number } }).session.id;
  await expect(page.getByTestId('log-line').nth(8)).toBeVisible({ timeout: RUN_TIMEOUT });
  expect(actions).toEqual(['run']);
  expect(await serverSession(page)).toMatchObject({ id: session, stop: 'none' });
});

test('a click meant for stop as the run ends does not start a new run', async ({ page }) => {
  // 実行が終わった知らせを、利用者が停止を押す直前に差し込むために、WebSocket を間に入れる
  let route: WebSocketRoute | undefined;
  await page.routeWebSocket(/\/ws$/, (ws) => {
    ws.connectToServer();
    route = ws;
  });
  await page.reload();
  await choose(page, 'forever.yaml');
  const session = await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(stopButton(page)).toHaveText(/停止/);
  const at = await centre(stopButton(page));
  const actions = recordActions(page);

  // 実行が終わった（結果は成功）。サーバーの要約を写して、終わった形にする
  const now = await serverSession(page);
  const ended = (protocolExample('session-ended') as { response: { session: ServerSession } })
    .response.session;
  route?.send(
    JSON.stringify({
      type: 'session',
      session: {
        ...now,
        state: 'ended',
        launcher: { ...(now?.launcher ?? {}), running: false, exit_code: 0 },
        phase: 'ended',
        result: ended['result'],
      },
    }),
  );
  await expect(page.getByTestId('outcome')).toContainText('成功');
  // 利用者は、止めようとして同じ位置を押し続ける。もう一度実行できるようになった後も、少し押し続ける
  let after = 0;
  for (let i = 0; i < 100 && after < 4; i++) {
    await page.mouse.click(at.x, at.y);
    const startable =
      (await startButton(page).getAttribute('aria-disabled')) !== 'true' &&
      (await startButton(page).getAttribute('data-armed')) !== 'false';
    if (startable) after++;
    await page.waitForTimeout(100);
  }
  expect(after).toBe(4);
  expect(actions).not.toContain('run');
  expect((await serverSession(page))?.id).toBe(session);
  await stopOnServer(page);
});

test('a participant that fails: the result, the reason and its exit code', async ({ page }) => {
  await choose(page, 'participant-crash.yaml');
  const session = await start(page);
  await expect(result(page, session)).toContainText('失敗', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('outcome')).toContainText('失敗');
  await expect(page.getByTestId('first-error')).toContainText('participant-failed');
  await expect(result(page, session).getByTestId('finding')).toContainText('participant-failed');
  await expect(page.getByTestId('status-Dashboard')).toContainText('終了コード 3');
  await expect(page.getByTestId('status-Dashboard')).toContainText('失敗');
  await expect(page.getByTestId('status-Dashboard')).toContainText('プロセスが失敗で終わった');
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
  // no-result は異常終了のカードが日本語で説明しているので、「詳細」の中にたたむ
  await expect(page.getByTestId('problems-explained')).toContainText('no-result');
  await expect(page.getByTestId('problems-explained')).not.toHaveAttribute('open');
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
  await expect(startButton(page)).toHaveText(/もう一度実行/);
});

test('a launcher that runs on after its result can still be stopped', async ({ page }) => {
  // 構成ファイルの replay-fault: result-then-wait: 結果を書いた後も、止めるよう頼まれるまで終わらない
  await choose(page, 'result-then-wait.yaml');
  const session = await start(page);
  await expect(result(page, session)).toContainText('成功', { timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('outcome')).toContainText('ランチャーが終わるのを待っています');
  await expect(page.getByTestId('finishing')).toBeVisible();
  await expect(stopButton(page)).toHaveText(/停止/);
  await expect(stopButton(page)).not.toHaveAttribute('aria-disabled', 'true');
  // 結果から 1 秒以上たっても（画面の経過が進んだ）、ランチャーは終わらず、停止を出したまま
  const elapsed = async () =>
    Number((await page.getByTestId('elapsed').textContent())?.match(/[\d.]+/)?.[0]);
  const atResult = await elapsed();
  await expect.poll(elapsed, { timeout: 10_000 }).toBeGreaterThan(atResult + 1);
  expect((await serverSession(page))?.launcher.running).toBe(true);
  await expect(stopButton(page)).not.toHaveAttribute('aria-disabled', 'true');
  await expect(startButton(page)).toHaveAttribute('aria-disabled', 'true');
  const stop = page.waitForRequest((r) => r.url().endsWith('/api/stop'));
  await press(stopButton(page));
  expect((await stop).postDataJSON()).toEqual({ force: false });
  await expect(startButton(page)).toHaveText(/もう一度実行/, { timeout: RUN_TIMEOUT });
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
  await expect(stopButton(page)).not.toHaveAttribute('aria-disabled', 'true');
  await expect(page.getByTestId('status-Engine')).toContainText('参加した');
  await expect(page.getByTestId('log-line').first()).toBeVisible();

  // 別のタブ（サーバーの URL から開く）
  const other = await context.newPage();
  await openRun(other);
  await expect(other.getByTestId('session-file')).toHaveText('forever.yaml');
  await expect(other.getByTestId('log-line').first()).toBeVisible();
  // 別のタブで止めても、こちらに届く
  await press(stopButton(other));
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
  // 切れている間も、HTTP で止めるよう頼める（何度頼んでも同じ）。見えていないことは帯が言う
  await expect(page.getByTestId('blocked')).toContainText('最新の状態は見えていません');
  const stop = page.waitForResponse((r) => r.url().endsWith('/api/stop'));
  await press(stopButton(page));
  expect((await stop).request().postDataJSON()).toEqual({ force: false });
  // 応答の要約から、止めるよう頼んだことがわかる
  await expect(stopButton(page)).toHaveText(/止めています/);
  refuse = false;
  await expect(page.getByTestId('connection')).toHaveText('', { timeout: 10_000 });
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });

  // Dashboard の行は仮想時刻 500 ms ごとに「t=0 ms」「t=500 ms」…と 1 行ずつ。重ならず抜けずに並ぶ
  await page.getByTestId('log-source').click();
  await page.getByRole('option', { name: 'Dashboard', exact: true }).click();
  await expect(page.getByTestId('log-count')).toContainText('全');
  // 絞った行の数と、最後の行の時刻から数えた行の数が同じ（重なりも抜けもない）
  await expect
    .poll(async () => {
      const count = Number((await page.getByTestId('log-count').textContent())?.match(/\d+/)?.[0]);
      const last = await page.getByTestId('log-line').last().textContent();
      return Number(last?.match(/t=(\d+) ms/)?.[1]) / 500 + 1 - count;
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
  await expect(page.getByTestId('log-line').first()).toContainText('t=0 ms');
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
  await choose(page, 'simulation.yaml');
  const session = await start(page);
  // 実行が終わってから、その続きの seq で行を足す（ランチャーの事象と seq が重ならない）
  await expect(result(page, session)).toBeVisible({ timeout: RUN_TIMEOUT });
  let seq = Number((await serverSession(page))?.['last_seq']) + 1;
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
  await expect(page.getByTestId('log-line').last()).toContainText(`injected ${seq - 1}`);
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
  // 300 行がすべて届いた後も、読んでいる行は動かない
  await expect(page.getByText('読んでいる間に 300 行が届きました')).toBeVisible();
  expect(await firstVisible()).toBe(before);
  // 末尾へ戻ると、また追従する
  await page.getByTestId('log-to-end').click();
  await expect(page.getByTestId('log-follow')).toHaveClass(/checked/);
  await expect(page.getByTestId('log-line').last()).toContainText(`injected ${seq - 1}`);
  // seq が戻った事象を黙って捨てない
  await expect(page.getByTestId('notice')).toHaveCount(0);
  route?.send(
    JSON.stringify({
      type: 'event',
      session,
      event: { event: 'log', seq: 1, t_ms: 0, source: 'Engine', line: 'late' },
    }),
  );
  await expect(page.getByTestId('notice')).toContainText('seq that is not after the last one');
});

test('a log paused in one run shows the next run, not the lines of the one before', async ({
  page,
}) => {
  await choose(page, 'participant-crash.yaml');
  const first = await start(page);
  await expect(result(page, first)).toBeVisible({ timeout: RUN_TIMEOUT });
  // 前の実行にだけある行に絞り、追従をやめて読む
  await page.getByTestId('log-filter').fill('went wrong');
  await expect(page.getByTestId('log-count')).toContainText(/^\s*1 行/);
  await page.getByTestId('log-follow').click();
  await expect(page.getByTestId('log-follow')).not.toHaveClass(/checked/);
  // 次の実行
  await choose(page, 'simulation.yaml');
  const second = await start(page);
  await expect(result(page, second)).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('log-follow')).toHaveClass(/checked/);
  await expect(page.getByTestId('log-count')).toContainText(/^\s*0 行/);
});

test('stopping while the start waits for its answer', async ({ page }) => {
  // 開始の応答を遅らせる（サーバーは実行を始めている）
  let release: () => void = () => undefined;
  const held = new Promise<void>((resolve) => (release = resolve));
  await page.route(/\/api\/run$/, async (route) => {
    const response = await route.fetch();
    await held;
    await route.fulfill({ response });
  });
  await choose(page, 'forever.yaml');
  await press(startButton(page));
  await expect(page.getByTestId('outcome')).toContainText('始めています');
  await expect(startButton(page)).toHaveAttribute('aria-disabled', 'true');
  const stop = page.waitForResponse((r) => r.url().endsWith('/api/stop'));
  await press(stopButton(page));
  expect((await stop).request().postDataJSON()).toEqual({ force: false });
  await expect(page.getByTestId('outcome')).toContainText('開始をやめます');
  release();
  await expect(page.getByTestId('outcome')).toContainText('中断', { timeout: RUN_TIMEOUT });
  expect((await serverSession(page))?.stop).toBe('requested');
  await page.unroute(/\/api\/run$/);
});

test('the screen asks for the server URL without a token', async ({ browser }) => {
  const page = await browser.newPage();
  await page.goto('/');
  await expect(page.getByTestId('no-token')).toContainText('#token=');
  await page.close();
});

test('everything can be done with the keyboard; Enter on start does not stop the run', async ({
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
  await expect(startButton(page)).toHaveAttribute('data-armed', 'true');

  // 帯の開始のボタンへ、キーボードだけでたどる（帯はカードより前にある）
  for (
    let i = 0;
    i < 40 && !(await startButton(page).evaluate((b) => b === document.activeElement));
    i++
  ) {
    await page.keyboard.press('Shift+Tab');
  }
  await expect(startButton(page)).toBeFocused();
  const actions = recordActions(page);
  const response = page.waitForResponse((r) => r.url().endsWith('/api/run'));
  await page.keyboard.press('Enter');
  const session = ((await (await response).json()) as { session: { id: number } }).session.id;
  await expect(page.getByTestId('outcome')).toContainText('実行中');
  // フォーカスは開始のボタンに残る（押せない）。もう一度 Enter を押しても、止めない
  await expect(startButton(page)).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(page.getByTestId('log-line').nth(5)).toBeVisible({ timeout: RUN_TIMEOUT });
  expect(actions).toEqual(['run']);
  expect((await serverSession(page))?.stop).toBe('none');
  // 止めるのは、次の要素の停止のボタン
  await page.keyboard.press('Tab');
  await expect(stopButton(page)).toBeFocused();
  await expect(stopButton(page)).toHaveAttribute('data-armed', 'true');
  await page.keyboard.press('Enter');
  await expect(result(page, session)).toContainText('中断', { timeout: RUN_TIMEOUT });
  expect(actions).toEqual(['run', 'stop']);
  // 停止のボタンは押せなくなっても、そこにあってフォーカスを持ったまま
  await expect(stopButton(page)).toBeFocused();

  // ログは Tab で届き、矢印で動かせる
  const log = page.getByTestId('log');
  await log.focus();
  await expect(log).toBeFocused();
});

// 英語の文は長い。英語でも同じ幅で崩れないことを確かめる
for (const language of ['ja', 'en'] as const) {
  test.describe(
    language === 'ja' ? 'the screen at every width' : 'the screen at every width, in English',
    () => {
      test.use({ locale: language === 'ja' ? 'ja-JP' : 'en-US' });

      /**
       * 状態: ファイルを選んで所見がある、実行中でログが流れる、止めるよう頼んだ（待たずに止めるボタンが出る）、
       * 失敗で終わった、ランチャーが異常終了した。
       */
      const STATES = [
        { name: 'chosen', file: 'check-error.yaml', run: false },
        { name: 'running', file: 'forever.yaml', run: true },
        { name: 'paused', file: 'forever.yaml', run: true },
        { name: 'stopping', file: 'ignore-stop.yaml', run: true },
        { name: 'failed', file: 'participant-crash.yaml', run: true },
        { name: 'crashed', file: 'launcher-crash.yaml', run: true },
      ] as const;

      for (const state of STATES) {
        test(`${state.name}: the status bar stays in view, no overflow or overlap`, async ({
          page,
        }) => {
          await choose(page, state.file, language);
          if (state.name === 'paused') {
            await page.getByTestId('start-paused').getByRole('checkbox').check();
          }
          if (state.name === 'paused') {
            await start(page);
            await expect(page.getByTestId('resume')).not.toHaveAttribute('aria-disabled', 'true', {
              timeout: RUN_TIMEOUT,
            });
            await expect(page.getByTestId('time-card')).toBeVisible();
          } else if (state.run) {
            const session = await start(page);
            if (state.name === 'running' || state.name === 'stopping') {
              await expect(page.getByTestId('log-line').nth(5)).toBeVisible({
                timeout: RUN_TIMEOUT,
              });
              if (state.name === 'stopping') {
                await press(stopButton(page));
                await expect(page.getByTestId('force-stop')).toBeVisible();
                // 一時停止と再開は、押せないまま同じ場所に残る（待たずに止めるはその後）
                await expect(page.getByTestId('pause')).toHaveAttribute('aria-disabled', 'true');
                const pause = await page.getByTestId('pause').boundingBox();
                const force = await page.getByTestId('force-stop').boundingBox();
                expect(pause && force && (force.y > pause.y || force.x > pause.x)).toBe(true);
              }
            } else {
              await expect(startButton(page)).toHaveText(new RegExp(WORDS[language].again), {
                timeout: RUN_TIMEOUT,
              });
              await expect(page.locator(`[data-session="${session}"]`).first()).toBeVisible();
            }
          }
          const content = page.locator('mat-sidenav-content');
          for (const viewport of VIEWPORTS) {
            // 窓は伸ばさない（利用者が見る大きさのまま）
            await page.setViewportSize({ width: viewport.width, height: viewport.height });
            await expect(page.getByTestId('nav')).toHaveAttribute(
              'data-nav-mode',
              viewport.navMode,
            );
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
              for (const element of [
                startButton(page),
                stopButton(page),
                page.getByTestId('outcome'),
              ]) {
                await expect(element, `${viewport.width} px, ${where}`).toBeInViewport({
                  ratio: 1,
                });
              }
            }
            // 帯は窓の高さの 3 分の 1 を超えない
            const band = await page.locator('app-run-status').boundingBox();
            expect(band?.height ?? 0, `${viewport.width} px`).toBeLessThanOrEqual(
              viewport.height / 3,
            );
          }
          await stopIfRunning(page, language);
        });
      }
    },
  );
}

test.describe('the screen at every width', () => {
  test('failed, in the dark color scheme', async ({ page }) => {
    await page.emulateMedia({ colorScheme: 'dark' });
    await page.setViewportSize({ width: 1024, height: 768 });
    await choose(page, 'participant-crash.yaml');
    await start(page);
    await expect(startButton(page)).toHaveText(/もう一度実行/, { timeout: RUN_TIMEOUT });
    const background = await page.evaluate(() => getComputedStyle(document.body).backgroundColor);
    const [r, g, b] = (background.match(/\d+/g) ?? []).map(Number);
    expect(r + g + b).toBeLessThan(3 * 80);
    await expect.poll(() => findLayoutProblems(page)).toEqual([]);
  });
});
