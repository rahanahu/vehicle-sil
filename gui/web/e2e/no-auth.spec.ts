// Verifies --no-auth end-to-end across the Standard GUI (gui/web), Timeline (gui/timeline), and
// Web Components starter (examples/gui_starter) without #token=.
import { expect, test } from '@playwright/test';
import { existsSync } from 'node:fs';
import { join } from 'node:path';
import {
  CONFIGS,
  GuiServer,
  REPLAY_LAUNCHER,
  REPOSITORY,
  launchGuiServer,
} from './support/gui-server';
import { RUN_TIMEOUT, inSync } from './support/run-page';

test.describe('GUI server --no-auth', () => {
  test.beforeEach(() => {
    test.skip(test.info().project.name !== 'static', 'only with the built screen');
  });

  test('standard GUI and timeline work without #token= under --no-auth', async ({
    page,
    browser,
  }) => {
    const server: GuiServer = await launchGuiServer({
      launcher: REPLAY_LAUNCHER,
      root: CONFIGS,
      config: join(CONFIGS, 'simulation.yaml'),
      args: ['--no-auth'],
      env: { REPLAY_TICK_MS: '10' },
      port: 0,
    });
    try {
      expect(server.url).not.toContain('#token=');
      expect(server.token).toBe('');

      await page.goto(server.url);
      await inSync(page);
      await expect(page.getByTestId('no-token')).toHaveCount(0);

      await page.getByRole('combobox', { name: 'シナリオ' }).click();
      await page.getByRole('option', { name: 'fails.yaml', exact: true }).click();
      await expect(page.getByTestId('check-scenario')).toContainText('scenarios/fails.yaml');
      const start = page.getByTestId('start');
      await expect(start).toHaveAttribute('data-armed', 'true');
      await start.click();

      const card = page.getByTestId('scenario-card');
      await expect(card.getByTestId('scenario-outcome')).toHaveText(/シナリオは失敗しました/, {
        timeout: RUN_TIMEOUT,
      });
      const href = (await card.getByTestId('scenario-timeline').getAttribute('href')) ?? '';
      expect(href).toMatch(/^timeline\/\?src=%2Fapi%2Ftimeline%2F[^&]+&lang=ja$/);

      // Fresh browser context (no sessionStorage, no #token=): timeline loads directly.
      const fresh = await browser.newPage();
      try {
        await fresh.goto(new URL(href, server.url).href);
        await expect(fresh.locator('vehicle-sil-timeline')).toBeVisible();
        await expect(fresh.locator('#load')).toBeHidden({ timeout: RUN_TIMEOUT });
        await expect(fresh.locator('#page-error')).toBeHidden();
      } finally {
        await fresh.close();
      }
    } finally {
      await server.stop();
    }
  });

  test('web components starter (examples/gui_starter) works without #token= under --no-auth', async ({
    page,
  }) => {
    const buildDir = join(REPOSITORY, 'build', 'gui-client');
    for (const name of ['vehicle-sil-gui-client.mjs', 'vehicle-sil-gui-elements.mjs']) {
      if (!existsSync(join(buildDir, name))) {
        throw new Error(`${join(buildDir, name)} is missing: run npm run build:client in gui/web`);
      }
    }
    const server: GuiServer = await launchGuiServer({
      launcher: REPLAY_LAUNCHER,
      root: CONFIGS,
      config: join(CONFIGS, 'simulation.yaml'),
      args: ['--no-auth'],
      env: { REPLAY_TICK_MS: '10' },
      port: 0,
      web: join(REPOSITORY, 'examples', 'gui_starter'),
    });
    try {
      expect(server.url).not.toContain('#token=');
      await page.goto(server.url);
      const status = page.locator('vsil-status');
      await expect(status).toHaveAttribute('data-link', 'online', { timeout: RUN_TIMEOUT });
      await expect(page.locator('[data-error]')).toHaveCount(0);
      const start = page.locator('vsil-run-controls button', { hasText: 'Start' });
      await expect(start).toHaveAttribute('aria-disabled', 'false', { timeout: RUN_TIMEOUT });
      await start.click();
      await expect(status).toHaveAttribute('data-run', 'running', { timeout: RUN_TIMEOUT });
      const stop = page.locator('vsil-run-controls button', { hasText: 'Stop' }).first();
      await expect(stop).toHaveAttribute('aria-disabled', 'false', { timeout: RUN_TIMEOUT });
      await stop.click();
      await expect(status).toHaveAttribute('data-run', 'ended', { timeout: RUN_TIMEOUT });
    } finally {
      await server.stop();
    }
  });
});
