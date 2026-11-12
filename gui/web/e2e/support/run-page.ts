// 実行の画面を操る手順（e2e/run.spec.ts と、本物のランチャーで動かす e2e/real/ が使う）。
import { Page, expect } from '@playwright/test';
import { guiToken } from './gui-server';

/** ランチャーの 1 回の実行の上限（偽のランチャーは 1 ステップ 50 ms、stop_at_ms まで 10 ステップ。本物の例の ECU はもっと速い）。 */
export const RUN_TIMEOUT = 20_000;

export async function openRun(page: Page): Promise<void> {
  await page.goto(`/#token=${guiToken()}`);
  await expect(page.getByRole('heading', { level: 1, name: '実行' })).toBeVisible();
  await expect(page).toHaveTitle('実行 - vehicle-sil');
}

/** 状態の帯の主なボタン（開始 / 停止 / 止めています / もう一度実行。1 つの要素のまま変わる）。 */
export function primary(page: Page) {
  return page.getByTestId('primary');
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
  await expect(primary(page)).not.toHaveAttribute('aria-disabled', 'true');
  const response = page.waitForResponse((r) => r.url().endsWith('/api/run'));
  await primary(page).click();
  const answer = await response;
  expect(answer.status()).toBe(200);
  return ((await answer.json()) as { session: { id: number } }).session.id;
}

export function result(page: Page, session: number) {
  return page.locator(`[data-testid="result-card"][data-session="${session}"]`);
}

/** 前の試験のセッションが動いていたら止める（順に止め、止まらなければ待たずに止める）。 */
export async function stopIfRunning(page: Page): Promise<void> {
  await expect(primary(page)).toBeVisible();
  // つないで揃うまでは、帯が「開始」のままのことがある
  await expect(page.getByTestId('connection')).toHaveText('', { timeout: 10_000 });
  await expect(page.getByTestId('blocked').filter({ hasText: 'サーバーと揃うまで' })).toHaveCount(
    0,
  );
  if ((await primary(page).getAttribute('data-action')) === 'stop') {
    await primary(page).click();
  }
  const force = page.getByTestId('force-stop');
  await expect(primary(page)).toHaveAttribute('data-action', /start|again|stopping/);
  if ((await primary(page).getAttribute('data-action')) === 'stopping') {
    try {
      await expect(primary(page)).toHaveAttribute('data-action', /start|again/, { timeout: 3000 });
    } catch {
      await force.click();
      await page.getByTestId('force-stop-confirm').click();
    }
  }
  await expect(primary(page)).toHaveAttribute('data-action', /start|again/, {
    timeout: RUN_TIMEOUT,
  });
}
