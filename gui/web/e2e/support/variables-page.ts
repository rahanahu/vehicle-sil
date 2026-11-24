// 変数の画面を操る手順（e2e/variables.spec.ts と、本物のランチャーで動かす e2e/real/ が使う）。
import { Locator, Page, expect } from '@playwright/test';
import { scaled } from './timeouts';

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
    data: { file, disable: [], ...(startPaused ? { start_paused: true } : {}) },
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
export async function stopRun(page: Page, target: Target, timeout = scaled(20_000)): Promise<void> {
  const current = await page.request.get(new URL('api/session', target.base).href, {
    headers: { Authorization: `Bearer ${target.token}` },
  });
  expect(current.status()).toBe(200);
  const currentBody = (await current.json()) as {
    instance: string;
    session: { id: string; state: string } | null;
  };
  if (currentBody.session?.state === 'running') {
    await page.request.post(new URL('api/stop-session', target.base).href, {
      headers: headers(target),
      data: {
        run_id: currentBody.session.id,
        instance: currentBody.instance,
        force: true,
      },
    });
  }
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

/**
 * 画面を通さずに書く前に、変数が書ける状態になるまで待つ: いまの実行の変数の状態が `ready` で、`peer` が
 * つながって変数を出している（実行を始めた直後は `connecting` で、書くと 409 `variables-not-ready`）。
 * いまの実行の識別子を返す。
 */
export async function waitForVariables(
  page: Page,
  target: Target,
  peer: string,
  timeout = scaled(20_000),
): Promise<string> {
  let id = '';
  await expect
    .poll(
      async () => {
        const response = await page.request.get(new URL('api/session', target.base).href, {
          headers: { Authorization: `Bearer ${target.token}` },
        });
        const body = (await response.json()) as {
          session: {
            id: string;
            variables: {
              state: string;
              peers: { name: string; connected: boolean; variables: unknown[] }[];
            };
          } | null;
        };
        const variables = body.session?.variables;
        const found = variables?.peers.find((p) => p.name === peer);
        const ready = found?.connected && found.variables.length > 0;
        const listed = !found ? 'not listed' : ready ? 'connected' : 'not connected';
        id = body.session?.id ?? '';
        return `${variables?.state ?? 'no run'}, ${peer} ${listed}`;
      },
      { timeout, message: `the variables of ${peer} did not become writable` },
    )
    .toBe(`ready, ${peer} connected`);
  return id;
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

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/** `@defer` のプレースホルダーを含めて、対象の部品が見える位置までスクロールする。 */
export async function scrollWidgetIntoView(page: Page, peer: string, name: string): Promise<void> {
  const placeholder = page
    .getByRole('region', { name: peer, exact: true })
    .getByTestId('widget-placeholder')
    .filter({
      has: page.locator('.name', { hasText: new RegExp(`^\\s*${escapeRegExp(name)}\\s*$`) }),
    });
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      await widget(page, peer, name).or(placeholder).first().scrollIntoViewIfNeeded();
      return;
    } catch (error) {
      if (attempt === 2 || !(error instanceof Error) || !/not attached/i.test(error.message)) {
        throw error;
      }
    }
  }
}

/** 部品の値を数として読む（単位を除く）。部品は見えている間だけ値を受け取るので、先に見えるところへ動かす。 */
export async function numberOf(page: Page, peer: string, name: string): Promise<number> {
  await scrollWidgetIntoView(page, peer, name);
  const value = widgetValue(page, peer, name);
  await value.scrollIntoViewIfNeeded();
  const text = (await value.textContent()) ?? '';
  return Number(text.trim().split(/\s+/)[0]);
}
