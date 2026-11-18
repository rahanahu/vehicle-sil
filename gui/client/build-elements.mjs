// Bundles the parts (custom elements, src/elements/index.ts) into one ES module that imports the main entry instead of
// copying it: the main entry keeps the page's one shared connection (`connect()`), and a copy of it would make a second.
//
//   node build-elements.mjs <outfile> <main-entry-specifier>     (borrows esbuild from gui/web/node_modules)
//
// For example `node build-elements.mjs build/gui-client/vehicle-sil-gui-elements.mjs ./vehicle-sil-gui-client.mjs`.
// Used by `npm run build:client` (gui/web) and by pack.mjs.
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const CLIENT = dirname(fileURLToPath(import.meta.url));
const fromWeb = createRequire(join(CLIENT, '..', 'web', 'package.json'));

/** Writes `outfile`; `mainEntry` is what the parts import (`../index` in the sources) written as the browser will read it. */
export async function buildElements(outfile, mainEntry) {
  const mainEntryIsExternal = {
    name: 'main-entry-is-external',
    setup(build) {
      build.onResolve({ filter: /^\.\.\/index$/ }, () => ({ path: mainEntry, external: true }));
    },
  };
  await fromWeb('esbuild').build({
    entryPoints: [join(CLIENT, 'src', 'elements', 'index.ts')],
    bundle: true,
    format: 'esm',
    platform: 'neutral',
    target: 'es2022',
    outfile,
    plugins: [mainEntryIsExternal],
    logLevel: 'error',
  });
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const [outfile, mainEntry] = process.argv.slice(2);
  if (!outfile || !mainEntry) {
    console.error('usage: node build-elements.mjs <outfile> <main-entry-specifier>');
    process.exit(2);
  }
  await buildElements(outfile, mainEntry);
}
