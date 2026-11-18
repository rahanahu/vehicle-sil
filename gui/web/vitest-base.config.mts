// Extra settings of the unit tests (`npm test`, picked up by `runnerConfig: true` in angular.json). The tests run in a
// real browser, which cannot read files, so the protocol examples (gui/protocol/examples/) come through a command
// that runs on Node (see protocolExamples in gui/client/src/testing/protocol-examples.ts).
import { readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const examplesDir = fileURLToPath(new URL('../protocol/examples', import.meta.url));
const schemaFile = fileURLToPath(new URL('../protocol/dashboard.schema.json', import.meta.url));

export default {
  test: {
    browser: {
      commands: {
        // The example files, as {name, text} in name order.
        readProtocolExamples: (_context: unknown, dir?: string) =>
          readdirSync(dir ?? examplesDir)
            .sort()
            .filter((file) => file.endsWith('.json'))
            .map((file) => ({
              name: file.slice(0, -'.json'.length),
              text: readFileSync(`${dir ?? examplesDir}/${file}`, 'utf8'),
            })),
        readDashboardSchema: () => readFileSync(schemaFile, 'utf8'),
      },
    },
  },
};
