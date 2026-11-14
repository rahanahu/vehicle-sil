// シェルのレイアウトを、幅を変えて確かめる。中身は試験用の画面（/_test/layout）で、捨てる画面には頼らない。
// 画面を足したら ROUTES に足す（その画面も、すべての幅で崩れないことを確かめる）。
import { Page, expect, test } from '@playwright/test';
import { guiToken } from './support/gui-server';
import { VIEWPORTS, Viewport, findLayoutProblems } from './support/layout';

/**
 * 確かめる経路と、その画面の見出し。最初のもの（中身を固定した試験用の画面）は、ほかの試験も使う。
 * 実行の画面は、いろいろな状態での崩れを run.spec.ts でも確かめる。
 */
const ROUTES = [
  { path: '_test/layout', heading: 'レイアウトの試験', english: 'Layout test' },
  { path: '', heading: '実行', english: 'Run' },
  { path: 'variables', heading: '変数', english: 'Variables' },
];

/** スクリーンショットを残す幅。 */
const SCREENSHOT_WIDTHS = [360, 480, 1024, 1920];

async function open(page: Page, path: string, heading: string) {
  // サーバーが表示する URL と同じく、トークンを付けて開く
  await page.goto(`/${path}#token=${guiToken()}`);
  await expect(page.getByRole('heading', { level: 1, name: heading })).toBeVisible();
}

for (const viewport of VIEWPORTS) {
  test.describe(`${viewport.width}x${viewport.height}`, () => {
    test.use({ viewport: { width: viewport.width, height: viewport.height } });

    for (const route of ROUTES) {
      test(`${route.path} lays out without overflow or overlap`, async ({ page }, testInfo) => {
        await open(page, route.path, route.heading);
        const nav = page.getByTestId('nav');
        await expect(nav).toHaveAttribute('data-nav-mode', viewport.navMode);
        await expect(page.getByTestId('app-title')).toBeVisible();
        expect(await findLayoutProblems(page)).toEqual([]);

        if (SCREENSHOT_WIDTHS.includes(viewport.width)) {
          await page.screenshot({
            path: testInfo.outputPath(
              `layout-${route.path.replace(/\W+/g, '-') || 'run'}-${viewport.width}.png`,
            ),
            fullPage: true,
          });
        }
      });
    }

    test('the navigation follows the width and the keyboard', async ({ page }) => {
      await open(page, ROUTES[0].path, ROUTES[0].heading);
      await checkNavigation(page, viewport);
    });
  });
}

// 英語の文は長い。英語でも、すべての幅で崩れない
test.describe('in English', () => {
  test.use({ locale: 'en-US' });

  for (const viewport of VIEWPORTS) {
    for (const route of ROUTES) {
      test(`${viewport.width}x${viewport.height}: ${route.path} lays out without overflow or overlap`, async ({
        page,
      }, testInfo) => {
        await page.setViewportSize({ width: viewport.width, height: viewport.height });
        await open(page, route.path, route.english);
        await expect(page.getByTestId('nav')).toHaveAttribute('data-nav-mode', viewport.navMode);
        await expect(page.getByTestId('language-menu')).toBeInViewport({ ratio: 1 });
        expect(await findLayoutProblems(page)).toEqual([]);
        if (SCREENSHOT_WIDTHS.includes(viewport.width)) {
          await page.screenshot({
            path: testInfo.outputPath(
              `layout-en-${route.path.replace(/\W+/g, '-') || 'run'}-${viewport.width}.png`,
            ),
            fullPage: true,
          });
        }
      });
    }
  }
});

async function checkNavigation(page: Page, viewport: Viewport) {
  const nav = page.getByTestId('nav');
  const toggle = page.getByTestId('nav-toggle');
  const firstLink = nav.getByRole('link').first();

  if (viewport.navMode === 'drawer') {
    await expect(nav).toBeHidden();
    await expect(toggle).toHaveAttribute('aria-expanded', 'false');
    await toggle.click();
    await expect(nav).toBeVisible();
    await expect(toggle).toHaveAttribute('aria-expanded', 'true');
    // Esc で閉じ、フォーカスは開くボタンに戻る
    await firstLink.focus();
    await page.keyboard.press('Escape');
    await expect(nav).toBeHidden();
    await expect(toggle).toBeFocused();
    await expect(toggle).toHaveAttribute('aria-expanded', 'false');
    return;
  }

  await expect(toggle).toHaveCount(0);
  await expect(nav).toBeVisible();
  const label = nav.getByText((await firstLink.getAttribute('aria-label')) ?? '');
  if (viewport.navMode === 'rail') {
    await expect(label).toHaveCount(0);
    expect((await nav.boundingBox())?.width).toBeLessThan(120);
  } else {
    await expect(label).toBeVisible();
  }
  // Esc を押してもナビゲーションは消えない
  await firstLink.focus();
  await page.keyboard.press('Escape');
  await page.waitForTimeout(300);
  await expect(nav).toBeVisible();
  await expect(nav).toHaveAttribute('data-nav-mode', viewport.navMode);
}

test('the content does not get narrower when the window gets wider', async ({ page }) => {
  const widths: number[] = [];
  for (const viewport of VIEWPORTS) {
    await page.setViewportSize({ width: viewport.width, height: viewport.height });
    await open(page, ROUTES[0].path, ROUTES[0].heading);
    await expect(page.getByTestId('nav')).toHaveAttribute('data-nav-mode', viewport.navMode);
    widths.push((await page.getByTestId('content').boundingBox())?.width ?? 0);
  }
  for (let i = 1; i < widths.length; i++) {
    expect(
      widths[i],
      `content at ${VIEWPORTS[i].width}px vs ${VIEWPORTS[i - 1].width}px`,
    ).toBeGreaterThanOrEqual(widths[i - 1] - 1);
  }
});

test('resizing between rail and full keeps the content beside the navigation', async ({ page }) => {
  await page.setViewportSize({ width: 1024, height: 768 });
  await open(page, ROUTES[0].path, ROUTES[0].heading);
  for (const [width, mode] of [
    [1920, 'full'],
    [1024, 'rail'],
    [1300, 'full'],
  ] as const) {
    await page.setViewportSize({ width, height: 800 });
    await expect(page.getByTestId('nav')).toHaveAttribute('data-nav-mode', mode);
    await expect.poll(() => findLayoutProblems(page)).toEqual([]);
  }
});

test.describe('the checks catch a broken layout', () => {
  test.use({ viewport: { width: 480, height: 720 } });

  test('a fixed-width block in the content', async ({ page }) => {
    await open(page, ROUTES[0].path, ROUTES[0].heading);
    await page.evaluate(() => {
      const block = document.createElement('div');
      block.style.width = '900px';
      block.textContent = 'fixed';
      document.querySelector('main')?.append(block);
    });
    const problems = await findLayoutProblems(page);
    expect(problems.join('\n')).toMatch(/sticks out of <main>/);
    expect(problems.join('\n')).toMatch(/scrolls sideways/);
  });

  test('a long word inside a card', async ({ page }) => {
    await open(page, ROUTES[0].path, ROUTES[0].heading);
    await page.evaluate(() => {
      const word = document.createElement('span');
      word.style.whiteSpace = 'nowrap';
      word.textContent = 'X'.repeat(120);
      document.querySelector('main mat-card mat-card-content')?.append(word);
    });
    expect((await findLayoutProblems(page)).join('\n')).toMatch(/sticks out of <mat-card/);
  });
});

test('follows the dark color scheme', async ({ page }, testInfo) => {
  await page.emulateMedia({ colorScheme: 'dark' });
  await open(page, ROUTES[0].path, ROUTES[0].heading);
  const [background, text] = await page.evaluate(() => {
    const style = getComputedStyle(document.body);
    return [style.backgroundColor, style.color];
  });
  // 背景が暗く、文字が明るい
  const luminance = (rgb: string) => {
    const [r, g, b] = (rgb.match(/\d+(\.\d+)?/g) ?? []).map(Number);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
  };
  expect(luminance(background)).toBeLessThan(80);
  expect(luminance(text)).toBeGreaterThan(170);
  await page.screenshot({ path: testInfo.outputPath('layout-dark.png') });
});
