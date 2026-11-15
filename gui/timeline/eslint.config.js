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

const TEXT_ONLY =
  'The page puts text in with textContent only: names and messages come from the file.';

/** Text from the file (names, units, messages) must never be parsed as HTML or become a handler. */
const NO_HTML = {
  'no-restricted-properties': [
    'error',
    ...[
      'innerHTML',
      'outerHTML',
      'insertAdjacentHTML',
      'createContextualFragment',
      'setHTMLUnsafe',
      'parseHTMLUnsafe',
      'srcdoc',
      'write',
      'writeln',
    ].map((property) => ({
      ...(property.startsWith('write') ? { object: 'document' } : {}),
      property,
      message: TEXT_ONLY,
    })),
  ],
  'no-restricted-syntax': [
    'error',
    { selector: "NewExpression[callee.name='DOMParser']", message: TEXT_ONLY },
    {
      // h() turns on... keys into listeners; setAttribute must not make a handler or a document of text.
      selector:
        "CallExpression[callee.property.name='setAttribute'][arguments.0.type='Literal'][arguments.0.value=/^(on|srcdoc$)/i]",
      message: TEXT_ONLY,
    },
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
