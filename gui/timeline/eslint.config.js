// @ts-check
// Lint for gui/timeline (the timeline page). It has no dependencies, so it borrows ESLint from gui/web's
// node_modules. `npm run lint` in gui/web runs this too.
const { createRequire } = require('node:module');
const fromWeb = createRequire(require.resolve('../web/package.json'));
const eslint = fromWeb('@eslint/js');
const { defineConfig } = fromWeb('eslint/config');

const BROWSER = [
  'window',
  'document',
  'location',
  'fetch',
  'URL',
  'URLSearchParams',
  'ResizeObserver',
  'Buffer',
];
const NODE = ['module', 'require', '__dirname'];
const SHARED = ['globalThis', 'console'];
const globals = (names) => Object.fromEntries(names.map((name) => [name, 'readonly']));

/** Text from the file (names, units, messages) must never be parsed as HTML. */
const NO_HTML = {
  'no-restricted-properties': [
    'error',
    ...['innerHTML', 'outerHTML', 'insertAdjacentHTML', 'write', 'writeln'].map((property) => ({
      ...(property.startsWith('write') ? { object: 'document' } : {}),
      property,
      message:
        'The page puts text in with textContent only: names and messages come from the file.',
    })),
  ],
};

module.exports = defineConfig([
  {
    files: ['viewer.js'],
    extends: [eslint.configs.recommended],
    languageOptions: { sourceType: 'script', globals: globals([...BROWSER, ...NODE, ...SHARED]) },
    rules: NO_HTML,
  },
  {
    files: ['tests/**/*.js', 'eslint.config.js'],
    extends: [eslint.configs.recommended],
    languageOptions: { sourceType: 'commonjs', globals: globals([...NODE, ...SHARED, 'Buffer']) },
  },
]);
