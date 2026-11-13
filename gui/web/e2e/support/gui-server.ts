// e2e の試験の前に GUI サーバー（gui/server）を偽のランチャーと一緒に起動し、終わったら止める（Playwright の globalSetup）。
// サーバーは起動のたびにトークンを作るので、Playwright の webServer ではなく、ここで起動して 1 行の JSON からトークンを読む。
// 本物のランチャーで動かす e2e（e2e/real/）も、ここの startGuiServer でサーバーを起動する（support/real-server.ts）。
import { ChildProcess, spawn } from 'node:child_process';
import { once } from 'node:events';
import { resolve } from 'node:path';
import { createInterface } from 'node:readline';
import type { FullConfig } from '@playwright/test';

/**
 * GUI サーバーのポート（開発サーバーの転送先にもなる）。手元で動かしているもの（既定は空き）とぶつからない範囲で、
 * 実行ごとに変える（前の実行の接続が TIME_WAIT で残っていると、同じポートで待てないため）。
 * 設定ファイルは試験を動かすプロセスでも読み直されるので、環境変数で同じ値を渡す。
 */
process.env['VEHICLE_SIL_GUI_E2E_PORT'] ??= String(18000 + (process.pid % 1000));
export const GUI_PORT = Number(process.env['VEHICLE_SIL_GUI_E2E_PORT']);

/** 構成ファイルを選べる範囲: 偽のランチャーの筋書きごとの構成ファイル。 */
export const REPOSITORY = resolve(__dirname, '../../../..');
export const CONFIGS = resolve(REPOSITORY, 'gui/server/tests/configs');
export const FAKE_LAUNCHER = resolve(REPOSITORY, 'gui/server/tests/fake_launcher.py');

/** 試験が読むトークン（process.env に置くと、試験を動かすプロセスに渡る）。 */
export function guiToken(): string {
  const token = process.env['VEHICLE_SIL_GUI_E2E_TOKEN'];
  if (!token) throw new Error('the GUI server was not started (e2e/support/gui-server.ts)');
  return token;
}

/** プロセスグループにまだ誰かいるか。 */
function groupAlive(pgid: number): boolean {
  try {
    process.kill(-pgid, 0);
    return true;
  } catch {
    return false;
  }
}

/**
 * サーバーを止め、子（偽のランチャーと participant）まで終わるのを待つ（次の試験が同じポートを使えるように）。
 * POSIX: サーバーは SIGTERM で、動いているランチャーを止めてから終わる。uv と python をまとめて（プロセスグループ）送り、
 * 15 秒で終わらなければ SIGKILL。
 */
async function stop(server: ChildProcess): Promise<void> {
  const pid = server.pid;
  if (pid === undefined) return;
  if (process.platform === 'win32') {
    if (server.exitCode === null) {
      await once(spawn('taskkill', ['/T', '/F', '/PID', String(pid)]), 'exit');
    }
    return;
  }
  if (!groupAlive(pid)) return;
  process.kill(-pid, 'SIGTERM');
  const deadline = Date.now() + 15_000;
  while (groupAlive(pid) && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  if (groupAlive(pid)) process.kill(-pid, 'SIGKILL');
}

/** GUI サーバーの起動の指定。 */
export interface GuiServerOptions {
  launcher: string;
  root: string;
  devOrigin?: string;
  env?: Record<string, string>;
  /** 待つポート（既定は GUI_PORT）。 */
  port?: number;
  /** 配る画面のディレクトリ（`--web`。既定はサンプルの画面）。 */
  web?: string;
}

/** 起動した GUI サーバー。 */
export interface GuiServer {
  /** 画面を開く URL（`#token=` つき）。 */
  url: string;
  token: string;
  stop: () => Promise<void>;
}

export default async function startFakeGuiServer(config: FullConfig): Promise<() => Promise<void>> {
  return startGuiServer({
    launcher: FAKE_LAUNCHER,
    root: CONFIGS,
    devOrigin: config.metadata['devOrigin'] as string | undefined,
    env: { FAKE_LAUNCHER_TICK_MS: '50' },
  });
}

/** GUI サーバーを起動してトークンを環境変数に置き、止める関数を返す。 */
export async function startGuiServer(options: GuiServerOptions): Promise<() => Promise<void>> {
  const server = await launchGuiServer(options);
  process.env['VEHICLE_SIL_GUI_E2E_TOKEN'] = server.token;
  return server.stop;
}

/** GUI サーバーを起動する（環境変数は変えない。試験ごとに新しいサーバーが要る試験も使う）。 */
export async function launchGuiServer(options: GuiServerOptions): Promise<GuiServer> {
  const { launcher, root, devOrigin, web, port = GUI_PORT } = options;
  const server = spawn(
    'uv',
    [
      'run',
      '--project',
      REPOSITORY,
      'vehicle-sil-gui',
      '--no-browser',
      '--json',
      '--port',
      String(port),
      '--launcher',
      launcher,
      '--root',
      root,
      ...(devOrigin ? ['--dev-origin', devOrigin] : []),
      ...(web ? ['--web', web] : []),
    ],
    {
      cwd: REPOSITORY,
      detached: process.platform !== 'win32',
      stdio: ['ignore', 'pipe', 'inherit'],
      env: { ...process.env, ...options.env },
    },
  );
  const lines = createInterface({ input: server.stdout! });
  const first = await Promise.race([
    once(lines, 'line').then(([line]) => line as string),
    once(server, 'exit').then(([code]) => {
      throw new Error(`the GUI server ended (exit code ${code}) before printing its URL`);
    }),
    new Promise<never>((_, reject) =>
      setTimeout(() => reject(new Error('the GUI server did not start in 120 s')), 120_000).unref(),
    ),
  ]).catch(async (error: unknown) => {
    await stop(server);
    throw error;
  });
  const url = (JSON.parse(first) as { url: string }).url;
  const token = new URLSearchParams(new URL(url).hash.slice(1)).get('token') ?? '';
  return { url, token, stop: () => stop(server) };
}
