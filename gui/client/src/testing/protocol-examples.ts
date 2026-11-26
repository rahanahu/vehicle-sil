// Loads the examples in gui/protocol/examples/ for unit tests. Do not import it from production code.
// On Node the files are read directly (an example you add is picked up by the tests automatically). A test in a real
// browser (the unit tests of gui/web) cannot read files, so there the files come through the command
// `readProtocolExamples`, which gui/web/vitest-base.config.mts defines and runs on Node.
// The tests run in gui/web (the gui/client tests run with gui/web's tools too).

/** An HTTP example ({description, request, status, response}). */
export interface HttpExample {
  name: string;
  description: string;
  request: { method: string; path: string; body?: unknown };
  status: number;
  response: unknown;
}

/** A WebSocket example ({description, client, server}). */
export interface WebSocketExample {
  name: string;
  description: string;
  client: unknown[];
  server: unknown[];
}

interface NodeFs {
  readdirSync(path: string): string[];
  readFileSync(path: string, encoding: 'utf8'): string;
}

interface NodeProcess {
  cwd(): string;
}

/**
 * Names (without extension) and contents of gui/protocol/examples/. dir is the examples directory (default
 * `../protocol/examples`, which is right when the current directory is gui/web).
 */
export async function protocolExamples(dir?: string): Promise<Map<string, unknown>> {
  if (!(globalThis as { process?: { versions?: { node?: string } } }).process?.versions?.node) {
    // Not on Node, so in a browser: the examples directory is the one of gui/web/vitest-base.config.mts unless dir is given.
    const { commands } = (await import('vitest/browser')) as unknown as {
      commands: Record<
        'readProtocolExamples',
        (dir?: string) => Promise<{ name: string; text: string }[]>
      >;
    };
    return new Map(
      (await commands.readProtocolExamples(dir)).map(({ name, text }) => [name, JSON.parse(text)]),
    );
  }
  // Load node: built-ins at run time, so the test bundle (esbuild) does not include them
  const fsName = 'node:fs';
  const fs = (await import(/* @vite-ignore */ fsName)) as NodeFs;
  const process = (globalThis as unknown as { process: NodeProcess }).process;
  dir ??= `${process.cwd()}/../protocol/examples`;
  const examples = new Map<string, unknown>();
  for (const file of fs.readdirSync(dir).sort()) {
    if (file.endsWith('.json')) {
      examples.set(
        file.slice(0, -'.json'.length),
        JSON.parse(fs.readFileSync(`${dir}/${file}`, 'utf8')),
      );
    }
  }
  return examples;
}

export async function httpExamples(dir?: string): Promise<HttpExample[]> {
  return [...(await protocolExamples(dir))]
    .filter(([, value]) => typeof value === 'object' && value !== null && 'request' in value)
    .map(([name, value]) => ({ name, ...(value as Omit<HttpExample, 'name'>) }));
}

export async function webSocketExamples(dir?: string): Promise<WebSocketExample[]> {
  return [...(await protocolExamples(dir))]
    .filter(([, value]) => typeof value === 'object' && value !== null && 'server' in value)
    .map(([name, value]) => ({ name, ...(value as Omit<WebSocketExample, 'name'>) }));
}

export interface ProtocolSchemas {
  openapi: Record<string, unknown>;
  wsServer: Record<string, unknown>;
  wsClient: Record<string, unknown>;
  wsCommandEvent: Record<string, unknown>;
  cliJson: Record<string, unknown>;
}

/**
 * Loads the generated OpenAPI 3.1 document and Draft 2020-12 JSON Schemas from gui/protocol/ (Node tests only).
 */
export async function protocolSchemas(protocolDir?: string): Promise<ProtocolSchemas> {
  const fsName = 'node:fs';
  const yamlName = 'yaml';
  const fs = (await import(/* @vite-ignore */ fsName)) as NodeFs;
  const yamlMod = (await import(/* @vite-ignore */ yamlName)) as {
    parse(text: string): Record<string, unknown>;
  };
  const process = (globalThis as unknown as { process: NodeProcess }).process;
  const dir = protocolDir ?? `${process.cwd()}/../protocol`;
  const readJson = (rel: string): Record<string, unknown> =>
    JSON.parse(fs.readFileSync(`${dir}/${rel}`, 'utf8')) as Record<string, unknown>;
  return {
    openapi: yamlMod.parse(fs.readFileSync(`${dir}/openapi.yaml`, 'utf8')),
    wsServer: readJson('schemas/ws-server.schema.json'),
    wsClient: readJson('schemas/ws-client.schema.json'),
    wsCommandEvent: readJson('schemas/ws-command-event.schema.json'),
    cliJson: readJson('schemas/cli-json.schema.json'),
  };
}
