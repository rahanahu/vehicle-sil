// 実行の画面の写真（環境変数 VEHICLE_SIL_GUI_SHOTS にディレクトリを書いたときだけ撮る）。
// 状態ごとに新しい GUI サーバー（流し直すランチャー）を起動するので、前の試験の実行は写らない。
// 窓は伸ばさず、利用者が見る大きさのまま撮る。
import { Page, expect, test } from '@playwright/test';
import { CONFIGS, REPLAY_LAUNCHER, GuiServer, launchGuiServer } from './support/gui-server';
import { VIEWPORTS } from './support/layout';
import {
  Language,
  RUN_TIMEOUT,
  WORDS,
  choose,
  press,
  start,
  startButton,
  stopButton,
} from './support/run-page';

const directory = process.env['VEHICLE_SIL_GUI_SHOTS'];
/** 撮る画面の言語（VEHICLE_SIL_GUI_SHOTS_LANGUAGE、ja か en。既定は ja）。 */
const language: Language = process.env['VEHICLE_SIL_GUI_SHOTS_LANGUAGE'] === 'en' ? 'en' : 'ja';

/** 撮る状態と、その構成ファイル。 */
const STATES = [
  { name: 'chosen', file: 'check-error.yaml' },
  { name: 'running', file: 'forever.yaml' },
  { name: 'stopping', file: 'ignore-stop.yaml' },
  { name: 'ended', file: 'simulation.yaml' },
  { name: 'failed', file: 'participant-crash.yaml' },
  { name: 'crashed', file: 'launcher-crash.yaml' },
] as const;

const WIDTHS = [360, 1024];

async function reach(page: Page, state: (typeof STATES)[number]): Promise<void> {
  await choose(page, state.file, language);
  if (state.name === 'chosen') return;
  await start(page);
  if (state.name === 'running' || state.name === 'stopping') {
    await expect(page.getByTestId('log-line').nth(5)).toBeVisible({ timeout: RUN_TIMEOUT });
    if (state.name === 'stopping') {
      await press(stopButton(page));
      await expect(page.getByTestId('force-stop')).toHaveAttribute('data-armed', 'true');
    }
    return;
  }
  await expect(startButton(page)).toHaveText(new RegExp(WORDS[language].again), {
    timeout: RUN_TIMEOUT,
  });
  await expect(startButton(page)).toHaveAttribute('data-armed', 'true');
}

test.describe('screenshots', () => {
  test.skip(!directory, 'VEHICLE_SIL_GUI_SHOTS is not set');
  test.use({ locale: language === 'en' ? 'en-US' : 'ja-JP' });

  for (const state of STATES) {
    test(state.name, async ({ page }, testInfo) => {
      // 開発サーバーの形（e2e:dev）では撮らない（同じ画面を 2 回撮らない）
      test.skip(testInfo.project.name !== 'static', 'only with the built screen');
      const server: GuiServer = await launchGuiServer({
        launcher: REPLAY_LAUNCHER,
        root: CONFIGS,
        env: { REPLAY_TICK_MS: '50' },
        // 試験ごとに空いているポート（前のサーバーの接続が残っていても待てるように）
        port: 0,
      });
      try {
        await page.goto(server.url);
        await expect(
          page.getByRole('heading', { level: 1, name: WORDS[language].run }),
        ).toBeVisible();
        await reach(page, state);
        const content = page.locator('mat-sidenav-content');
        for (const viewport of VIEWPORTS.filter((v) => WIDTHS.includes(v.width))) {
          await page.setViewportSize({ width: viewport.width, height: viewport.height });
          await expect(page.getByTestId('nav')).toHaveAttribute('data-nav-mode', viewport.navMode);
          await content.evaluate((element) => element.scrollTo(0, 0));
          // ナビゲーションの動き（幅の区分の切り替え）が終わってから撮る
          await page.waitForTimeout(500);
          await page.screenshot({ path: `${directory}/run-${state.name}-${viewport.width}.png` });
        }
      } finally {
        // 動いている実行は、待たずに止めてからサーバーを止める（止まらない筋書きもあるので）
        await page.request.post(new URL('api/stop', server.url).href, {
          headers: { Authorization: `Bearer ${server.token}`, Origin: new URL(server.url).origin },
          data: { force: true },
        });
        await page.close();
        await server.stop();
      }
    });
  }
});
