// e2e の試験の前に GUI サーバー（gui/server）を流し直すランチャーと一緒に起動し、終わったら止める（Playwright の globalSetup）。
// サーバーは起動のたびにトークンを作るので、Playwright の webServer ではなく、ここで起動して 1 行の JSON からトークンを読む。
// 本物のランチャーで動かす e2e（e2e/real/）も、ここの startGuiServer でサーバーを起動する（support/real-server.ts）。
import { ChildProcess, execFileSync, spawn } from 'node:child_process';
import { once } from 'node:events';
import { readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import type { FullConfig } from '@playwright/test';
import { scaled } from './timeouts';

/**
 * 127.0.0.1 でいま空いているポート（OS が選ぶ）。同じ PC で別の作業ツリーの e2e や手元の GUI サーバーが動いていても
 * ぶつからない。設定ファイルを読む時点で要るので、同期で node の子プロセスに聞く。
 */
export function freePort(): number {
  const script =
    "const s = require('node:net').createServer(); " +
    "s.listen(0, '127.0.0.1', () => { process.stdout.write(String(s.address().port)); s.close(); });";
  return Number(execFileSync(process.execPath, ['-e', script], { encoding: 'utf8' }));
}

/**
 * GUI サーバーのポート（開発サーバーの転送先にもなる）。実行ごとに空いているポートを選ぶ。
 * 設定ファイルは試験を動かすプロセスでも読み直されるので、環境変数で同じ値を渡す。
 */
process.env['VEHICLE_SIL_GUI_E2E_PORT'] ??= String(freePort());
export const GUI_PORT = Number(process.env['VEHICLE_SIL_GUI_E2E_PORT']);

/** 構成ファイルを選べる範囲: 流し直すランチャーの録りがある構成ファイル。 */
export const REPOSITORY = resolve(__dirname, '../../../..');
export const CONFIGS = resolve(REPOSITORY, 'gui/server/tests/configs');
export const REPLAY_LAUNCHER = resolve(REPOSITORY, 'gui/server/tests/replay_launcher.py');

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
 * サーバーを止め、子（流し直すランチャーと participant）まで終わるのを待つ（次の試験が同じポートを使えるように）。
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
  /** 待つポート（既定は GUI_PORT。0: GUI サーバーが空いているポートを選ぶ。試験ごとに別のサーバーを起動するときに使う）。 */
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
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    devOrigin: config.metadata['devOrigin'] as string | undefined,
    env: { REPLAY_TICK_MS: '50' },
  });
}

/** GUI サーバーを起動してトークンを環境変数に置き、止める関数を返す。 */
export async function startGuiServer(options: GuiServerOptions): Promise<() => Promise<void>> {
  const server = await launchGuiServer(options);
  process.env['VEHICLE_SIL_GUI_E2E_TOKEN'] = server.token;
  return server.stop;
}

let launches = 0;

/** GUI サーバーを起動する（環境変数は変えない。試験ごとに新しいサーバーが要る試験も使う）。 */
export async function launchGuiServer(options: GuiServerOptions): Promise<GuiServer> {
  const { launcher, root, devOrigin, web, port = GUI_PORT } = options;
  // 流し直すランチャーが、試験の誤り（録りのない命令など）で終わったときに 1 行を足すファイル。止めるときに空を確かめる
  const report = join(
    tmpdir(),
    `vehicle-sil-replay-report-${process.pid}-${port}-${Date.now()}-${++launches}.txt`,
  );
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
      env: { ...process.env, REPLAY_REPORT: report, ...options.env },
    },
  );
  const lines = createInterface({ input: server.stdout! });
  const startTimeout = scaled(120_000);
  const first = await Promise.race([
    once(lines, 'line').then(([line]) => line as string),
    once(server, 'exit').then(([code]) => {
      throw new Error(`the GUI server ended (exit code ${code}) before printing its URL`);
    }),
    new Promise<never>((_, reject) =>
      setTimeout(
        () => reject(new Error(`the GUI server did not start in ${startTimeout / 1000} s`)),
        startTimeout,
      ).unref(),
    ),
  ]).catch(async (error: unknown) => {
    await stop(server);
    throw error;
  });
  const url = (JSON.parse(first) as { url: string }).url;
  const token = new URLSearchParams(new URL(url).hash.slice(1)).get('token') ?? '';
  return { url, token, stop: () => stopAndCheck(server, report) };
}

/** サーバーを止め、流し直すランチャーが試験の誤りで終わっていないことを確かめる（終わっていれば、その文で落とす）。 */
async function stopAndCheck(server: ChildProcess, report: string): Promise<void> {
  await stop(server);
  let text: string;
  try {
    text = readFileSync(report, 'utf8');
  } catch {
    return; // 何も書かれなかった
  } finally {
    rmSync(report, { force: true });
  }
  if (text.trim()) throw new Error(`the replaying launcher ended on a mistake of a test:\n${text}`);
}
