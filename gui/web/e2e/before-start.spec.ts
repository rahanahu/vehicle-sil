// 止めた状態で始めた実行が、最初のステップの前で待っている形（ランチャーが Control Server を持つとき: phase は
// waiting のまま、time の事象は来ない）を、画面から再開できること。偽のランチャーに Control Server を持つ形を
// させるので、この試験だけの GUI サーバーを起動する。
import { expect, test } from '@playwright/test';
import { CONFIGS, FAKE_LAUNCHER, GUI_PORT, GuiServer, launchGuiServer } from './support/gui-server';
import { RUN_TIMEOUT, WORDS, choose, result, start } from './support/run-page';

const CONTROL_FEATURES =
  'events,stdin-commands,check-variables,speed,start-paused,time-commands,control-links,control-server';

test('started paused with a control server: it waits before the first step, and the screen resumes it', async ({
  page,
}, testInfo) => {
  // 開発サーバーの形（e2e:dev）では回さない（同じ画面。サーバーを別に起動するので、ビルドした画面で 1 回）
  test.skip(testInfo.project.name !== 'static', 'only with the built screen');
  const server: GuiServer = await launchGuiServer({
    launcher: FAKE_LAUNCHER,
    root: CONFIGS,
    env: { FAKE_LAUNCHER_TICK_MS: '50', FAKE_LAUNCHER_FEATURES: CONTROL_FEATURES },
    port: GUI_PORT + 200,
  });
  try {
    await page.goto(server.url);
    await expect(page.getByRole('heading', { level: 1, name: WORDS.ja.run })).toBeVisible();
    await choose(page, 'simulation.yaml');
    await page.getByTestId('start-paused').getByRole('checkbox').check();
    const session = await start(page);
    // 最初のステップの前: サーバーの要約では、段階は waiting のままで、time はまだない
    await expect
      .poll(
        async () => {
          const response = await page.request.get(new URL('api/session', server.url).href, {
            headers: { Authorization: `Bearer ${server.token}` },
          });
          const summary = (await response.json()) as {
            session: { phase: string; time: unknown; variables: { state: string } };
          };
          return [summary.session.phase, summary.session.time, summary.session.variables.state];
        },
        { timeout: RUN_TIMEOUT },
      )
      .toEqual(['waiting', null, 'ready']);
    // 再開は押せる。一時停止は押せない
    await expect(page.getByTestId('resume')).not.toHaveAttribute('aria-disabled', 'true', {
      timeout: RUN_TIMEOUT,
    });
    await expect(page.getByTestId('pause')).toHaveAttribute('aria-disabled', 'true');
    const sent = page.waitForRequest((r) => r.url().endsWith('/api/time'));
    await page.getByTestId('resume').click();
    expect((await sent).postDataJSON()).toEqual({ command: 'resume' });
    await expect(result(page, session).getByTestId('result')).toContainText('成功', {
      timeout: RUN_TIMEOUT,
    });
  } finally {
    await page.request.post(new URL('api/stop', server.url).href, {
      headers: { Authorization: `Bearer ${server.token}`, Origin: new URL(server.url).origin },
      data: { force: true },
    });
    await page.close();
    await server.stop();
  }
});
