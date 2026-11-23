// Worker-scoped GUI server fixture for Playwright static mode (workers > 1).
import { test as base, expect } from '@playwright/test';
import { CONFIGS, GuiServer, REPLAY_LAUNCHER, launchGuiServer } from './gui-server';

interface WorkerFixtures {
  workerServer: { baseUrl: string; token: string } | null;
}

export const test = base.extend<Record<never, never>, WorkerFixtures>({
  workerServer: [
    // eslint-disable-next-line no-empty-pattern
    async ({}, use, workerInfo) => {
      if (workerInfo.project.name !== 'static') {
        await use(null);
        return;
      }
      const server: GuiServer = await launchGuiServer({
        launcher: REPLAY_LAUNCHER,
        root: CONFIGS,
        env: { REPLAY_TICK_MS: '20' },
        port: 0,
      });
      process.env['VEHICLE_SIL_GUI_E2E_TOKEN'] = server.token;
      try {
        await use({ baseUrl: new URL('/', server.url).href, token: server.token });
      } finally {
        delete process.env['VEHICLE_SIL_GUI_E2E_TOKEN'];
        await server.stop();
      }
    },
    { scope: 'worker' },
  ],
  baseURL: async ({ workerServer, baseURL }, use) => {
    await use(workerServer?.baseUrl ?? baseURL);
  },
});

export { expect };
export type { Locator, Page, WebSocketRoute } from '@playwright/test';
