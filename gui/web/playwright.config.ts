import { defineConfig, devices } from '@playwright/test';
import { GUI_PORT } from './e2e/support/gui-server';
import { scaled } from './e2e/support/timeouts';

// static（npm run e2e）: GUI サーバー（gui/server）がビルド済みの画面（dist/browser）と API を同じオリジンで配信する。
// dev（npm run e2e:dev）: 開発サーバー（ng serve）が画面を配信し、/api と /ws を GUI サーバーへ転送する。
// どちらも GUI サーバーは e2e/support/gui-server.ts が流し直すランチャー（gui/server/tests/replay_launcher.py）と一緒に起動する（トークンを読むため）。
// 切り替えは設定ファイルで行う（環境変数は Windows の npm スクリプトで書き方が変わるため）。
export type E2eMode = 'static' | 'dev';

// 手元で動かしている開発サーバー（4200）とぶつからないポート。
const devPort = 14200;
const devOrigin = `http://127.0.0.1:${devPort}`;

export const e2eConfig = (mode: E2eMode) =>
  defineConfig({
    testDir: 'e2e',
    // 本物のランチャーで動かす e2e は別の設定（playwright.real.config.ts）
    testIgnore: ['real/**'],
    fullyParallel: false,
    workers: 1,
    retries: 0,
    forbidOnly: true,
    // 1 つの試験と expect の待ちの上限（Playwright の既定の 30 秒と 5 秒）に、VEHICLE_SIL_TEST_TIMEOUT_SCALE の倍率を掛ける
    timeout: scaled(30_000),
    expect: { timeout: scaled(5_000) },
    reporter: [['list']],
    outputDir: `test-results/${mode}`,
    globalSetup: './e2e/support/gui-server.ts',
    metadata: mode === 'dev' ? { devOrigin } : {},
    use: {
      baseURL: mode === 'dev' ? devOrigin : `http://127.0.0.1:${GUI_PORT}`,
      trace: 'retain-on-failure',
      // 画面の言語はブラウザの言語で決まる。試験は日本語（元の言語）で書き、英語の試験（i18n.spec.ts）だけが変える
      locale: 'ja-JP',
    },
    // プロジェクトの名前で試験が配信の形を知る
    projects: [{ name: mode, use: { ...devices['Desktop Chrome'] } }],
    webServer:
      mode === 'dev'
        ? [
            {
              command: `npx ng serve --port ${devPort}`,
              env: { GUI_PORT: String(GUI_PORT) },
              url: `${devOrigin}/`,
              reuseExistingServer: false,
              timeout: scaled(180_000),
            },
          ]
        : [],
  });

export default e2eConfig('static');
