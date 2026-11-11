// 単体試験のための、gui/protocol/examples/ の実例の読み込み。本番のコードからは import しない。
// 単体試験は Node の上で動くので、ファイルを直接読む（例を足したら、試験も自動でそれを読む）。

/** HTTP の例（{description, request, status, response}）。 */
export interface HttpExample {
  name: string;
  description: string;
  request: { method: string; path: string; body?: unknown };
  status: number;
  response: unknown;
}

/** WebSocket の例（{description, client, server}）。 */
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

/** gui/protocol/examples/ の、名前（拡張子なし）と中身。試験は gui/web で動く。 */
export async function protocolExamples(): Promise<Map<string, unknown>> {
  // node: の組み込みは、試験の束ね（esbuild）に含めず、実行のときに読む
  const fsName = 'node:fs';
  const fs = (await import(/* @vite-ignore */ fsName)) as NodeFs;
  const process = (globalThis as unknown as { process: NodeProcess }).process;
  const dir = `${process.cwd()}/../protocol/examples`;
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

export async function httpExamples(): Promise<HttpExample[]> {
  return [...(await protocolExamples())]
    .filter(([, value]) => typeof value === 'object' && value !== null && 'request' in value)
    .map(([name, value]) => ({ name, ...(value as Omit<HttpExample, 'name'>) }));
}

export async function webSocketExamples(): Promise<WebSocketExample[]> {
  return [...(await protocolExamples())]
    .filter(([, value]) => typeof value === 'object' && value !== null && 'server' in value)
    .map(([name, value]) => ({ name, ...(value as Omit<WebSocketExample, 'name'>) }));
}
