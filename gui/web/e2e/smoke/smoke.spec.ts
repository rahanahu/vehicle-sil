// 通しの確認用（後で捨てる）。消し方は README.md の「仮の画面」。
// 画面、HTTP、WebSocket（開発サーバーの転送を含む）がつながることだけを確かめる。
import { expect, test } from '@playwright/test';

test('shows the server health and the WebSocket values', async ({ page }) => {
  await page.goto('/smoke');

  await expect(page).toHaveTitle('通しの確認 - vehicle-sil');
  await expect(page.getByTestId('health')).toHaveText('状態: ok（stub）');
  await expect(page.getByTestId('ws-state')).toHaveText('open');
  // 桁区切りなし、小数 2 桁
  await expect(page.getByTestId('value-rpm')).toHaveText(/^\d+\.\d{2}$/);

  // 値が流れ続けている（届いた番号が進む）
  const first = Number(await page.getByTestId('ws-seq').textContent());
  await expect
    .poll(async () => Number(await page.getByTestId('ws-seq').textContent()))
    .toBeGreaterThan(first);
});

test('the navigation moves between the screens', async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto('/smoke');
  const nav = page.getByTestId('nav');
  await expect(nav.getByRole('link', { name: '通しの確認' })).toHaveAttribute(
    'aria-current',
    'page',
  );
  await nav.getByRole('link', { name: 'この画面について' }).click();
  await expect(page).toHaveURL(/\/about$/);
  await expect(page).toHaveTitle('この画面について - vehicle-sil');
  await expect(page.getByRole('heading', { level: 1, name: 'この画面について' })).toBeVisible();
});
