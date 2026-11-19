// @ts-check
// Enforces the rules of how to write the code ("Coding rules" in README.md) with lint.
const eslint = require('@eslint/js');
const { defineConfig } = require('eslint/config');
const tseslint = require('typescript-eslint');
const angular = require('angular-eslint');

// Lint does not decide where to use RxJS ("Coding rules" in README.md decides).
// Lint holds only the rules that are wrong wherever they are broken.

/** gui/client のソースを相対パスで import しない（入口 @vehicle-sil/gui-client から使う）。 */
const CLIENT_SOURCES = {
  group: ['**/client/src', '**/client/src/**'],
  message: 'import from @vehicle-sil/gui-client (or @vehicle-sil/gui-client/testing in tests)',
};

/** 本番のコードは、ライブラリの公開の入口だけを使う。 */
const PUBLIC_ENTRY_ONLY = {
  group: ['@vehicle-sil/gui-client/internal', '@vehicle-sil/gui-client/testing'],
  message:
    'the app uses only @vehicle-sil/gui-client: internal is outside the compatibility promise and testing is for tests (add what is missing to gui/client/src/index.ts)',
};

/** 写して使える部品と包みが頼ってはいけないもの: 画面とアプリの殻。 */
const PORTABLE_PARTS = {
  group: [
    '**/run',
    '**/run/**',
    '**/variables',
    '**/variables/**',
    '**/not-found/**',
    '**/app',
    '**/app.*',
    '**/navigation',
    '**/testing/**',
  ],
  message:
    'widgets/ and core/ are copied into other Angular apps: they must not import the pages (run/, variables/) or the app shell',
};

const FRONT_DOOR_MESSAGE =
  'Do not read this low-level session field in a screen: it copies a rule of the launcher. Take "may I do this, and if not why" from the front door (`controls`, `status`). If you only show it, add this file to DISPLAY_ONLY_FILES in eslint.config.js with the reason in the PR.';

/** The low-level session fields a screen must not read (`launcher.running` is added separately). */
const LOW_LEVEL_FIELDS = ['stopRequests', 'runOutcome', 'features'];

/** The same reads in TypeScript (ESTree) and in templates (Angular expression AST). */
const FRONT_DOOR_TS = [
  ...LOW_LEVEL_FIELDS.map((n) => `MemberExpression[property.name='${n}']`),
  "MemberExpression[property.name='running'][object.property.name='launcher']",
  "MemberExpression[property.name='running'][object.name='launcher']",
].map((selector) => ({ selector, message: FRONT_DOOR_MESSAGE }));

const FRONT_DOOR_HTML = [
  ...LOW_LEVEL_FIELDS.flatMap((n) => [
    `PropertyRead[name='${n}']`,
    `SafePropertyRead[name='${n}']`,
  ]),
  "PropertyRead[name='running'][receiver.name='launcher']",
  "SafePropertyRead[name='running'][receiver.name='launcher']",
].map((selector) => ({ selector, message: FRONT_DOOR_MESSAGE }));

// Display only: these files read the fields to show something the front door has no value for
// (the stopped-scenario distinction, a launcher still running after the result, the number of
// stop requests, the paused band). A new file that needs one of these is added here with a reason in the PR.
const DISPLAY_ONLY_FILES = [
  'src/app/run/run-status.ts',
  'src/app/run/run-session.ts',
  'src/app/core/session/simulation-time.ts',
  'src/app/run/run-page.ts',
  'src/app/run/run-page.html',
];

module.exports = defineConfig([
  {
    files: ['**/*.ts'],
    extends: [
      eslint.configs.recommended,
      tseslint.configs.recommended,
      tseslint.configs.stylistic,
      angular.configs.tsRecommended,
    ],
    processor: angular.processInlineTemplates,
    rules: {
      '@angular-eslint/directive-selector': [
        'error',
        { type: 'attribute', prefix: 'app', style: 'camelCase' },
      ],
      '@angular-eslint/component-selector': [
        'error',
        { type: 'element', prefix: 'app', style: 'kebab-case' },
      ],
      // standalone、inject()、OnPush（推奨の設定にあるものも、警告でなく誤りにする）
      '@angular-eslint/prefer-standalone': 'error',
      '@angular-eslint/prefer-inject': 'error',
      '@angular-eslint/inject-at-top': 'error',
      '@angular-eslint/no-forward-ref': 'error',
      '@angular-eslint/no-attribute-decorator': 'error',
      '@angular-eslint/prefer-on-push-component-change-detection': 'error',
      '@angular-eslint/use-component-view-encapsulation': 'error',
      // デコレーターでなく signal の API（input()、output()、model()、viewChild() など）
      '@angular-eslint/prefer-signals': 'error',
      '@angular-eslint/prefer-signal-model': 'error',
      '@angular-eslint/prefer-output-emitter-ref': 'error',
      '@angular-eslint/prefer-output-readonly': 'error',
      '@angular-eslint/prefer-host-metadata-property': 'error',
      '@angular-eslint/no-queries-metadata-property': 'error',
      '@angular-eslint/computed-must-return': 'error',
      // ライフサイクルの誤用
      '@angular-eslint/no-async-lifecycle-method': 'error',
      '@angular-eslint/no-lifecycle-call': 'error',
      '@angular-eslint/no-implicit-take-until-destroyed': 'error',
      // クライアントのライブラリ（gui/client）は入口からだけ使う（公開の一覧にないものに頼らない）
      'no-restricted-imports': ['error', { patterns: [CLIENT_SOURCES] }],
    },
  },
  {
    // 本番のコードは、ライブラリの公開の入口だけを使う（internal は互換の約束の外、testing は試験の部品）
    files: ['src/**/*.ts'],
    ignores: ['src/**/*.spec.ts'],
    rules: {
      'no-restricted-imports': ['error', { patterns: [CLIENT_SOURCES, PUBLIC_ENTRY_ONLY] }],
    },
  },
  {
    // A screen takes its decisions from the front door, not from the launcher's low-level fields
    files: ['src/**/*.ts'],
    ignores: ['src/**/*.spec.ts', ...DISPLAY_ONLY_FILES],
    rules: { 'no-restricted-syntax': ['error', ...FRONT_DOOR_TS] },
  },
  {
    // The widgets (widgets/) and the wrappers (core/) do not depend on the screens (run/, variables/) or the app shell,
    // so that they stay usable on their own
    files: ['src/app/widgets/**/*.ts', 'src/app/core/**/*.ts'],
    ignores: ['src/**/*.spec.ts'],
    rules: {
      'no-restricted-imports': [
        'error',
        { patterns: [CLIENT_SOURCES, PUBLIC_ENTRY_ONLY, PORTABLE_PARTS] },
      ],
    },
  },
  {
    files: ['src/app/core/**/*.ts'],
    ignores: ['src/**/*.spec.ts'],
    rules: {
      'no-restricted-imports': [
        'error',
        {
          patterns: [
            CLIENT_SOURCES,
            PUBLIC_ENTRY_ONLY,
            PORTABLE_PARTS,
            {
              group: ['**/widgets', '**/widgets/**'],
              message: 'core/ is under the widgets: it does not import them',
            },
          ],
        },
      ],
    },
  },
  {
    // 型の情報が要る規則（src だけ。tsconfig.app.json と tsconfig.spec.json の対象）
    files: ['src/**/*.ts'],
    languageOptions: {
      parserOptions: { projectService: true, tsconfigRootDir: __dirname },
    },
    rules: {
      '@angular-eslint/no-uncalled-signals': 'error',
      '@angular-eslint/reactive-context-must-read-signal': 'error',
      // 実験的、開発者プレビューの API を土台にしない
      '@angular-eslint/no-developer-preview': 'error',
      '@angular-eslint/no-experimental': 'error',
    },
  },
  {
    files: ['**/*.html'],
    ignores: ['src/index.html'],
    extends: [angular.configs.templateRecommended, angular.configs.templateAccessibility],
    rules: {
      '@angular-eslint/template/prefer-control-flow': 'error',
      '@angular-eslint/template/prefer-at-empty': 'error',
      '@angular-eslint/template/prefer-at-else': 'error',
      '@angular-eslint/template/prefer-contextual-for-variables': 'error',
      '@angular-eslint/template/no-empty-control-flow': 'error',
      '@angular-eslint/template/prefer-self-closing-tags': 'error',
      '@angular-eslint/template/no-any': 'error',
      '@angular-eslint/template/button-has-type': 'error',
      '@angular-eslint/template/prefer-ngsrc': 'error',
      // 画面の言葉には i18n の印を付ける（英語を後から足せるように）。id は抽出のときに作られるものを使う
      '@angular-eslint/template/i18n': [
        'error',
        {
          checkId: false,
          checkAttributes: true,
          ignoreAttributes: [
            'aria-controls',
            'data-testid',
            'data-nav-mode',
            'appearance',
            'mat-list-item',
            'matListItemIcon',
            'matListItemTitle',
            'matIconButton',
            'preserveAspectRatio',
            'fill-rule',
            'd',
            'points',
            'ariaCurrentWhenActive',
            'disableClose',
            'scope',
            'data-layout-item',
            'data-scroll-x',
            'routerLinkActive',
            'routerLink',
            'role',
            'type',
            'id',
            'class',
            'viewBox',
            'width',
            'height',
            'fill',
            'aria-hidden',
            'autosize',
            'mode',
            'matButton',
            'subscriptSizing',
            'inputmode',
            'aria-labelledby',
            'aria-current',
            'xPosition',
            'rel',
          ],
        },
      ],
      // テンプレートが読むのは signal。Observable は toSignal で signal にしてから読む（async パイプを使わない）
      'no-restricted-syntax': [
        'error',
        {
          selector: "BindingPipe[name='async']",
          message: 'async パイプを使わない。toSignal で signal にして読む。',
        },
        ...FRONT_DOOR_HTML,
      ],
    },
  },
  {
    files: DISPLAY_ONLY_FILES.filter((f) => f.endsWith('.html')),
    rules: {
      'no-restricted-syntax': [
        'error',
        {
          selector: "BindingPipe[name='async']",
          message: 'async パイプを使わない。toSignal で signal にして読む。',
        },
      ],
    },
  },
]);
