import { defineConfig, devices } from '@playwright/test';
import { GUI_PORT } from './e2e/support/gui-server';

// 本物のランチャー（npm run e2e:real）: GUI サーバーがビルド済みの画面（dist/browser）と API を配信し、
// 環境変数 VEHICLE_SIL_GUI_LAUNCHER の vehicle_sil_launch と、その隣にビルドした ecu_main の例の ECU を動かす
// （e2e/support/real-server.ts）。先に npm run build が要る。scripts/verify-gui.py --launcher <path> が回す。
export default defineConfig({
  testDir: 'e2e/real',
  fullyParallel: false,
  workers: 1,
  retries: 0,
  forbidOnly: true,
  reporter: [['list']],
  outputDir: 'test-results/real',
  globalSetup: './e2e/support/real-server.ts',
  use: {
    baseURL: `http://127.0.0.1:${GUI_PORT}`,
    trace: 'retain-on-failure',
    // 画面の言語はブラウザの言語で決まる。試験は日本語（元の言語）で書き、英語の試験（i18n.spec.ts）だけが変える
    locale: 'ja-JP',
  },
  projects: [{ name: 'real', use: { ...devices['Desktop Chrome'] } }],
});
