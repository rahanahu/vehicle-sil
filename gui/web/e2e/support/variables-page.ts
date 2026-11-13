// 変数の画面を操る手順（e2e/variables.spec.ts と、本物のランチャーで動かす e2e/real/ が使う）。
import { Locator, Page, expect } from '@playwright/test';

/** 相手の GUI サーバー: 画面の URL（`#token=` なし）とトークン。 */
export interface Target {
  base: string;
  token: string;
}

function headers(target: Target) {
  return { Authorization: `Bearer ${target.token}`, Origin: new URL(target.base).origin };
}

/** 画面を通さずに実行を始める（変数の画面の試験は、実行の画面の操作を確かめない）。 */
export async function startRun(
  page: Page,
  target: Target,
  file: string,
  startPaused = false,
): Promise<void> {
  const response = await page.request.post(new URL('api/run', target.base).href, {
    headers: headers(target),
    data: { file, disable: [], speed: null, start_paused: startPaused },
  });
  expect(response.status(), await response.text()).toBe(200);
}

/** 画面を通さずに時刻の命令を送る。 */
export async function timeCommand(page: Page, target: Target, command: string): Promise<void> {
  const response = await page.request.post(new URL('api/time', target.base).href, {
    headers: headers(target),
    data: { command },
  });
  expect(response.status(), await response.text()).toBe(200);
}

/** 画面を通さずに、待たずに止めて、終わるまで待つ。 */
export async function stopRun(page: Page, target: Target, timeout = 20_000): Promise<void> {
  await page.request.post(new URL('api/stop', target.base).href, {
    headers: headers(target),
    data: { force: true },
  });
  await expect
    .poll(
      async () => {
        const response = await page.request.get(new URL('api/session', target.base).href, {
          headers: { Authorization: `Bearer ${target.token}` },
        });
        const body = (await response.json()) as { session: { state: string } | null };
        return body.session?.state ?? 'none';
      },
      { timeout },
    )
    .not.toBe('running');
}

/** 変数の画面を開く（ナビゲーションから。経路とナビゲーションも確かめる）。 */
export async function openVariables(page: Page, target: Target): Promise<void> {
  await page.goto(`${target.base}#token=${target.token}`);
  await page.getByTestId('nav').getByRole('link', { name: '変数' }).click();
  await expect(page.getByRole('heading', { level: 1, name: '変数' })).toBeVisible();
  await expect(page).toHaveTitle('変数 - vehicle-sil');
  await expect(page).toHaveURL(/\/variables$/);
}

/** 変数の部品（枠）。 */
export function widget(page: Page, peer: string, name: string): Locator {
  return page.locator(`[data-variable="${peer}/${name}"]`);
}

/** 部品が出している値の文字。 */
export function widgetValue(page: Page, peer: string, name: string): Locator {
  return widget(page, peer, name).getByTestId('widget-value');
}

/** 部品の値を数として読む（単位を除く）。部品は見えている間だけ値を受け取るので、先に見えるところへ動かす。 */
export async function numberOf(page: Page, peer: string, name: string): Promise<number> {
  const value = widgetValue(page, peer, name);
  await value.scrollIntoViewIfNeeded();
  const text = (await value.textContent()) ?? '';
  return Number(text.trim().split(/\s+/)[0]);
}
