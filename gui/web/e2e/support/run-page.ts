// 実行の画面を操る手順（e2e/run.spec.ts と、本物のランチャーで動かす e2e/real/ が使う）。
import { Locator, Page, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { REPOSITORY, guiToken } from './gui-server';

/** gui/protocol/examples/ の例（名前は拡張子なし）。偽の通知を差し込む試験が、形を作らずに使う。 */
export function protocolExample(name: string): unknown {
  const path = resolve(REPOSITORY, 'gui/protocol/examples', `${name}.json`);
  return JSON.parse(readFileSync(path, 'utf8')) as unknown;
}

/** ランチャーの 1 回の実行の上限（偽のランチャーは 1 ステップ 50 ms、stop_at_ms まで 10 ステップ。本物の例の ECU はもっと速い）。 */
export const RUN_TIMEOUT = 20_000;

export async function openRun(page: Page): Promise<void> {
  await page.goto(`/#token=${guiToken()}`);
  await expect(page.getByRole('heading', { level: 1, name: '実行' })).toBeVisible();
  await expect(page).toHaveTitle('実行 - vehicle-sil');
}

/** 状態の帯の開始のボタン（開始 / もう一度実行）。 */
export function startButton(page: Page) {
  return page.getByTestId('start');
}

/** 状態の帯の停止のボタン（停止 / 止めています）。開始とは別の要素で、いつも開始の後ろにある。 */
export function stopButton(page: Page) {
  return page.getByTestId('stop');
}

/**
 * ボタンを押す: 押せるようになり、押せるようになったばかりの間（押しても何もしない）が過ぎてから押す。
 * Playwright の click は aria-disabled の間は待つが、その間は待たない。
 */
export async function press(button: Locator): Promise<void> {
  await expect(button).not.toHaveAttribute('aria-disabled', 'true');
  await expect(button).toHaveAttribute('data-armed', 'true');
  await button.click();
}

/** 選んでいる構成ファイル（mat-select の表示。選択肢の一覧も mat-select の中に描かれるので、表示の部分だけ）。 */
export function chosenFile(page: Page) {
  return page.getByRole('combobox', { name: '構成ファイル' }).locator('.mat-mdc-select-value-text');
}

/** 構成ファイルを選び、その照合の結果が出るまで待つ。 */
export async function choose(page: Page, file: string): Promise<void> {
  const select = page.getByRole('combobox', { name: '構成ファイル' });
  await expect(select).toBeEnabled();
  if ((await chosenFile(page).textContent())?.trim() !== file) {
    const checked = page.waitForResponse(
      (r) => r.url().endsWith('/api/check') && r.request().postDataJSON().file === file,
    );
    await select.click();
    await page.getByRole('option', { name: file, exact: true }).click();
    await checked;
  }
  await expect(chosenFile(page)).toHaveText(file);
  await expect(page.getByTestId('check-outcome')).toBeVisible();
  await expect(page.getByRole('button', { name: '照合し直す' })).toBeEnabled();
}

/** 始めて、サーバーが付けたセッションの番号を返す。 */
export async function start(page: Page): Promise<number> {
  await expect(startButton(page)).not.toHaveAttribute('aria-disabled', 'true');
  await expect(startButton(page)).toHaveAttribute('data-armed', 'true');
  const response = page.waitForResponse((r) => r.url().endsWith('/api/run'));
  await startButton(page).click();
  const answer = await response;
  expect(answer.status()).toBe(200);
  return ((await answer.json()) as { session: { id: number } }).session.id;
}

/** サーバーの要約の、試験が見る部分。 */
export interface ServerSession {
  id: number;
  state: string;
  stop: string;
  launcher: { running: boolean };
  [key: string]: unknown;
}

/** サーバーのいまの要約（画面を通さずに API で読む。試験の確かめ用）。 */
export async function serverSession(page: Page): Promise<ServerSession | null> {
  const response = await page.request.get('api/session', {
    headers: { Authorization: `Bearer ${guiToken()}` },
  });
  expect(response.status()).toBe(200);
  return ((await response.json()) as { session: ServerSession | null }).session;
}

/**
 * 画面を通さずに、サーバーの実行を止めて終わるまで待つ（順に止め、3 秒で終わらなければ待たずに止める）。
 * 前の試験の後始末と、画面に偽の通知を差し込んだ試験の後始末に使う。
 */
export async function stopOnServer(page: Page): Promise<void> {
  const headers = { Authorization: `Bearer ${guiToken()}`, Origin: new URL(page.url()).origin };
  const ended = async () => (await serverSession(page))?.state !== 'running';
  for (const [force, timeout] of [
    [false, 3000],
    [true, RUN_TIMEOUT],
  ] as const) {
    await page.request.post('api/stop', { headers, data: { force } });
    try {
      await expect.poll(ended, { timeout }).toBe(true);
      return;
    } catch (error) {
      if (force) throw error;
    }
  }
}

export function result(page: Page, session: number) {
  return page.locator(`[data-testid="result-card"][data-session="${session}"]`);
}

/** 前の試験のセッションが動いていたら止め（サーバーの API で）、画面がそれを映すまで待つ。 */
export async function stopIfRunning(page: Page): Promise<void> {
  await expect(page.getByTestId('connection')).toHaveText('', { timeout: 10_000 });
  if ((await serverSession(page))?.state === 'running') await stopOnServer(page);
  await expect(stopButton(page)).toHaveAttribute('aria-disabled', 'true', {
    timeout: RUN_TIMEOUT,
  });
  await expect(page.getByTestId('blocked').filter({ hasText: 'サーバーと揃うまで' })).toHaveCount(
    0,
  );
}
