import { defineConfig, devices } from '@playwright/test';
import { GUI_PORT } from './e2e/support/gui-server';

// static（npm run e2e）: GUI サーバー（gui/server）がビルド済みの画面（dist/browser）と API を同じオリジンで配信する。
// dev（npm run e2e:dev）: 開発サーバー（ng serve）が画面を配信し、/api と /ws を GUI サーバーへ転送する。
// どちらも GUI サーバーは e2e/support/gui-server.ts が偽のランチャーと一緒に起動する（トークンを読むため）。
// 切り替えは設定ファイルで行う（環境変数は Windows の npm スクリプトで書き方が変わるため）。
export type E2eMode = 'static' | 'dev';

// 手元で動かしている開発サーバー（4200）とぶつからないポート。
const devPort = 14200;
const devOrigin = `http://127.0.0.1:${devPort}`;

export const e2eConfig = (mode: E2eMode) =>
  defineConfig({
    testDir: 'e2e',
    fullyParallel: false,
    workers: 1,
    retries: 0,
    forbidOnly: true,
    reporter: [['list']],
    outputDir: `test-results/${mode}`,
    globalSetup: './e2e/support/gui-server.ts',
    metadata: mode === 'dev' ? { devOrigin } : {},
    use: {
      baseURL: mode === 'dev' ? devOrigin : `http://127.0.0.1:${GUI_PORT}`,
      trace: 'retain-on-failure',
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
              timeout: 180_000,
            },
          ]
        : [],
  });

export default e2eConfig('static');
