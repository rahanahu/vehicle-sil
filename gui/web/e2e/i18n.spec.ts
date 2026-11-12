// 画面の言語: ブラウザの言語で英語になること、言語のメニューで変えて覚えること、実行中に変えても実行が続くこと、
// 訳がない文は日本語の元の文で出ること。英語の幅ごとの崩れは run.spec.ts（the screen at every width, in English）と
// layout.spec.ts が確かめる。
import { Page, expect, test } from '@playwright/test';
import {
  RUN_TIMEOUT,
  WORDS,
  choose,
  chosenFile,
  openRun,
  press,
  result,
  serverSession,
  start,
  startButton,
  stopButton,
  stopIfRunning,
  stopOnServer,
} from './support/run-page';

test.use({ locale: 'en-US' });

test.beforeEach(async ({ page }) => {
  await openRun(page, 'en');
  await stopIfRunning(page, 'en');
});

/** 言語のメニューで言語を選び、読み込み直した画面を待つ。 */
async function switchTo(page: Page, name: '日本語' | 'English'): Promise<void> {
  await page.getByTestId('language-menu').click();
  const loaded = page.waitForEvent('load');
  await page.getByRole('menuitemradio', { name }).click();
  await loaded;
  const language = name === 'English' ? 'en' : 'ja';
  await expect(page.locator('html')).toHaveAttribute('lang', language);
  await expect(page.getByRole('heading', { level: 1, name: WORDS[language].run })).toBeVisible();
}

test('the run screen is in English: words, the status band, findings as they are', async ({
  page,
}) => {
  await expect(page.locator('html')).toHaveAttribute('lang', 'en');
  const menu = page.getByTestId('language-menu');
  await expect(menu).toHaveAccessibleName('Language: English');
  await expect(menu).toHaveText(/English/);

  // 照合: 所見の文はランチャーの英語のまま
  await choose(page, 'check-error.yaml', 'en');
  await expect(page.getByTestId('check-outcome')).toHaveText(
    /Check: Has errors \(errors: \d+, warnings: \d+, notes: \d+\)/,
  );
  const finding = page.getByTestId('check-card').getByTestId('finding').first();
  await expect(finding).toContainText('step-mismatch');
  await expect(finding).toContainText("step_ms is 20, but the executable's step is 10 ms");
  await expect(page.getByTestId('blocked')).toContainText('Fix them before you start.');
  await expect(startButton(page)).toHaveText(/Start/);
  await expect(stopButton(page)).toHaveText(/Stop/);

  // 実行: 状態の帯、段階、結果、ログの行数（複数形）
  await choose(page, 'simulation.yaml', 'en');
  await expect(page.getByTestId('check-outcome')).toContainText('Check: Passed');
  const session = await start(page);
  await expect(result(page, session)).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(result(page, session).getByTestId('result')).toContainText('Passed');
  await expect(page.getByTestId('outcome')).toContainText('Passed');
  await expect(
    page.getByRole('list', { name: 'Phase' }).locator('[aria-current="step"]'),
  ).toHaveText(/Ended/);
  await expect(page.getByTestId('status-Engine')).toContainText('Ended');
  await expect(page.getByTestId('sim-time')).toHaveText(/^\d+\.\d{3} s$/);
  await expect(page.getByTestId('log-count')).toHaveText(/^\s*\d+ lines?\b/);
  await expect(startButton(page)).toHaveText(/Run again/);
  // 日本語の文字が残っていない（participant の名前とログはランチャーのもの）
  const text = (await page.locator('app-run-status').innerText()) ?? '';
  expect(text).not.toMatch(/[぀-ヿ一-鿿]/);
});

test('the dialog to stop without waiting is in English', async ({ page }) => {
  await choose(page, 'ignore-stop.yaml', 'en');
  await start(page);
  await expect(page.getByTestId('log-line').first()).toBeVisible({ timeout: RUN_TIMEOUT });
  await press(stopButton(page));
  await expect(stopButton(page)).toHaveText(/Stopping…/);
  const force = page.getByTestId('force-stop');
  await expect(force).toHaveText(/Stop without waiting…/);
  await press(force);
  const dialog = page.getByRole('dialog');
  await expect(dialog).toContainText('Stop without waiting?');
  await expect(dialog).toContainText('may be lost');
  await expect(dialog.getByRole('button', { name: 'Cancel' })).toBeFocused();
  await expect(dialog.getByRole('button', { name: 'Stop without waiting' })).toBeVisible();
  await page.keyboard.press('Enter');
  await expect(dialog).toBeHidden();
  await stopOnServer(page);
});

test('the choice is remembered, and wins over the browser language', async ({ page }) => {
  await switchTo(page, '日本語');
  // 読み込み直した後も、フォーカスは言語のメニューにある
  await expect(page.getByTestId('language-menu')).toBeFocused();
  await expect(page.getByTestId('language-menu')).toHaveAccessibleName('言語（Language）: 日本語');
  expect(await page.evaluate(() => localStorage.getItem('vehicle-sil-gui-language'))).toBe('ja');
  // もう一度読み込んでも日本語（ブラウザの言語は英語のまま）
  await page.reload();
  await expect(page.getByRole('heading', { level: 1, name: '実行' })).toBeVisible();
  await expect(page.locator('html')).toHaveAttribute('lang', 'ja');
  // メニューは、どちらの言語でも同じ名前（その言語自身の文字）で選べる
  await page.getByTestId('language-menu').click();
  await expect(page.getByRole('menuitemradio', { name: '日本語' })).toHaveAttribute(
    'aria-checked',
    'true',
  );
  await expect(page.getByRole('menuitemradio', { name: 'English' })).toHaveAttribute('lang', 'en');
  await page.keyboard.press('Escape');
  await switchTo(page, 'English');
  expect(await page.evaluate(() => localStorage.getItem('vehicle-sil-gui-language'))).toBe('en');
});

test('switching the language during a run keeps the run and the choices', async ({ page }) => {
  await choose(page, 'forever.yaml', 'en');
  const session = await start(page);
  await expect(page.getByTestId('log-line').nth(3)).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('outcome')).toContainText('Running');

  await switchTo(page, '日本語');
  // 実行はサーバーで続いていて、画面はそれを映す
  const after = await serverSession(page);
  expect(after?.id).toBe(session);
  expect(after?.state).toBe('running');
  await expect(page.getByTestId('outcome')).toContainText('実行中');
  await expect(page.getByTestId('session-file')).toHaveText('forever.yaml');
  await expect(page.getByTestId('log-line').first()).toBeVisible();
  await expect(stopButton(page)).not.toHaveAttribute('aria-disabled', 'true');
  // 画面から止められる
  await press(stopButton(page));
  await expect(result(page, session)).toBeVisible({ timeout: RUN_TIMEOUT });

  // まだ送っていない選択（ファイル、外した participant）も持ち越す
  await choose(page, 'simulation.yaml', 'ja');
  const logger = page.getByTestId('check-participant-Logger').getByRole('switch');
  await logger.click();
  await expect(logger).not.toBeChecked();
  await switchTo(page, 'English');
  await expect(chosenFile(page, 'en')).toHaveText('simulation.yaml');
  await expect(page.getByTestId('check-participant-Logger').getByRole('switch')).not.toBeChecked();
  await expect(page.getByTestId('check-participant-Engine').getByRole('switch')).toBeChecked();
});

test('a message without a translation is shown in Japanese, not as an id', async ({ page }) => {
  // 開始のボタンの訳を「まだ訳していない」に、構成ファイルの見出しの訳を空にし、ほかの訳は残す
  await page.route('**/locale/messages.en.json', async (route) => {
    const response = await route.fetch();
    const file = (await response.json()) as {
      translations: Record<string, { source: string; target: string | null }>;
    };
    for (const entry of Object.values(file.translations)) {
      if (entry.source === '開始' || entry.source === 'もう一度実行') entry.target = null;
      if (entry.source === '構成ファイル') entry.target = '';
    }
    await route.fulfill({ response, json: file });
  });
  // 読み込み直す（トークンは sessionStorage にある）
  await page.reload();
  await expect(page.getByRole('heading', { level: 1, name: 'Run' })).toBeVisible();
  await expect(startButton(page)).toHaveText(/開始|もう一度実行/);
  await expect(page.getByRole('heading', { level: 2, name: '構成ファイル' })).toBeVisible();
  await expect(page.getByTestId('language-menu')).toHaveAccessibleName('Language: English');
});

test('without the translation file the screen is in Japanese', async ({ page }) => {
  await page.route('**/locale/messages.en.json', (route) => route.fulfill({ status: 404 }));
  await page.reload();
  await expect(page.getByRole('heading', { level: 1, name: '実行' })).toBeVisible();
  await expect(page.locator('html')).toHaveAttribute('lang', 'ja');
});
