// 自分の画面のひな形（examples/gui_starter）を、GUI サーバーの --web で配って通す。サンプルの画面を使わず、
// クライアントのライブラリを 1 つにまとめたファイル（npm run build:client）だけで、つなぐ、照合、開始と停止、変数を
// 見て書く、ができること。偽のランチャーと偽の Control Server が相手。
import { expect, test } from '@playwright/test';
import { copyFileSync, cpSync, existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  CONFIGS,
  FAKE_LAUNCHER,
  GUI_PORT,
  GuiServer,
  REPOSITORY,
  launchGuiServer,
} from './support/gui-server';

const CONTROL_FEATURES =
  'events,stdin-commands,check-variables,speed,start-paused,time-commands,control-links,control-server';
const BUNDLE = join(REPOSITORY, 'build', 'gui-client', 'vehicle-sil-gui-client.mjs');

let server: GuiServer;
let screen: string;

test.beforeAll(async () => {
  // 開発サーバーの形（e2e:dev）では回さない（サンプルの画面を使わないので、1 回で足りる）
  test.skip(test.info().project.name !== 'static', 'only once');
  if (!existsSync(BUNDLE)) {
    throw new Error(`${BUNDLE} is missing: run npm run build:client in gui/web first`);
  }
  // README のとおり、ひな形を写してライブラリを隣に置く
  screen = mkdtempSync(join(tmpdir(), 'vehicle-sil-gui-starter-'));
  cpSync(join(REPOSITORY, 'examples', 'gui_starter'), screen, { recursive: true });
  copyFileSync(BUNDLE, join(screen, 'vehicle-sil-gui-client.mjs'));
  server = await launchGuiServer({
    launcher: FAKE_LAUNCHER,
    root: CONFIGS,
    env: { FAKE_LAUNCHER_TICK_MS: '50', FAKE_LAUNCHER_FEATURES: CONTROL_FEATURES },
    port: GUI_PORT + 500,
    web: screen,
  });
});

test.afterAll(async () => {
  await server?.stop();
  if (screen) rmSync(screen, { recursive: true, force: true });
});

test('the starter screen checks, runs, shows a variable and writes it', async ({ page }) => {
  const errors: string[] = [];
  page.on('pageerror', (error) => errors.push(error.message));
  page.on('console', (message) => {
    if (message.type() === 'error') errors.push(message.text());
  });
  await page.goto(server.url);
  await expect(page.getByTestId('connection')).toHaveText('connection: open');
  expect(new URL(page.url()).hash).toBe(''); // the token left the URL

  await page.getByTestId('config').selectOption('forever.yaml');
  await page.getByTestId('check').click();
  await expect(page.getByTestId('check-result')).toHaveText(/^passed \(errors 0/);

  await page.getByTestId('start').click();
  await expect(page.getByTestId('phase')).toHaveText('running');
  await expect(page.getByTestId('variables')).toHaveText('ready');
  await page.getByTestId('variable').selectOption('Engine.engine.demand');
  await page.getByTestId('new-value').fill('40');
  await page.getByTestId('write').click();
  await expect(page.getByTestId('write-result')).toHaveText(/^applied at \d+ ms$/);
  await expect(page.getByTestId('value')).toHaveText(/^40 % \(at \d+ ms\)$/);

  await page.getByTestId('stop').click();
  await expect(page.getByTestId('result')).not.toHaveText('-');
  expect(errors).toEqual([]);
});
