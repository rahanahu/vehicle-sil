// Loads the examples in gui/protocol/examples/ for unit tests. Do not import it from production code.
// Unit tests run on Node, so the files are read directly (an example you add is picked up by the tests automatically).
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
