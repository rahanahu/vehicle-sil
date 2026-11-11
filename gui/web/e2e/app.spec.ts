// 配信と経路の試験（捨てない）。
import { expect, test } from '@playwright/test';
import { guiToken } from './support/gui-server';

const staticOnly = () =>
  test.skip(test.info().project.name !== 'static', 'ビルド済みの画面を配信するときだけ');

test('an unknown path shows the not-found page', async ({ page }) => {
  await page.goto('/no/such/page');
  await expect(page.getByRole('heading', { level: 1, name: '画面が見つからない' })).toBeVisible();
  await expect(page).toHaveTitle('見つからない - vehicle-sil');
});

test('a mistyped API path answers 404, not the app', async ({ request }) => {
  const headers = { Authorization: `Bearer ${guiToken()}` };
  expect((await request.get('/api/helth', { headers })).status()).toBe(404);
});

test('the API refuses a request without the token', async ({ request }) => {
  expect((await request.get('/api/app')).status()).toBe(401);
});

test.describe('static serving', () => {
  test.beforeEach(staticOnly);

  test('runs under a strict Content-Security-Policy', async ({ page }) => {
    const violations: string[] = [];
    page.on('console', (message) => {
      if (/Content Security Policy/.test(message.text())) violations.push(message.text());
    });
    const response = await page.goto('/_test/layout');
    expect(response?.headers()['content-security-policy']).toContain("script-src 'self'");
    await expect(page.getByRole('heading', { level: 1 })).toBeVisible();
    // 全体のスタイルが当たっている（media="print" のまま残っていない）
    const media = await page.evaluate(() =>
      [...document.querySelectorAll('link[rel="stylesheet"]')].map(
        (link) => (link as HTMLLinkElement).media,
      ),
    );
    expect(media.filter((m) => m && m !== 'all')).toEqual([]);
    expect(violations).toEqual([]);
  });

  test('serves the icons with their content types', async ({ page, request }) => {
    await page.goto('/_test/layout');
    await expect(page.locator('meta[name="theme-color"]')).toHaveAttribute('content', '#0f172a');
    await expect(page.locator('html')).toHaveAttribute('lang', 'ja');
    // LOCALE_ID は $localize.locale（ビルドの sourceLocale）から決まる。lang と同じ ja
    expect(
      await page.evaluate(
        () => (globalThis as { $localize?: { locale?: string } }).$localize?.locale,
      ),
    ).toBe('ja');
    await expect(page.locator('link[rel="icon"][type="image/svg+xml"]')).toHaveAttribute(
      'href',
      'favicon.svg',
    );

    const svg = await request.get('/favicon.svg');
    expect(svg.status()).toBe(200);
    expect(svg.headers()['content-type']).toMatch(/^image\/svg\+xml/);
    expect(await svg.text()).toContain('<svg');

    const ico = await request.get('/favicon.ico');
    expect(ico.status()).toBe(200);
    expect(ico.headers()['content-type']).toMatch(/^image\/(vnd\.microsoft\.icon|x-icon)/);
  });

  test('answers 404 for a missing asset instead of the app', async ({ request }) => {
    expect((await request.get('/missing-chunk.js')).status()).toBe(404);
  });
});
