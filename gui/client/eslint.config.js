// @ts-check
// gui/client の lint。gui/client は依存を持たないので、道具（ESLint とその規則）は gui/web の node_modules から借りる。
// gui/web で `npm run lint` が、これも回す。
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
      // 枠組み（Angular、RxJS など）にも、ほかのパッケージにも依らない: import は自分のファイルだけ
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
      // ブラウザの外（Node）でも動く: ブラウザにしかないものを使わない
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
      // 動的な import() も、ほかのパッケージを読まない（no-restricted-imports は動的な import() を見ない）
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
    // 試験の部品は、Node の組み込み（node:fs）を実行のときに読む
    files: ['src/testing/**/*.ts'],
    rules: { 'no-restricted-syntax': 'off' },
  },
]);
