// 実行の画面で、ネットワークを外につなぐ（部品 <vsil-connect> と <vsil-outside> を画面に置いたもの）。録りは
// connect.yaml のもの（ETH_EXT を偽の pcap のアダプター fake0 につなぐ）。つないだ実行の間の表示を確かめられる速さで、
// この試験だけの GUI サーバーを起動する。
import { expect, test } from '@playwright/test';
import { CONFIGS, REPLAY_LAUNCHER, GuiServer, launchGuiServer } from './support/gui-server';
import { VIEWPORTS, findLayoutProblems } from './support/layout';
import { RUN_TIMEOUT, WORDS, choose, start, startButton, stopButton } from './support/run-page';

test('a network is connected outside only when chosen, says so before each start, and shows its row while it runs', async ({
  page,
}, testInfo) => {
  // 開発サーバーの形（e2e:dev）では回さない（同じ画面。サーバーを別に起動するので、ビルドした画面で 1 回）
  test.skip(testInfo.project.name !== 'static', 'only with the built screen');
  test.setTimeout(120_000);
  const server: GuiServer = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    env: { REPLAY_TICK_MS: '250' },
    port: 0,
  });
  const posted: { path: string; body: unknown }[] = [];
  page.on('request', (request) => {
    if (request.method() === 'POST')
      posted.push({ path: new URL(request.url()).pathname, body: request.postDataJSON() });
  });
  const listed = () => posted.filter((p) => p.path === '/api/connections');
  let sockets = 0;
  page.on('websocket', () => sockets++);
  /** 写真を残す（試験の出力のディレクトリ。窓の大きさのまま、上から）。 */
  const shoot = async (state: string) => {
    await page.locator('mat-sidenav-content').evaluate((element) => element.scrollTo(0, 0));
    const width = page.viewportSize()?.width ?? 0;
    await page.screenshot({ path: testInfo.outputPath(`connect-${state}-${width}.png`) });
  };
  try {
    await page.goto(server.url);
    await expect(page.getByRole('heading', { level: 1, name: WORDS.ja.run })).toBeVisible();
    await choose(page, 'connect.yaml');
    await page.getByTestId('stop-at-ms').fill('1000');

    // 照合の can と ethernet のネットワークごとに選択がある。最初は「つながない」で、開くまで一覧を取らない
    // （SIL Kit だけの PC でブリッジを起こさない）
    const eth = page.getByTestId('connect-ETH_EXT');
    // 選択そのもの（隠れている間は、役割で探すと見つからない）
    const select = eth.locator('select');
    await expect(page.getByTestId('connect-J1939_CAN').getByRole('combobox')).toHaveValue('none');
    await expect(select).toHaveValue('none');
    // 部品の語は英語のまま
    await expect(select.locator('option[value="none"]')).toHaveText('SIL Kit only (not connected)');
    await expect(
      page.getByText('つなぐ先の一覧は、GUI サーバーが動いている PC のものです。'),
    ).toBeVisible();
    await expect(page.getByTestId('connect-summary')).toHaveCount(0);
    expect(listed()).toEqual([]);

    // キーボードで開く（Tab で通り過ぎるだけでは取らない）
    await select.focus();
    await select.press('Tab');
    expect(listed()).toEqual([]);
    await select.focus();
    await select.press('ArrowDown');
    await expect(select.locator('option[value="fake0"]')).toHaveCount(1);
    expect(listed()).toEqual([
      { path: '/api/connections', body: { file: 'connect.yaml', network: 'ETH_EXT' } },
    ]);
    // 先の説明（ブリッジの文。使用中のデバイスもここに出る）と、使えない先の理由が読める
    await expect(select.locator('option[value="fake0"]')).toContainText('up, connected');
    await expect(select.locator('option[value="tap:lo"]')).toBeDisabled();
    await expect(select.locator('option[value="tap:lo"]')).toContainText('not Ethernet');

    await select.selectOption('fake0');
    // 始める前に、開始のボタンの下に、つなぐ先が出る
    await expect(page.getByTestId('connect-summary')).toContainText('ETH_EXT → fake0');
    // 等速を明示しているので、外部接続による速度変更の注記は出ない。
    await expect(eth.locator('.vsil-findings li[data-code="outside-speed-set"]')).toHaveCount(0);

    // 狭い幅でも横にはみ出さない（先の説明が長い選択の箱も）
    const narrow = VIEWPORTS[0];
    await shoot('chosen');
    await page.setViewportSize({ width: narrow.width, height: narrow.height });
    await expect.poll(() => findLayoutProblems(page)).toEqual([]);
    await shoot('chosen');
    await page.setViewportSize({ width: 1200, height: 800 });

    await start(page);
    expect(posted.filter((p) => p.path === '/api/run')).toEqual([
      { path: '/api/run', body: expect.objectContaining({ connect: { ETH_EXT: 'fake0' } }) },
    ]);
    // 走行中: つないだネットワークの行、時刻の操作が使えない理由（ランチャーの文）。停止は押せる
    const row = page.getByTestId('outside').locator('li');
    await expect(row).toHaveAttribute('data-network', 'ETH_EXT', { timeout: RUN_TIMEOUT });
    await expect(row).toContainText('fake0');
    await expect(page.getByTestId('step-hint')).toContainText('keeps to real time');
    await expect(page.getByTestId('pause')).toHaveAttribute('aria-disabled', 'true');
    await expect(page.getByTestId('pause')).toHaveAccessibleDescription(/pause is refused/);
    await expect(page.getByTestId('pause-hint')).toContainText('pause is refused');
    // 速さは 1 だけ: 最速と倍率は押せず、そう言う
    const card = page.getByTestId('time-card');
    for (const name of ['最速', '倍率'])
      await expect(card.getByRole('radio', { name })).toHaveAttribute('aria-disabled', 'true');
    await expect(card.getByTestId('speed-limits')).toHaveText(
      'この実行で選べる速さは等速だけです。',
    );
    await expect(stopButton(page)).not.toHaveAttribute('aria-disabled', 'true');
    // 走行中は選択を隠す（外さないので選択は残る）。開けないので一覧も取らない
    await expect(page.getByTestId('connect-ETH_EXT')).toBeHidden();
    await expect(select).toHaveValue('fake0');
    await expect(page.getByTestId('connect-summary')).toHaveCount(0);
    await shoot('running');
    await page.setViewportSize({ width: narrow.width, height: narrow.height });
    await expect.poll(() => findLayoutProblems(page)).toEqual([]);
    await shoot('running');
    await page.setViewportSize({ width: 1200, height: 800 });

    // 終わると行は消え、選択は残る。開始のボタンの下に、またつなぐ先が出る
    await expect(startButton(page)).toHaveText(new RegExp(WORDS.ja.again), {
      timeout: 3 * RUN_TIMEOUT,
    });
    // 行は実行の状態が言う間は残り、ランチャーが終わると消える
    await expect(page.getByTestId('outside-card')).toHaveCount(0, { timeout: RUN_TIMEOUT });
    await expect(select).toBeVisible();
    await expect(select).toHaveValue('fake0');
    await expect(page.getByTestId('connect-summary')).toContainText('ETH_EXT → fake0');
    expect(listed()).toHaveLength(1);

    // 「もう一度実行」は同じ先につなぐ
    await start(page);
    expect(posted.filter((p) => p.path === '/api/run')[1]).toEqual({
      path: '/api/run',
      body: expect.objectContaining({ connect: { ETH_EXT: 'fake0' } }),
    });
    await expect(row).toHaveAttribute('data-network', 'ETH_EXT', { timeout: RUN_TIMEOUT });
    // 録りの実行は停止の命令を持たないので、ランチャーが終わるまで待つ
    await expect(startButton(page)).toHaveText(new RegExp(WORDS.ja.again), {
      timeout: 3 * RUN_TIMEOUT,
    });
    await expect(page.getByTestId('outside-card')).toHaveCount(0, { timeout: RUN_TIMEOUT });
    // 画面と部品は 1 本の接続を使う
    expect(sockets).toBe(1);

    // 言語を替えて読み込み直すと、選択は持ち越さない（つながない、に戻る）
    await page.getByTestId('language-menu').click();
    const loaded = page.waitForEvent('load');
    await page.getByRole('menuitemradio', { name: 'English' }).click();
    await loaded;
    await expect(page.locator('html')).toHaveAttribute('lang', 'en');
    await expect(page.getByTestId('check-outcome')).toBeVisible({ timeout: RUN_TIMEOUT });
    await expect(select).toHaveValue('none');
    await expect(page.getByTestId('connect-summary')).toHaveCount(0);
  } finally {
    await page.request.post(new URL('api/stop', server.url).href, {
      headers: { Authorization: `Bearer ${server.token}`, Origin: new URL(server.url).origin },
      data: { force: true },
    });
    await page.close();
    await server.stop();
  }
});
