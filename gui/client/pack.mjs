// Makes the client library a .tgz that npm can install (not published to npm; it ships with the release).
// Run `npm run pack:client` in gui/web (it borrows esbuild and TypeScript from gui/web/node_modules; no new dependency).
//
//   node ../client/pack.mjs [--out <dir>] [--version <x.y.z>]
//
// Contents: dist/index.js (one ESM bundle), dist/elements.js (the parts, importing ./index.js), types/ (.d.ts), timeline/ (the element <vehicle-sil-timeline> of
// gui/timeline as a script, the same as an ES module for @vehicle-sil/gui-client/timeline, and its .d.ts),
// package.json (exports), README.md, README.ja.md,
// LICENSE. Checked after it is made: Node imports the installed package, and the TypeScript example of
// examples/gui_starter/README.md (and of README.ja.md), the example of the parts section of README.md and a use of the element type-check with the module
// resolutions nodenext and bundler.
// Exit codes: 0 made, 1 failed, 2 usage error.
import { execFileSync } from 'node:child_process';
import {
  cpSync,
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildElements } from './build-elements.mjs';

const CLIENT = dirname(fileURLToPath(import.meta.url));
const REPOSITORY = resolve(CLIENT, '../..');
const WEB = join(REPOSITORY, 'gui', 'web');
const NAME = '@vehicle-sil/gui-client';
const TIMELINE = join(REPOSITORY, 'gui', 'timeline');
/** A screen's use of the element, type-checked against the package: the import, properties and events. */
const TIMELINE_EXAMPLE = `
import '@vehicle-sil/gui-client/timeline';
import type { VehicleSilTimelineElement } from '@vehicle-sil/gui-client/timeline';
const timeline: VehicleSilTimelineElement = document.createElement('vehicle-sil-timeline');
timeline.src = '/api/timeline/3';
timeline.token = null;
timeline.variables = ['Body.*'];
timeline.labels = { 'Body.body.speed': 'Speed' };
timeline.hide = 'tables help';
timeline.compact = true;
timeline.from = 0;
timeline.addEventListener('load', (e) => console.log(e.detail.status, e.detail.end));
timeline.addEventListener('error', (e) => console.log(e.detail.message, e.detail.status));
timeline.addEventListener('cursorchange', (e) => console.log(e.detail.cursor));
timeline.addEventListener('stepselect', (e) => console.log(e.detail.step));
timeline.addEventListener('viewchange', (e) => console.log(e.detail.from, e.detail.to));
document.body.append(timeline);
`;
/** What a screen writes to use the parts, type-checked against the package. */
const ELEMENTS_EXAMPLE = `
import '@vehicle-sil/gui-client/elements';
import { pageIntent, setReasonText } from '@vehicle-sil/gui-client/elements';
import type { VsilRunControlsElement } from '@vehicle-sil/gui-client/elements';
setReasonText((reason) => (reason.code === 'busy' ? 'Wait a moment.' : null));
pageIntent.update({ scenario: 'scenarios/a.yaml' });
const controls: VsilRunControlsElement | null = document.querySelector('vsil-run-controls');
if (controls) controls.intent = { speed: 'max' };
const write = document.createElement('vsil-write');
write.setAttribute('variable', 'Engine.ecu.connected');
document.body.append(write);
`;
const fromWeb = createRequire(join(WEB, 'package.json'));

const USAGE = `usage: node gui/client/pack.mjs [--out <dir>] [--version <x.y.z>]

Makes the client library an npm package: <out>/vehicle-sil-gui-client-<version>.tgz (default out:
build/gui-client; default version: that of gui/server/pyproject.toml), then installs it there and checks it:
Node imports it, and the TypeScript example of examples/gui_starter/README.md (and of README.ja.md) type-checks
against it with the module resolutions nodenext and bundler. Uses esbuild and TypeScript from gui/web/node_modules (npm ci there).
Exit codes: 0 made, 1 failed (the message says where), 2 usage error.`;

function usage(message) {
  console.error(`pack-client: error: ${message}`);
  console.error(USAGE);
  process.exit(2);
}

function fail(message) {
  console.error(`pack-client: FAILED: ${message}`);
  process.exit(1);
}

function options(argv) {
  const out = { out: join(REPOSITORY, 'build', 'gui-client'), version: null };
  for (let i = 0; i < argv.length; i++) {
    const name = argv[i];
    if (name === '--help' || name === '-h') {
      console.log(USAGE);
      process.exit(0);
    } else if (name === '--out' || name === '--version') {
      const value = argv[++i];
      if (value === undefined) usage(`${name} needs a value`);
      out[name.slice(2)] = name === '--out' ? resolve(value) : value;
    } else {
      usage(`unknown argument ${name}`);
    }
  }
  if (out.version !== null && !/^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/.test(out.version)) {
    usage(`--version ${out.version}: write a version such as 1.2.3`);
  }
  return out;
}

/** The version of the GUI server (gui/server/pyproject.toml): the library goes out with it. */
function serverVersion() {
  const text = readFileSync(join(REPOSITORY, 'gui', 'server', 'pyproject.toml'), 'utf8');
  const match = /^version\s*=\s*"([^"]+)"/m.exec(text);
  if (!match) fail('no version = "..." in gui/server/pyproject.toml');
  return match[1];
}

function node(script, args, cwd) {
  try {
    return execFileSync(process.execPath, [script, ...args], {
      cwd,
      encoding: 'utf8',
      stdio: 'pipe',
    });
  } catch (error) {
    const output = `${error.stdout ?? ''}${error.stderr ?? ''}`.trim();
    fail(`${relative(REPOSITORY, script)} ${args.join(' ')}\n${output}`);
  }
}

/** tsc writes the specifiers as written ('./client'); Node and moduleResolution nodenext need './client.js'. */
function addExtensions(directory) {
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) {
      addExtensions(path);
    } else if (entry.name.endsWith('.d.ts')) {
      const text = readFileSync(path, 'utf8').replace(
        /(\bfrom\s+|\bimport\(\s*)(['"])(\.{1,2}\/[^'"]+?)(?<!\.js)\2/g,
        '$1$2$3.js$2',
      );
      writeFileSync(path, text);
    }
  }
}

/** The ```ts example of the starter's README, English and Japanese: what a TypeScript screen writes first. */
function readmeExamples() {
  const examples = [];
  for (const name of ['README.md', 'README.ja.md']) {
    const path = join(REPOSITORY, 'examples', 'gui_starter', name);
    if (!existsSync(path)) continue;
    const match = /```ts\n([\s\S]*?)```/.exec(readFileSync(path, 'utf8'));
    if (!match)
      fail(`examples/gui_starter/${name} has no \`\`\`ts example to check the types with`);
    examples.push(match[1]);
  }
  if (examples.length === 0) fail('examples/gui_starter/README.md is missing');
  return examples;
}

/** The \`\`\`ts example of the parts' section of README.md, so that the README keeps to what the package types allow. */
function elementsReadme() {
  const text = readFileSync(join(CLIENT, 'README.md'), 'utf8');
  const section = /^## Parts \(custom elements\)$/m.exec(text);
  if (!section) fail('gui/client/README.md has no "## Parts (custom elements)" section');
  const match = /```ts\n([\s\S]*?)```/.exec(text.slice(section.index));
  if (!match) fail('the parts section of gui/client/README.md has no ```ts example to check');
  return [match[1]];
}

/** Installs the package as a user would (node_modules/@vehicle-sil/gui-client) and uses it. */
function check(packageDir, checkDir, tsc) {
  rmSync(checkDir, { recursive: true, force: true });
  cpSync(packageDir, join(checkDir, 'node_modules', ...NAME.split('/')), { recursive: true });
  writeFileSync(join(checkDir, 'package.json'), JSON.stringify({ private: true, type: 'module' }));
  const examples = readmeExamples();
  const base = {
    strict: true,
    noEmit: true,
    skipLibCheck: false, // the .d.ts of the package are checked too
    target: 'ES2022',
    lib: ['ES2022', 'DOM'],
    types: [],
  };
  for (const [mode, settings] of [
    ['nodenext', { module: 'nodenext', moduleResolution: 'nodenext' }],
    ['bundler', { module: 'preserve', moduleResolution: 'bundler' }],
  ]) {
    const config = join(checkDir, `tsconfig.${mode}.json`);
    writeFileSync(
      config,
      JSON.stringify({ compilerOptions: { ...base, ...settings }, files: ['main.ts'] }),
    );
    for (const example of [...examples, TIMELINE_EXAMPLE, ELEMENTS_EXAMPLE, ...elementsReadme()]) {
      writeFileSync(join(checkDir, 'main.ts'), `export {};\n${example}`);
      node(tsc, ['-p', config], checkDir);
    }
  }
  const probe =
    `const m = await import('${NAME}');` +
    "for (const name of ['createClient', 'takeToken', 'WriteError']) " +
    "if (typeof m[name] !== 'function') throw new Error('not exported: ' + name);" +
    // Without a DOM the element's file defines only its functions that need none.
    `await import('${NAME}/timeline');` +
    "if (typeof globalThis.VehicleSilTimeline?.readTimeline !== 'function') throw new Error('timeline: no VehicleSilTimeline');" +
    // The parts need a DOM, so Node does not load them: the main entry must not pull them in, and the parts must not
    // carry a copy of the main entry (two copies would make two connections).
    "const fs = await import('node:fs');" +
    `const dist = ${JSON.stringify(join('node_modules', ...NAME.split('/'), 'dist'))};` +
    "if (fs.readFileSync(dist + '/index.js', 'utf8').includes('vsil-status')) throw new Error('the main entry contains the parts');" +
    "const parts = fs.readFileSync(dist + '/elements.js', 'utf8');" +
    "if (!/from\\s*[\"']\\.\\/index\\.js[\"']/.test(parts)) throw new Error('the parts do not import the main entry');" +
    "if (parts.includes('class ActionError')) throw new Error('the parts carry a copy of the main entry');";
  try {
    execFileSync(process.execPath, ['--input-type=module', '-e', probe], {
      cwd: checkDir,
      stdio: 'pipe',
    });
  } catch (error) {
    fail(`importing ${NAME} with Node: ${String(error.stderr ?? error).trim()}`);
  }
}

async function main() {
  const args = options(process.argv.slice(2));
  const version = args.version ?? serverVersion();
  const packageDir = join(args.out, 'package');
  rmSync(packageDir, { recursive: true, force: true });
  mkdirSync(packageDir, { recursive: true });

  const esbuild = fromWeb('esbuild');
  esbuild.buildSync({
    entryPoints: [join(CLIENT, 'src', 'index.ts')],
    bundle: true,
    format: 'esm',
    platform: 'neutral',
    target: 'es2022',
    outfile: join(packageDir, 'dist', 'index.js'),
    logLevel: 'error',
  });
  // The parts import the main entry (dist/index.js) instead of copying it: the page's connection is that one's state.
  await buildElements(join(packageDir, 'dist', 'elements.js'), './index.js');
  const tsc = fromWeb.resolve('typescript/bin/tsc');
  const types = join(packageDir, 'types');
  node(tsc, ['-p', join(CLIENT, 'tsconfig.package.json'), '--declarationDir', types], CLIENT);
  addExtensions(types);
  mkdirSync(join(packageDir, 'timeline'));
  for (const name of ['vehicle-sil-timeline.js', 'vehicle-sil-timeline.d.ts'])
    cpSync(join(TIMELINE, name), join(packageDir, 'timeline', name));
  // The same script as an ES module, for import: a bundler takes a file without export for CommonJS and warns.
  writeFileSync(
    join(packageDir, 'timeline', 'vehicle-sil-timeline.mjs'),
    `${readFileSync(join(TIMELINE, 'vehicle-sil-timeline.js'), 'utf8')}\nexport {};\n`,
  );

  const manifest = {
    name: NAME,
    version,
    description:
      'Client library of the vehicle-sil GUI server: connection, run and time control, ECU variables. No dependencies; browser and Node.',
    license: 'MIT',
    type: 'module',
    exports: {
      '.': { types: './types/index.d.ts', default: './dist/index.js' },
      './elements': { types: './types/elements/index.d.ts', default: './dist/elements.js' },
      './timeline': {
        types: './timeline/vehicle-sil-timeline.d.ts',
        default: './timeline/vehicle-sil-timeline.mjs',
      },
    },
    types: './types/index.d.ts',
    // Importing the timeline or the parts defines elements: a bundler must keep that import.
    sideEffects: [
      './dist/elements.js',
      './timeline/vehicle-sil-timeline.js',
      './timeline/vehicle-sil-timeline.mjs',
    ],
    files: ['dist', 'types', 'timeline', 'README.ja.md'],
    engines: { node: '>=22' },
  };
  writeFileSync(join(packageDir, 'package.json'), `${JSON.stringify(manifest, null, 2)}\n`);
  cpSync(join(CLIENT, 'README.md'), join(packageDir, 'README.md'));
  // README.md links to the Japanese edition, so it goes along.
  cpSync(join(CLIENT, 'README.ja.md'), join(packageDir, 'README.ja.md'));
  cpSync(join(REPOSITORY, 'LICENSE'), join(packageDir, 'LICENSE'));

  const checkDir = join(args.out, 'check');
  check(packageDir, checkDir, tsc);
  rmSync(checkDir, { recursive: true, force: true });

  const npm = process.env['npm_execpath'];
  const packArgs = ['pack', '--json', '--pack-destination', args.out];
  let packed;
  try {
    const output = npm
      ? execFileSync(process.execPath, [npm, ...packArgs], { cwd: packageDir, encoding: 'utf8' })
      : execFileSync('npm', packArgs, {
          cwd: packageDir,
          encoding: 'utf8',
          shell: process.platform === 'win32',
        });
    packed = JSON.parse(output)[0];
  } catch (error) {
    fail(`npm pack: ${String(error.stderr ?? error).trim()}`);
  }
  const files = packed.files.map((f) => f.path);
  for (const needed of [
    'package.json',
    'README.md',
    'README.ja.md',
    'LICENSE',
    'dist/index.js',
    'dist/elements.js',
    'types/index.d.ts',
    'types/elements/index.d.ts',
    'timeline/vehicle-sil-timeline.js',
    'timeline/vehicle-sil-timeline.mjs',
    'timeline/vehicle-sil-timeline.d.ts',
  ]) {
    if (!files.includes(needed)) fail(`the package has no ${needed}`);
  }
  const tgz = join(args.out, packed.filename);
  console.log(
    `pack-client: ${relative(process.cwd(), tgz)} (${NAME} ${version}, ${files.length} files; ` +
      'imported with Node, the example of examples/gui_starter and the timeline element type-checked with ' +
      'nodenext and bundler)',
  );
}

await main();
