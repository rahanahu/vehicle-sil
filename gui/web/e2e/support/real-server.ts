// 本物のランチャー（vehicle_sil_launch）と例の ECU で動かす e2e（e2e/real/）の globalSetup。
// 環境変数 VEHICLE_SIL_GUI_LAUNCHER のランチャーと、その隣にビルドした ecu_main の例（VEHICLE_SIL_GUI_EXAMPLES で
// 別の場所も指せる）を使う。構成ファイルは gui/server/tests/real_configs.py が一時ディレクトリに書く（ビルドの
// ディレクトリには書かない）。終わったら GUI サーバーを止め、一時ディレクトリを消す。
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { REPOSITORY, startGuiServer, uvNoSyncArgs } from './gui-server';
import { scaled } from './timeouts';

export default async function startRealGuiServer(): Promise<() => Promise<void>> {
  const launcher = process.env['VEHICLE_SIL_GUI_LAUNCHER'];
  if (!launcher) {
    throw new Error(
      'VEHICLE_SIL_GUI_LAUNCHER is not set: name a built vehicle_sil_launch ' +
        '(build/<preset>/tools/launch/vehicle_sil_launch), or run scripts/verify-gui.py --launcher <path>',
    );
  }
  const root = mkdtempSync(join(tmpdir(), 'vehicle-sil-gui-real-'));
  try {
    execFileSync(
      'uv',
      [
        'run',
        ...uvNoSyncArgs(),
        '--project',
        REPOSITORY,
        'python',
        resolve(REPOSITORY, 'gui/server/tests/real_configs.py'),
        root,
      ],
      { stdio: ['ignore', 'ignore', 'inherit'] },
    );
  } catch (error) {
    rmSync(root, { recursive: true, force: true });
    throw error;
  }
  process.env['VEHICLE_SIL_GUI_E2E_ROOT'] = root;
  const stop = await startGuiServer({
    launcher: resolve(launcher),
    root,
    args: [
      '--participant-start-timeout-ms',
      String(scaled(2500)),
      '--participant-stop-timeout-ms',
      String(scaled(2000)),
    ],
  });
  return async () => {
    await stop();
    rmSync(root, { recursive: true, force: true });
  };
}
