// @ts-check
// 書き方の決まり（README.md の「書き方の決まり」）を lint で強制する。
const eslint = require('@eslint/js');
const { defineConfig } = require('eslint/config');
const tseslint = require('typescript-eslint');
const angular = require('angular-eslint');

// RxJS をどこで使うかは lint で縛らない（README の「書き方の決まり」で判断する）。
// lint には、どこに書いても誤りと言える規則だけを置く。

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
      ],
    },
  },
]);
