// クライアントのライブラリを、npm で入れられる .tgz にする（npm には公開しない。配布物に付ける）。
// gui/web で `npm run pack:client`（gui/web の node_modules の esbuild と TypeScript を借りる。依存を足さない）。
//
//   node ../client/pack.mjs [--out <dir>] [--version <x.y.z>]
//
// 中身: dist/index.js（1 つにまとめた ESM）、types/（.d.ts）、package.json（exports）、README.md、LICENSE。
// 作った後に確かめる: 入れた形で Node から import でき、examples/gui_starter/README.md の TypeScript の例が、
// 型の解決 nodenext と bundler の両方で型の検査を通る。終了コード: 0 できた、1 失敗、2 使い方の誤り。
import { execFileSync } from 'node:child_process';
import { cpSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const CLIENT = dirname(fileURLToPath(import.meta.url));
const REPOSITORY = resolve(CLIENT, '../..');
const WEB = join(REPOSITORY, 'gui', 'web');
const NAME = '@vehicle-sil/gui-client';
const fromWeb = createRequire(join(WEB, 'package.json'));

const USAGE = `usage: node gui/client/pack.mjs [--out <dir>] [--version <x.y.z>]

Makes the client library an npm package: <out>/vehicle-sil-gui-client-<version>.tgz (default out:
build/gui-client; default version: that of gui/server/pyproject.toml), then installs it there and checks it:
Node imports it, and the TypeScript example of examples/gui_starter/README.md type-checks against it with the
module resolutions nodenext and bundler. Uses esbuild and TypeScript from gui/web/node_modules (npm ci there).
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

/** The ```ts example of the starter's README: what a TypeScript screen writes first. */
function readmeExample() {
  const readme = readFileSync(join(REPOSITORY, 'examples', 'gui_starter', 'README.md'), 'utf8');
  const match = /```ts\n([\s\S]*?)```/.exec(readme);
  if (!match) fail('examples/gui_starter/README.md has no ```ts example to check the types with');
  return match[1];
}

/** Installs the package as a user would (node_modules/@vehicle-sil/gui-client) and uses it. */
function check(packageDir, checkDir, tsc) {
  rmSync(checkDir, { recursive: true, force: true });
  cpSync(packageDir, join(checkDir, 'node_modules', ...NAME.split('/')), { recursive: true });
  writeFileSync(join(checkDir, 'package.json'), JSON.stringify({ private: true, type: 'module' }));
  writeFileSync(join(checkDir, 'main.ts'), `export {};\n${readmeExample()}`);
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
    node(tsc, ['-p', config], checkDir);
  }
  const probe =
    `const m = await import('${NAME}');` +
    "for (const name of ['createClient', 'takeToken', 'WriteError']) " +
    "if (typeof m[name] !== 'function') throw new Error('not exported: ' + name);";
  try {
    execFileSync(process.execPath, ['--input-type=module', '-e', probe], {
      cwd: checkDir,
      stdio: 'pipe',
    });
  } catch (error) {
    fail(`importing ${NAME} with Node: ${String(error.stderr ?? error).trim()}`);
  }
}

function main() {
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
  const tsc = fromWeb.resolve('typescript/bin/tsc');
  const types = join(packageDir, 'types');
  node(tsc, ['-p', join(CLIENT, 'tsconfig.package.json'), '--declarationDir', types], CLIENT);
  addExtensions(types);

  const manifest = {
    name: NAME,
    version,
    description:
      'Client library of the vehicle-sil GUI server: connection, run and time control, ECU variables. No dependencies; browser and Node.',
    license: 'MIT',
    type: 'module',
    exports: { '.': { types: './types/index.d.ts', default: './dist/index.js' } },
    types: './types/index.d.ts',
    sideEffects: false,
    files: ['dist', 'types'],
    engines: { node: '>=22' },
  };
  writeFileSync(join(packageDir, 'package.json'), `${JSON.stringify(manifest, null, 2)}\n`);
  cpSync(join(CLIENT, 'README.md'), join(packageDir, 'README.md'));
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
    'LICENSE',
    'dist/index.js',
    'types/index.d.ts',
  ]) {
    if (!files.includes(needed)) fail(`the package has no ${needed}`);
  }
  const tgz = join(args.out, packed.filename);
  console.log(
    `pack-client: ${relative(process.cwd(), tgz)} (${NAME} ${version}, ${files.length} files; ` +
      'imported with Node, the example of examples/gui_starter type-checked with nodenext and bundler)',
  );
}

main();
