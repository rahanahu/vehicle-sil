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
  'history',
  'navigator',
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

/**
 * Text that the page shows comes from messages.js, in English and Japanese. A string of words (two words of letters,
 * or a capitalized word as an element's text or title) in viewer.js is such a text: it goes in the dictionary.
 * Class names (`class:`) and the 'use strict' directive are not.
 */
const IN_DICTIONARY =
  'Text that the page shows goes in messages.js (English and Japanese): use t(<key>).';
const WORDS = '/[A-Za-z]{2,}\\s+[A-Za-z]{2,}/';
const NO_TEXT = [
  {
    selector: `Literal[value=${WORDS}]:not(Property[key.name='class'] > Literal):not(ExpressionStatement > Literal)`,
    message: IN_DICTIONARY,
  },
  {
    selector: `TemplateElement[value.raw=${WORDS}]:not(Property[key.name='class'] > TemplateLiteral > TemplateElement)`,
    message: IN_DICTIONARY,
  },
  {
    selector: 'Property[key.name=/^(text|title)$/] > Literal[value=/[A-Za-z]{2,}/]',
    message: IN_DICTIONARY,
  },
  {
    selector: "CallExpression[callee.name='h'] > Literal[value=/^[A-Z][a-z]/]",
    message: IN_DICTIONARY,
  },
];

module.exports = defineConfig([
  {
    files: ['viewer.js'],
    extends: [eslint.configs.recommended],
    languageOptions: { sourceType: 'script', globals: globals([...BROWSER, ...NODE, ...SHARED]) },
    rules: {
      ...NO_HTML,
      'no-restricted-syntax': [...NO_HTML['no-restricted-syntax'], ...NO_TEXT],
    },
  },
  {
    files: ['tests/**/*.js', 'eslint.config.js'],
    extends: [eslint.configs.recommended],
    languageOptions: { sourceType: 'commonjs', globals: globals([...NODE, ...SHARED, 'Buffer']) },
  },
]);
