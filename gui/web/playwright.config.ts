import { defineConfig, devices } from '@playwright/test';

// static（npm run e2e）: 代役のサーバーがビルド済みの画面（dist/browser）と API を同じオリジンで配信する。
// dev（npm run e2e:dev）: 開発サーバー（ng serve）が画面を配信し、/api と /ws を代役のサーバーへ転送する。
// 切り替えは設定ファイルで行う（環境変数は Windows の npm スクリプトで書き方が変わるため）。
export type E2eMode = 'static' | 'dev';

// 手元で動かしている開発用のサーバー（8765、4200）とぶつからないポート。
const stubPort = 18765;
const devPort = 14200;

const stub = (extra: string) => ({
  command: `uv run dev/stub_server.py --port ${stubPort} --interval 0.2 ${extra}`,
  url: `http://127.0.0.1:${stubPort}/api/health`,
  reuseExistingServer: false,
  timeout: 120_000,
});

export const e2eConfig = (mode: E2eMode) =>
  defineConfig({
    testDir: 'e2e',
    fullyParallel: false,
    workers: 1,
    retries: 0,
    forbidOnly: true,
    reporter: [['list']],
    outputDir: `test-results/${mode}`,
    use: {
      baseURL: `http://127.0.0.1:${mode === 'dev' ? devPort : stubPort}`,
      trace: 'retain-on-failure',
    },
    // プロジェクトの名前で試験が配信の形を知る
    projects: [{ name: mode, use: { ...devices['Desktop Chrome'] } }],
    webServer:
      mode === 'dev'
        ? [
            stub(''),
            {
              command: `npx ng serve --port ${devPort}`,
              env: { STUB_PORT: String(stubPort) },
              url: `http://127.0.0.1:${devPort}/`,
              reuseExistingServer: false,
              timeout: 180_000,
            },
          ]
        : [stub('--static dist/browser')],
  });

export default e2eConfig('static');
