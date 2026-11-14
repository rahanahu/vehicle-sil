// 自分の画面のひな形（examples/gui_starter）を、GUI サーバーの --web で配って通す。サンプルの画面を使わず、
// クライアントのライブラリを 1 つにまとめたファイル（npm run build:client）だけで、つなぐ、照合、開始と停止、変数を
// 見て書く、ができること。流し直すランチャーと偽の Control Server が相手。
import { expect, test } from '@playwright/test';
import { copyFileSync, cpSync, existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  CONFIGS,
  REPLAY_LAUNCHER,
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
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    // more: スライダーとランプに合う変数（範囲のある engine.demand、それが 50 を超えると真の engine.warning）
    env: { REPLAY_TICK_MS: '50', REPLAY_FEATURES: CONTROL_FEATURES, REPLAY_VARIABLES: 'more' },
    port: 0,
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

  // スライダーとランプ（client.variables.bind）: 値、書き込みの様子、書けない理由
  await expect(page.getByTestId('slider-name')).toHaveText('Engine.engine.demand');
  await expect(page.getByTestId('lamp-name')).toHaveText('Engine.engine.warning');
  await expect(page.getByTestId('slider-value')).toHaveText('40 %');
  await expect(page.getByTestId('slider')).toHaveValue('40');
  await expect(page.getByTestId('lamp')).toHaveText('off');
  await page.getByTestId('slider').fill('80');
  await expect(page.getByTestId('slider-write')).toHaveText('applied');
  await expect(page.getByTestId('slider-value')).toHaveText('80 %');
  await expect(page.getByTestId('lamp')).toHaveText('on');
  await expect(page.getByTestId('lamp')).toHaveAttribute('data-on', 'true');

  // つまみを動かしている途中で実行が終わる: 離しても書かず、つまみは読んだ値へ戻る
  const box = (await page.getByTestId('slider').boundingBox())!;
  await page.mouse.move(box.x + box.width * 0.8, box.y + box.height / 2);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width * 0.2, box.y + box.height / 2, { steps: 5 });
  await expect(page.getByTestId('slider')).not.toHaveValue('80');
  await page.getByTestId('stop').evaluate((button: HTMLButtonElement) => button.click());
  await expect(page.getByTestId('result')).not.toHaveText('-');
  await expect(page.getByTestId('slider')).toBeDisabled();
  await page.mouse.up();
  await expect(page.getByTestId('slider-value')).toHaveText('80 %');
  await expect(page.getByTestId('slider')).toHaveValue('80');
  await expect(page.getByTestId('slider-why')).toHaveText('(cannot write: ended)');
  expect(errors).toEqual([]);
});
