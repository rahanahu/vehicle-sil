// Makes vehicle-sil-timeline.js, the one file that users load: messages.js, timeline.css and viewer.js, in that
// order, as they are (the CSS as the text of a script). Run it after changing one of them, and commit the result:
//
//   node gui/timeline/bundle.js            (npm run bundle:timeline in gui/web)
//   node gui/timeline/bundle.js --check    (exit 1 when vehicle-sil-timeline.js is not what the sources make)
//
// `npm run test:timeline` runs the check, so CI finds a file that was not made again. No dependencies.
'use strict';
const fs = require('node:fs');
const path = require('node:path');

const HERE = __dirname;
const OUT = path.join(HERE, 'vehicle-sil-timeline.js');

/** The text of a source file, with \n line ends (a Windows checkout may have \r\n). */
function source(name) {
  return fs.readFileSync(path.join(HERE, name), 'utf8').replace(/\r\n/g, '\n');
}

/** The contents of vehicle-sil-timeline.js, from the sources. Throws when the CSS cannot go into the script. */
function bundle() {
  const css = source('timeline.css');
  for (const bad of ['`', '${', '\\', '</']) {
    if (css.includes(bad))
      throw new Error(`timeline.css has ${bad}: the bundle keeps it in a template literal`);
  }
  const parts = [
    '// vehicle-sil-timeline.js: the custom element <vehicle-sil-timeline>, which draws a timeline file of',
    '// vehicle-sil (docs/gui.md, "Putting a timeline on your own screen"). Load it with <script src> or import it',
    '// for its effect; vehicle-sil-timeline.d.ts has its types. Made by gui/timeline/bundle.js from messages.js,',
    '// timeline.css and viewer.js: do not edit it, edit those and run bundle.js again.',
    '',
    source('messages.js').trimEnd(),
    '',
    '// timeline.css, adopted by the shadow root of every element',
    '(function (global) {',
    "  'use strict';",
    `  global.VehicleSilTimelineStyles = \`\n${css.trimEnd()}\n\`;`,
    '})(globalThis);',
    '',
    source('viewer.js').trimEnd(),
    '',
  ];
  return parts.join('\n');
}

function main(argv) {
  const check = argv.includes('--check');
  if (argv.some((a) => a !== '--check')) {
    console.error('usage: node gui/timeline/bundle.js [--check]');
    process.exit(2);
  }
  const made = bundle();
  if (check) {
    let have = null;
    try {
      have = fs.readFileSync(OUT, 'utf8').replace(/\r\n/g, '\n');
    } catch {
      // missing: said below
    }
    if (have !== made) {
      console.error(
        'bundle-timeline: FAILED: gui/timeline/vehicle-sil-timeline.js is not what messages.js, timeline.css and ' +
          'viewer.js make: run `npm run bundle:timeline` in gui/web (node gui/timeline/bundle.js) and commit it',
      );
      process.exit(1);
    }
    console.log('bundle-timeline: vehicle-sil-timeline.js is up to date');
    return;
  }
  fs.writeFileSync(OUT, made);
  console.log(`bundle-timeline: wrote ${path.relative(process.cwd(), OUT)}`);
}

if (require.main === module) main(process.argv.slice(2));
module.exports = { bundle, OUT };
