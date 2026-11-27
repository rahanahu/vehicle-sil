// Verifies the real packed @vehicle-sil/gui-client .tgz across 5 environments:
//   1. Vanilla TS (nodenext + bundler, skipLibCheck: false, zero framework packages installed)
//   2. Angular (ngc + strictTemplates: true, positive + negative)
//   3. React 18 & React 19 (tsc --noEmit --skipLibCheck false, positive + @ts-expect-error negative)
//   4. Vue 3 (vue-tsc --noEmit, positive + negative)
//   5. Svelte 5 (svelte-check, positive + negative)
// Plus real Playwright Chromium runtime mount smoke tests for Vanilla, React 19, Vue 3, and Svelte 5
// exercising live property binding (false, 0, boolean toggle, numeric stepMs, array variables) and
// genuine custom event reception (vsil-write-queued / vsil-write-result on <vsil-write> / <vsil-slider>
// plus load and mouse-driven cursorchange on <vehicle-sil-timeline>).
//
// Usage: node gui/client/fixtures/verify-consumers.mjs [--tgz <path>] [--no-install]
import { execFileSync } from 'node:child_process';
import {
  cpSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  symlinkSync,
} from 'node:fs';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const FIXTURES = dirname(fileURLToPath(import.meta.url));
const CLIENT = resolve(FIXTURES, '..');
const REPO = resolve(CLIENT, '../..');
const WEB = join(REPO, 'gui', 'web');
const PKG_NAME = '@vehicle-sil/gui-client';
const fromWeb = createRequire(join(WEB, 'package.json'));

function fail(message) {
  console.error(`verify-consumers: FAILED: ${message}`);
  process.exit(1);
}

function parseArgs(argv) {
  const opts = { tgz: null, install: true };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--tgz') {
      opts.tgz = argv[++i] ? resolve(argv[i]) : fail('--tgz needs a path');
    } else if (arg === '--no-install') {
      opts.install = false;
    } else {
      fail(`unknown argument: ${arg}`);
    }
  }
  return opts;
}

function findTarball(explicit) {
  if (explicit) {
    if (!existsSync(explicit)) fail(`tarball not found: ${explicit}`);
    return explicit;
  }
  const distDir = join(REPO, 'build', 'gui-client');
  if (!existsSync(distDir)) {
    fail('build/gui-client does not exist; run `npm --prefix gui/web run pack:client` first');
  }
  const matches = readdirSync(distDir)
    .filter((f) => /^vehicle-sil-gui-client-.*\.tgz$/.test(f))
    .sort();
  if (matches.length === 0) {
    fail('no vehicle-sil-gui-client-*.tgz in build/gui-client; run `npm run pack:client` first');
  }
  return join(distDir, matches.at(-1));
}

function runCmd(cmd, args, cwd, label, { expectFail = false, expectErrors = [] } = {}) {
  try {
    const out = execFileSync(cmd, args, {
      cwd,
      encoding: 'utf8',
      stdio: 'pipe',
      shell: process.platform === 'win32' && /\.(cmd|bat)$/i.test(cmd),
    });
    if (expectFail) {
      fail(`${label}: expected type-check failure, but exited 0\n${out}`);
    }
    return out;
  } catch (error) {
    const output = `${error.stdout ?? ''}${error.stderr ?? ''}`.trim();
    if (expectFail) {
      if (!output) fail(`${label}: exited non-zero without diagnostic output`);
      for (const pattern of expectErrors) {
        if (!pattern.test(output)) {
          fail(`${label}: expected diagnostic matching ${pattern}, got:\n${output}`);
        }
      }
      return output;
    }
    fail(`${label}:\n${output}`);
  }
}

function runNode(script, args, cwd, label, opts = {}) {
  return runCmd(process.execPath, [script, ...args], cwd, label, opts);
}

function runNpm(args, cwd, label) {
  const npmExec = process.env['npm_execpath'];
  if (npmExec && npmExec.endsWith('.js')) {
    return runNode(npmExec, args, cwd, label);
  }
  const npmBin = process.platform === 'win32' ? 'npm.cmd' : 'npm';
  return runCmd(npmBin, args, cwd, label);
}

function ensureFixtureInstalled(fixtureDir, name, allowInstall) {
  const nm = join(fixtureDir, 'node_modules');
  if (existsSync(nm)) return;
  if (!allowInstall) {
    fail(`${relative(REPO, nm)} is missing (run npm ci in ${relative(REPO, fixtureDir)})`);
  }
  console.log(`verify-consumers: installing fixture deps for ${name}...`);
  runNpm(['ci', '--no-audit', '--no-fund'], fixtureDir, `npm ci (${name})`);
}

function linkNodeModules(sourceNm, targetNm, allowlist = null) {
  mkdirSync(targetNm, { recursive: true });
  const linkType = process.platform === 'win32' ? 'junction' : 'dir';
  for (const entry of readdirSync(sourceNm, { withFileTypes: true })) {
    if (entry.name === '@vehicle-sil' || entry.name === '.package-lock.json') continue;
    if (allowlist && !allowlist.has(entry.name)) continue;
    const src = join(sourceNm, entry.name);
    const dst = join(targetNm, entry.name);
    if (entry.name.startsWith('@') && allowlist) {
      mkdirSync(dst, { recursive: true });
      for (const sub of readdirSync(src, { withFileTypes: true })) {
        if (!allowlist.has(`${entry.name}/${sub.name}`)) continue;
        symlinkSync(join(src, sub.name), join(dst, sub.name), linkType);
      }
    } else if (!existsSync(dst)) {
      symlinkSync(src, dst, linkType);
    }
  }
}

function copyFixtureSources(srcDir, stageDir, runtimeHelpersSubdir = 'src') {
  mkdirSync(stageDir, { recursive: true });
  for (const entry of readdirSync(srcDir, { withFileTypes: true })) {
    if (entry.name === 'node_modules' || entry.name === 'package-lock.json') continue;
    cpSync(join(srcDir, entry.name), join(stageDir, entry.name), { recursive: true });
  }
  if (runtimeHelpersSubdir) {
    const destDir = join(stageDir, runtimeHelpersSubdir);
    mkdirSync(destDir, { recursive: true });
    cpSync(join(FIXTURES, 'runtime-helpers.ts'), join(destDir, 'runtime-helpers.ts'));
  }
}

function installTarballIntoStage(stageDir, unpackedPkgDir) {
  const pkgDst = join(stageDir, 'node_modules', ...PKG_NAME.split('/'));
  mkdirSync(dirname(pkgDst), { recursive: true });
  cpSync(unpackedPkgDir, pkgDst, { recursive: true });
}

function assertIsolatedPkgPlugin(stageDir) {
  const expectedPrefix = resolve(stageDir, 'node_modules', ...PKG_NAME.split('/'));
  return {
    name: 'assert-isolated-tarball-pkg',
    setup(build) {
      build.onResolve({ filter: /^@vehicle-sil\/gui-client(\/.*)?$/ }, async (args) => {
        if (args.pluginData?.checked) return null;
        const result = await build.resolve(args.path, {
          resolveDir: args.resolveDir,
          kind: args.kind,
          pluginData: { checked: true },
        });
        if (result.errors.length > 0) return result;
        const resolved = resolve(result.path);
        if (resolved !== expectedPrefix && !resolved.startsWith(expectedPrefix + sep)) {
          return {
            errors: [
              {
                text: `${args.path} resolved outside isolated tarball dir: ${resolved} (expected under ${expectedPrefix})`,
              },
            ],
          };
        }
        return result;
      });
    },
  };
}

function vueSfcPlugin(vueFixtureDir) {
  const requireVue = createRequire(join(vueFixtureDir, 'package.json'));
  const compiler = requireVue('@vue/compiler-sfc');
  const isCustomElement = (tag) => tag.startsWith('vsil-') || tag === 'vehicle-sil-timeline';
  return {
    name: 'vue-sfc',
    setup(build) {
      build.onLoad({ filter: /\.vue$/ }, (args) => {
        const source = readFileSync(args.path, 'utf8');
        const id = 'vsil-vue-smoke';
        const { descriptor, errors } = compiler.parse(source, { filename: args.path });
        if (errors.length > 0) {
          return { errors: errors.map((e) => ({ text: String(e) })) };
        }
        const compiledScript = compiler.compileScript(descriptor, {
          id,
          inlineTemplate: true,
          templateOptions: {
            compilerOptions: { isCustomElement },
          },
        });
        return { contents: compiledScript.content, loader: 'ts' };
      });
    },
  };
}

async function svelte5Plugin(svelteFixtureDir, esbuild) {
  const requireSvelte = createRequire(join(svelteFixtureDir, 'package.json'));
  const compilerMod = await import(pathToFileURL(requireSvelte.resolve('svelte/compiler')).href);
  const compile = compilerMod.compile ?? compilerMod.default?.compile;
  if (typeof compile !== 'function') fail('could not load svelte/compiler compile()');
  return {
    name: 'svelte5',
    setup(build) {
      build.onLoad({ filter: /\.svelte$/ }, (args) => {
        const raw = readFileSync(args.path, 'utf8');
        // Strip TypeScript syntax inside <script lang="ts"> before handing to svelte/compiler.
        const stripped = raw.replace(
          /<script\b([^>]*)\blang=["']ts["']([^>]*)>([\s\S]*?)<\/script>/,
          (_match, before, after, body) => {
            const { code } = esbuild.transformSync(body, {
              loader: 'ts',
              target: 'es2022',
              tsconfigRaw: { compilerOptions: { verbatimModuleSyntax: true } },
            });
            return `<script${before}${after}>\n${code}\n</script>`;
          },
        );
        const { js } = compile(stripped, {
          filename: args.path,
          generate: 'client',
          dev: false,
        });
        return { contents: js.code, loader: 'js' };
      });
    },
  };
}

async function runChromiumSmoke(bundles) {
  const { chromium } = fromWeb('playwright-core');
  const browser = await chromium.launch({ headless: true });
  try {
    for (const { name, bundlePath } of bundles) {
      const page = await browser.newPage({ viewport: { width: 1000, height: 900 } });
      const pageErrors = [];
      const consoleErrors = [];
      page.on('pageerror', (err) => pageErrors.push(String(err)));
      page.on('console', (msg) => {
        if (msg.type() === 'error') consoleErrors.push(msg.text());
      });
      try {
        await page.setContent(
          '<!doctype html><html><head><meta charset="utf-8"></head><body><div id="app"></div></body></html>',
        );
        await page.addScriptTag({ path: bundlePath, type: 'module' });
        await page.waitForFunction(
          () =>
            Boolean(window.__vsilSmoke?.ready) &&
            (window.__vsilSmoke?.loads?.length ?? 0) >= 1 &&
            Boolean(document.querySelector('#w-false button')) &&
            Boolean(document.querySelector('#w-zero button')) &&
            Boolean(document.querySelector('#w-fail button')) &&
            Boolean(document.querySelector('#sl input')) &&
            Boolean(document.querySelector('#val output')) &&
            Boolean(document.querySelector('#rc button')) &&
            Boolean(document.querySelector('#tc input')),
          undefined,
          { timeout: 10_000 },
        );

        // Verify live DOM properties & falsy/boolean/numeric/array bindings.
        const initial = await page.evaluate(() => {
          const wFalse = document.querySelector('#w-false');
          const wZero = document.querySelector('#w-zero');
          const val = document.querySelector('#val');
          const valKebab = document.querySelector('#val-kebab');
          const rc = document.querySelector('#rc');
          const tc = document.querySelector('#tc');
          const tl = document.querySelector('#tl');
          return {
            wFalseAttr: wFalse.getAttribute('value'),
            wFalseProp: wFalse.value,
            wZeroAttr: wZero.getAttribute('value'),
            wZeroProp: wZero.value,
            valHasNoUnit: val.hasAttribute('no-unit'),
            valNoUnitProp: val.noUnit,
            valOutput: val.querySelector('output').textContent,
            valKebab: valKebab
              ? {
                  noUnit: valKebab.noUnit,
                  output: valKebab.querySelector('output')?.textContent ?? null,
                }
              : null,
            rcHasClient: rc.client !== null && rc.client !== undefined,
            rcIntentSpeed: rc.intent?.speed ?? null,
            tcStepAttr: tc.getAttribute('step-ms'),
            tcInputVal: tc.querySelector('input').value,
            tlCompact: tl.compact,
            tlCompactAttr: tl.hasAttribute('compact'),
            tlVariables: tl.variables,
            loads: window.__vsilSmoke.loads,
          };
        });

        const assertEq = (actual, expected, what) => {
          const a = JSON.stringify(actual);
          const e = JSON.stringify(expected);
          if (a !== e) fail(`[runtime:${name}] ${what}: expected ${e}, got ${a}`);
        };

        assertEq(initial.wFalseAttr, 'false', 'vsil-write value={false} attribute');
        assertEq(initial.wFalseProp, false, 'vsil-write value={false} property');
        assertEq(initial.wZeroAttr, '0', 'vsil-write value={0} attribute');
        assertEq(initial.wZeroProp, 0, 'vsil-write value={0} property');
        assertEq(initial.valHasNoUnit, true, 'vsil-value noUnit=true attribute');
        assertEq(initial.valNoUnitProp, true, 'vsil-value noUnit=true property');
        assertEq(initial.valOutput, '42', 'vsil-value hides unit when noUnit=true');
        if (name === 'vue') {
          assertEq(initial.valKebab, { noUnit: true, output: '42' }, 'vue :no-unit="true" initial');
        }
        assertEq(initial.rcHasClient, true, 'vsil-run-controls initial client');
        assertEq(initial.rcIntentSpeed, 'max', 'vsil-run-controls initial intent');
        assertEq(initial.tcStepAttr, '25', 'vsil-time-controls stepMs=25 attribute');
        assertEq(initial.tcInputVal, '25', 'vsil-time-controls stepMs=25 input value');
        assertEq(initial.tlCompact, true, 'vehicle-sil-timeline compact=true property');
        assertEq(initial.tlCompactAttr, true, 'vehicle-sil-timeline compact=true attribute');
        assertEq(initial.tlVariables, ['Body.level'], 'vehicle-sil-timeline variables array prop');
        assertEq(initial.loads[0].status, 'passed', 'vehicle-sil-timeline load event status');
        if (typeof initial.loads[0].end !== 'number' || initial.loads[0].end < 8020) {
          fail(
            `[runtime:${name}] vehicle-sil-timeline load event end expected >= 8020, got ${JSON.stringify(initial.loads[0].end)}`,
          );
        }

        // Click vsil-write buttons (success falsy false, 0, and pre-queue ActionError failure).
        await page.locator('#w-false button').click();
        await page.locator('#w-zero button').click();
        await page.locator('#w-fail button').click();
        await page.waitForFunction(
          () => (window.__vsilSmoke?.resultEvents?.length ?? 0) >= 3,
          undefined,
          { timeout: 5_000 },
        );

        // Exercise <vsil-slider> rapid coalescing while enqueue() is in flight: dropped intermediates must never emit vsil-write-queued.
        const heldQueuedCount = await page.evaluate(() => {
          window.__vsilSmoke.holdSliderEnqueue();
          const input = document.querySelector('#sl input');
          for (const v of ['20', '40', '60', '80']) {
            input.value = v;
            input.dispatchEvent(new Event('change'));
          }
          return window.__vsilSmoke.queuedEvents.filter((e) => e.source === 'sl').length;
        });
        assertEq(
          heldQueuedCount,
          0,
          'vsil-slider emits no vsil-write-queued while enqueue is held',
        );
        await page.evaluate(() => window.__vsilSmoke.releaseSliderEnqueue());
        await page.waitForFunction(
          () =>
            (window.__vsilSmoke?.resultEvents?.filter((e) => e.source === 'sl').length ?? 0) >= 2,
          undefined,
          { timeout: 5_000 },
        );

        // Click framework toggle button to flip noUnit from true -> false reactively.
        await page.locator('#toggle-unit').click();
        await page.waitForFunction(
          () =>
            document.querySelector('#val output')?.textContent === '42 km/h' &&
            (!document.querySelector('#val-kebab') ||
              document.querySelector('#val-kebab output')?.textContent === '42 km/h'),
          undefined,
          { timeout: 5_000 },
        );
        // Transition client & intent to undefined via reactive state and verify normalization to null.
        await page.locator('#clear-client').click();
        await page.waitForFunction(
          () => document.querySelector('#rc')?.client === null,
          undefined,
          { timeout: 5_000 },
        );

        const afterToggle = await page.evaluate(() => {
          const val = document.querySelector('#val');
          const valKebab = document.querySelector('#val-kebab');
          const rc = document.querySelector('#rc');
          return {
            writes: window.__vsilSmoke.writes,
            queuedEvents: window.__vsilSmoke.queuedEvents,
            resultEvents: window.__vsilSmoke.resultEvents,
            eventLog: window.__vsilSmoke.eventLog,
            valHasNoUnit: val.hasAttribute('no-unit'),
            valNoUnitProp: val.noUnit,
            valKebab: valKebab
              ? {
                  noUnit: valKebab.noUnit,
                  output: valKebab.querySelector('output')?.textContent ?? null,
                }
              : null,
            rcClientIsNull: rc.client === null,
            rcIntentIsNull: rc.intent === null,
          };
        });
        assertEq(
          afterToggle.writes,
          [
            { ref: 'E.e.flag', value: false },
            { ref: 'E.e.throttle', value: 0 },
            { ref: 'E.e.fault', value: true },
            { ref: 'E.e.throttle', value: 20 },
            { ref: 'E.e.throttle', value: 80 },
          ],
          'vsil-write falsy + fault + coalesced slider writes (40 and 60 never sent)',
        );
        assertEq(
          afterToggle.queuedEvents,
          [
            { source: 'w-false', session: 's1', seq: 1, peer: 'E', name: 'e.flag', value: false },
            { source: 'w-zero', session: 's1', seq: 2, peer: 'E', name: 'e.throttle', value: 0 },
            { source: 'sl', session: 's1', seq: 3, peer: 'E', name: 'e.throttle', value: 20 },
            { source: 'sl', session: 's1', seq: 4, peer: 'E', name: 'e.throttle', value: 80 },
          ],
          'vsil-write-queued emitted strictly for ECU-queued writes (never for pre-queue failure or coalesced 40/60)',
        );
        assertEq(
          afterToggle.resultEvents,
          [
            {
              source: 'w-false',
              ok: true,
              peer: 'E',
              name: 'e.flag',
              value: false,
              seq: 1,
              appliedAtMs: 110,
              late: false,
            },
            {
              source: 'w-zero',
              ok: true,
              peer: 'E',
              name: 'e.throttle',
              value: 0,
              seq: 2,
              appliedAtMs: 120,
              late: false,
            },
            {
              source: 'w-fail',
              ok: false,
              peer: 'E',
              name: 'e.fault',
              value: true,
              errorCode: 'write-busy',
              errorOutcome: 'refused',
              isActionError: true,
            },
            {
              source: 'sl',
              ok: true,
              peer: 'E',
              name: 'e.throttle',
              value: 20,
              seq: 3,
              appliedAtMs: 130,
              late: false,
            },
            {
              source: 'sl',
              ok: true,
              peer: 'E',
              name: 'e.throttle',
              value: 80,
              seq: 4,
              appliedAtMs: 140,
              late: false,
            },
          ],
          'vsil-write-result payloads include appliedAtMs on success and concrete ActionError on failure',
        );
        for (const seq of [1, 2, 3, 4]) {
          const qIdx = afterToggle.eventLog.indexOf(`queued:${seq}`);
          const rIdx = afterToggle.eventLog.indexOf(`result:${seq}`);
          if (qIdx < 0 || rIdx < 0 || qIdx >= rIdx) {
            fail(
              `[runtime:${name}] expected queued:${seq} before result:${seq}, got ${JSON.stringify(afterToggle.eventLog)}`,
            );
          }
        }
        assertEq(afterToggle.valHasNoUnit, false, 'vsil-value noUnit=false removes attribute');
        assertEq(afterToggle.valNoUnitProp, false, 'vsil-value noUnit=false property');
        if (name === 'vue') {
          assertEq(
            afterToggle.valKebab,
            { noUnit: false, output: '42 km/h' },
            'vue :no-unit="false" dynamic toggle shows unit',
          );
        }
        assertEq(afterToggle.rcClientIsNull, true, 'Part.client normalizes undefined to null');
        assertEq(
          afterToggle.rcIntentIsNull,
          true,
          'VsilRunControlsElement.intent normalizes undefined to null',
        );

        // Trigger a real pointermove inside <vehicle-sil-timeline>'s shadow DOM plot to emit cursorchange.
        const svgBox = await page.evaluate(() => {
          const tl = document.querySelector('#tl');
          const svgs = [...(tl.shadowRoot?.querySelectorAll('.lane-plot svg') ?? [])];
          const svg = svgs.at(-1);
          if (!svg) return null;
          svg.scrollIntoView({ block: 'center' });
          const r = svg.getBoundingClientRect();
          return { x: r.x, y: r.y, width: r.width, height: r.height };
        });
        if (!svgBox || svgBox.width <= 0 || svgBox.height <= 0) {
          fail(`[runtime:${name}] vehicle-sil-timeline rendered no visible plot SVG`);
        }
        await page.mouse.move(svgBox.x + svgBox.width * 0.35, svgBox.y + svgBox.height * 0.5);
        await page.mouse.move(svgBox.x + svgBox.width * 0.65, svgBox.y + svgBox.height * 0.5);
        await page.waitForFunction(
          () => (window.__vsilSmoke?.cursors?.length ?? 0) >= 1,
          undefined,
          { timeout: 5_000 },
        );
        const cursors = await page.evaluate(() => window.__vsilSmoke.cursors);
        if (typeof cursors.at(-1) !== 'number') {
          fail(
            `[runtime:${name}] cursorchange detail.cursor expected number, got ${JSON.stringify(cursors)}`,
          );
        }

        if (pageErrors.length > 0) {
          fail(`[runtime:${name}] uncaught page errors:\n${pageErrors.join('\n')}`);
        }
        if (consoleErrors.length > 0) {
          fail(`[runtime:${name}] console errors:\n${consoleErrors.join('\n')}`);
        }
        console.log(`verify-consumers: runtime smoke ok (${name})`);
      } finally {
        await page.close();
      }
    }
  } finally {
    await browser.close();
  }
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const tgz = findTarball(opts.tgz);
  const tsc = fromWeb.resolve('typescript/bin/tsc');
  const ngcPkgPath = fromWeb.resolve('@angular/compiler-cli/package.json');
  const ngcPkg = JSON.parse(readFileSync(ngcPkgPath, 'utf8'));
  const ngc = resolve(dirname(ngcPkgPath), ngcPkg.bin.ngc);
  const esbuild = fromWeb('esbuild');

  // Ensure isolated framework fixture toolsets are installed (never touching gui/web/node_modules).
  for (const [name, relDir] of [
    ['react18', join('react', 'v18')],
    ['react19', join('react', 'v19')],
    ['vue', 'vue'],
    ['svelte', 'svelte'],
  ]) {
    ensureFixtureInstalled(join(FIXTURES, relDir), name, opts.install);
  }

  const stageRoot = mkdtempSync(join(tmpdir(), 'vsil-consumers-'));
  try {
    // 1. Vanilla TS: install the real .tgz via npm with zero framework packages anywhere in its parent tree.
    const vanillaStage = join(stageRoot, 'vanilla');
    copyFixtureSources(join(FIXTURES, 'vanilla'), vanillaStage);
    runNpm(
      ['install', '--offline', '--no-audit', '--no-fund', '--no-package-lock', tgz],
      vanillaStage,
      'npm install real .tgz (vanilla)',
    );
    const unpackedPkg = join(vanillaStage, 'node_modules', ...PKG_NAME.split('/'));
    if (!existsSync(join(unpackedPkg, 'package.json'))) {
      fail('tarball installation did not populate node_modules/@vehicle-sil/gui-client');
    }
    runNode(
      tsc,
      ['-p', join(vanillaStage, 'tsconfig.nodenext.json')],
      vanillaStage,
      'vanilla (nodenext)',
    );
    runNode(
      tsc,
      ['-p', join(vanillaStage, 'tsconfig.bundler.json')],
      vanillaStage,
      'vanilla (bundler)',
    );
    console.log(
      'verify-consumers: type-check ok (vanilla: nodenext + bundler, skipLibCheck=false)',
    );

    // 2. Angular (ngc + strictTemplates: true): isolated stage with only Angular core packages + real .tgz.
    const angularStage = join(stageRoot, 'angular');
    copyFixtureSources(join(FIXTURES, 'angular'), angularStage, null);
    linkNodeModules(
      join(WEB, 'node_modules'),
      join(angularStage, 'node_modules'),
      new Set([
        '@angular',
        '@angular/core',
        '@angular/common',
        '@angular/compiler',
        '@angular/compiler-cli',
        '@angular/platform-browser',
        'rxjs',
        'tslib',
        'typescript',
      ]),
    );
    installTarballIntoStage(angularStage, unpackedPkg);
    runNode(ngc, ['-p', join(angularStage, 'tsconfig.json')], angularStage, 'angular (positive)');
    runNode(
      ngc,
      ['-p', join(angularStage, 'tsconfig.negative.json')],
      angularStage,
      'angular (negative)',
      {
        expectFail: true,
        expectErrors: [
          /Argument of type 'string' is not assignable to parameter of type 'Event'/,
          /Argument of type 'number' is not assignable to parameter of type 'Event'/,
        ],
      },
    );
    console.log(
      'verify-consumers: type-check ok (angular: ngc strictTemplates positive + negative)',
    );

    // 3. React 18 & React 19 (tsc --noEmit --skipLibCheck false, positive + @ts-expect-error negative).
    const react18Stage = join(stageRoot, 'react18');
    copyFixtureSources(join(FIXTURES, 'react', 'v18'), react18Stage, null);
    linkNodeModules(
      join(FIXTURES, 'react', 'v18', 'node_modules'),
      join(react18Stage, 'node_modules'),
    );
    installTarballIntoStage(react18Stage, unpackedPkg);
    runNode(tsc, ['-p', join(react18Stage, 'tsconfig.json')], react18Stage, 'react 18');

    const react19Stage = join(stageRoot, 'react19');
    copyFixtureSources(join(FIXTURES, 'react', 'v19'), react19Stage);
    linkNodeModules(
      join(FIXTURES, 'react', 'v19', 'node_modules'),
      join(react19Stage, 'node_modules'),
    );
    installTarballIntoStage(react19Stage, unpackedPkg);
    runNode(tsc, ['-p', join(react19Stage, 'tsconfig.json')], react19Stage, 'react 19');
    console.log('verify-consumers: type-check ok (react 18 + react 19, skipLibCheck=false)');

    // 4. Vue 3 (vue-tsc --noEmit, positive + negative).
    const vueStage = join(stageRoot, 'vue');
    const vueFixtureDir = join(FIXTURES, 'vue');
    copyFixtureSources(vueFixtureDir, vueStage);
    linkNodeModules(join(vueFixtureDir, 'node_modules'), join(vueStage, 'node_modules'));
    installTarballIntoStage(vueStage, unpackedPkg);
    const vueTsc = join(vueFixtureDir, 'node_modules', 'vue-tsc', 'bin', 'vue-tsc.js');
    runNode(
      vueTsc,
      ['-p', join(vueStage, 'tsconfig.json'), '--noEmit'],
      vueStage,
      'vue 3 (positive)',
    );
    runNode(
      vueTsc,
      ['-p', join(vueStage, 'tsconfig.negative.json'), '--noEmit'],
      vueStage,
      'vue 3 (negative)',
      {
        expectFail: true,
        expectErrors: [
          /Type 'number' is not assignable to type 'boolean/,
          /Argument of type 'number' is not assignable to parameter of type 'string'/,
          /Argument of type 'string' is not assignable to parameter of type 'boolean'/,
          /Type '"neon"' is not assignable to type/,
          /Argument of type 'string' is not assignable to parameter of type 'number'/,
        ],
      },
    );
    console.log('verify-consumers: type-check ok (vue 3: vue-tsc positive + negative)');

    // 5. Svelte 5 (svelte-check, positive + negative; --tsconfig is resolved relative to --workspace).
    const svelteStage = join(stageRoot, 'svelte');
    const svelteFixtureDir = join(FIXTURES, 'svelte');
    copyFixtureSources(svelteFixtureDir, svelteStage, join('src', 'runtime'));
    linkNodeModules(join(svelteFixtureDir, 'node_modules'), join(svelteStage, 'node_modules'));
    installTarballIntoStage(svelteStage, unpackedPkg);
    const svelteCheck = join(
      svelteFixtureDir,
      'node_modules',
      'svelte-check',
      'bin',
      'svelte-check',
    );
    runNode(
      svelteCheck,
      ['--workspace', join(svelteStage, 'src', 'positive'), '--tsconfig', '../../tsconfig.json'],
      svelteStage,
      'svelte 5 (positive)',
    );
    runNode(
      svelteCheck,
      [
        '--workspace',
        join(svelteStage, 'src', 'negative'),
        '--tsconfig',
        '../../tsconfig.negative.json',
      ],
      svelteStage,
      'svelte 5 (negative)',
      {
        expectFail: true,
        expectErrors: [
          /Type 'number' is not assignable to type 'boolean/,
          /Type 'false' is not assignable to type/,
          /Argument of type 'number' is not assignable to parameter of type 'string'/,
          /Argument of type 'string' is not assignable to parameter of type 'boolean'/,
          /Type '"neon"' is not assignable to type/,
          /Argument of type 'string' is not assignable to parameter of type 'number'/,
        ],
      },
    );
    console.log('verify-consumers: type-check ok (svelte 5: svelte-check positive + negative)');

    // 6. Real Playwright Chromium runtime mount smoke tests from the real packed .tgz.
    const runtimeOutDir = join(stageRoot, 'runtime');
    mkdirSync(runtimeOutDir, { recursive: true });
    const bundles = [];

    for (const spec of [
      {
        name: 'vanilla',
        stageDir: vanillaStage,
        entry: join(vanillaStage, 'src', 'runtime.ts'),
        plugins: [],
      },
      {
        name: 'react19',
        stageDir: react19Stage,
        entry: join(react19Stage, 'src', 'runtime.tsx'),
        plugins: [],
        extra: { jsx: 'automatic' },
      },
      {
        name: 'vue',
        stageDir: vueStage,
        entry: join(vueStage, 'src', 'runtime.ts'),
        plugins: [vueSfcPlugin(vueFixtureDir)],
        extra: {
          define: {
            __VUE_OPTIONS_API__: 'false',
            __VUE_PROD_DEVTOOLS__: 'false',
            __VUE_PROD_HYDRATION_MISMATCH_DETAILS__: 'false',
          },
        },
      },
      {
        name: 'svelte',
        stageDir: svelteStage,
        entry: join(svelteStage, 'src', 'runtime', 'runtime.ts'),
        plugins: [await svelte5Plugin(svelteFixtureDir, esbuild)],
        extra: { conditions: ['browser'] },
      },
    ]) {
      const outfile = join(runtimeOutDir, `${spec.name}.bundle.js`);
      await esbuild.build({
        absWorkingDir: spec.stageDir,
        entryPoints: [spec.entry],
        bundle: true,
        format: 'esm',
        platform: 'browser',
        target: 'es2022',
        nodePaths: [join(spec.stageDir, 'node_modules')],
        outfile,
        plugins: [assertIsolatedPkgPlugin(spec.stageDir), ...spec.plugins],
        logLevel: 'error',
        ...(spec.extra ?? {}),
      });
      bundles.push({ name: spec.name, bundlePath: outfile });
    }

    await runChromiumSmoke(bundles);

    console.log(
      `verify-consumers: 5 environments type-checked + ${bundles.length} Chromium runtime mounts passed (${relative(REPO, tgz)})`,
    );
  } finally {
    rmSync(stageRoot, { recursive: true, force: true });
  }
}

await main();
