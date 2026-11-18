// Settings of the unit tests of gui/client (`npm run test:client` in gui/web). Two projects:
// - node: the library (everything outside src/elements), on Node.
// - browser: the parts (src/elements), in a real Chromium (headless unless you pass `--browser.headless=false`).
// gui/client has no node_modules of its own: the packages come from gui/web, so `root` is gui/web (where the imports
// of the tests resolve) and `dir` is where the tests are.
import { createRequire } from 'node:module';
import { fileURLToPath, pathToFileURL } from 'node:url';

const fromWeb = createRequire(new URL('../web/package.json', import.meta.url));
const { playwright } = await import(
  pathToFileURL(fromWeb.resolve('@vitest/browser-playwright')).href
);

const web = fileURLToPath(new URL('../web', import.meta.url));
const client = import.meta.dirname;

export default {
  test: {
    projects: [
      {
        test: {
          name: 'node',
          root: web,
          dir: client,
          include: ['src/**/*.spec.ts'],
          exclude: ['src/elements/**'],
          environment: 'node',
          globals: true,
        },
      },
      {
        test: {
          name: 'browser',
          root: web,
          dir: client,
          include: ['src/elements/**/*.spec.ts'],
          globals: true,
          browser: {
            enabled: true,
            headless: true,
            provider: playwright(),
            instances: [{ browser: 'chromium' }],
          },
        },
      },
    ],
  },
};
