// @ts-check
// Lint for gui/client. gui/client has no dependencies, so it borrows the tools (ESLint and its rules) from
// gui/web's node_modules. `npm run lint` in gui/web runs this too.
const { createRequire } = require('node:module');
const fromWeb = createRequire(require.resolve('../web/package.json'));
const eslint = fromWeb('@eslint/js');
const { defineConfig } = fromWeb('eslint/config');
const tseslint = fromWeb('typescript-eslint');

module.exports = defineConfig([
  {
    files: ['**/*.ts'],
    extends: [eslint.configs.recommended, tseslint.configs.recommended, tseslint.configs.stylistic],
    rules: {
      // No dependency on a framework (Angular, RxJS and so on) or on any other package: import only our own files
      'no-restricted-imports': [
        'error',
        {
          patterns: [
            {
              regex: '^[^.]',
              message:
                'gui/client depends on nothing: import only its own files (a relative path). Framework glue belongs in the app (gui/web/src/app/core/).',
            },
          ],
        },
      ],
      // Runs outside the browser (Node) too: use nothing that only browsers have
      'no-restricted-globals': [
        'error',
        ...[
          'window',
          'document',
          'location',
          'history',
          'navigator',
          'localStorage',
          'sessionStorage',
          'indexedDB',
          'customElements',
          'HTMLElement',
          'requestAnimationFrame',
          'alert',
        ].map((name) => ({
          name,
          message: `gui/client also runs outside a browser (Node): do not use ${name}; take it as an option.`,
        })),
      ],
      // Dynamic import() must not load other packages either (no-restricted-imports does not see dynamic import())
      'no-restricted-syntax': [
        'error',
        {
          selector: 'ImportExpression',
          message:
            'gui/client depends on nothing: no dynamic import() outside src/testing/ (the test helpers read node:fs)',
        },
      ],
    },
  },
  {
    // The parts are custom elements: they need the DOM. Nothing outside src/elements imports them, so Node never loads them.
    files: ['src/elements/**/*.ts'],
    rules: { 'no-restricted-globals': 'off' },
  },
  {
    // The tests of the parts run in a browser and use its test API (vitest/browser); the parts themselves import nothing
    files: ['src/elements/**/*.spec.ts'],
    rules: { 'no-restricted-imports': 'off' },
  },
  {
    // The test helpers load Node built-ins (node:fs) at run time
    files: ['src/testing/**/*.ts'],
    rules: { 'no-restricted-syntax': 'off' },
  },
]);
