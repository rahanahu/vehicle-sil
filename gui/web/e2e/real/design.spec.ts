// ネットワークの設計の画面を、本物の GUI サーバーと本物のランチャーで通す。participant は `--describe` にだけ答える偽の
// ECU（support/describe-only-ecu.mjs）なので、C++ の例の ECU は要らない。構成ファイルは、サーバーの root（e2e/real が
// 作った一時ディレクトリ）に試験の中で書く。
// 環境変数 VEHICLE_SIL_GUI_SHOTS にディレクトリを書くと、できあがった画面（幅 1024）も撮る。
import { expect, test } from '@playwright/test';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { guiToken } from '../support/gui-server';
import { RUN_TIMEOUT } from '../support/run-page';

const ECU = resolve(__dirname, '../support/describe-only-ecu.mjs');
const shots = process.env['VEHICLE_SIL_GUI_SHOTS'];

/** ECU_A（eth0）と ECU_B（eth0、eth1）の participant だけの構成ファイル。 */
function writeStart(file: string): void {
  const command = (channels: string, base: number) =>
    `[${JSON.stringify(process.execPath)}, ${JSON.stringify(ECU)}, ${channels}, ${base}]`;
  const root = process.env['VEHICLE_SIL_GUI_E2E_ROOT'];
  if (!root) throw new Error('VEHICLE_SIL_GUI_E2E_ROOT is not set (e2e/support/real-server.ts)');
  writeFileSync(
    join(root, file),
    [
      '# two ECUs; the networks are designed on the screen',
      'vehicle_sil_simulation: 1',
      'stop_at_ms: 1000',
      'networks: []',
      'participants:',
      '  - name: ECU_A',
      `    command: ${command('eth0', 1)}`,
      '  - name: ECU_B',
      `    command: ${command('"eth0,eth1"', 16)}`,
      '',
    ].join('\n'),
  );
}

test('designs two Ethernet networks for two ECUs and passes the check', async ({ page }) => {
  writeStart('design-ab.yaml');
  await page.goto(`/#token=${guiToken()}`);
  await page.getByRole('link', { name: '設計' }).click();
  await expect(page.getByRole('heading', { level: 1, name: 'ネットワークの設計' })).toBeVisible();

  await page.getByTestId('design-file').click();
  await page.getByRole('option', { name: 'design-ab.yaml', exact: true }).click();

  // 参加者だけの構成: ECU の申告からチャネルが出る。まだネットワークがないので、照合は通らない
  await expect(page.getByTestId('channel-ECU_B-eth1')).toContainText('02:00:00:00:00:11', {
    timeout: RUN_TIMEOUT,
  });
  await expect(page.getByTestId('design-outcome')).not.toContainText('通った');

  // ネットワークを 2 つ足す
  for (const name of ['ETH_AB', 'ETH_EXT']) {
    await page.getByTestId('network-name').fill(name);
    await page.getByTestId('network-add').click();
    await expect(page.getByTestId(`network-${name}`)).toBeVisible();
  }

  // ETH_EXT は外につなぐ予定のネットワーク: `external` は文の欄に書く
  const area = page.getByTestId('design-text');
  const text = await area.inputValue();
  await area.fill(
    text.replace(
      /(- name: ETH_EXT\n\s+kind: ethernet\n)/,
      '$1    external:\n      all_frames: true\n',
    ),
  );
  await expect(area).toHaveValue(/external:\n\s+all_frames: true/);

  // 3 本つなぐ
  const connect = async (participant: string, channel: string, network: string) => {
    await page.getByTestId(`network-${participant}-${channel}`).click();
    await page.getByRole('option', { name: network, exact: true }).click();
  };
  await connect('ECU_A', 'eth0', 'ETH_AB');
  await connect('ECU_B', 'eth0', 'ETH_AB');
  await connect('ECU_B', 'eth1', 'ETH_EXT');

  // 通る（ETH_EXT に ECU が 1 つだけでも、`external` があるので誤りでも警告でもない）
  await expect(page.getByTestId('design-outcome')).toContainText('通った', {
    timeout: RUN_TIMEOUT,
  });
  await expect(page.getByTestId('design-outcome')).not.toContainText('照合しています');
  await expect(page.getByTestId('finding').and(page.locator('[data-severity="0"]'))).toHaveCount(0);
  await expect(page.getByTestId('network-ETH_AB')).toContainText('ECU_A, ECU_B');
  await expect(area).toHaveValue(/ETH_EXT: eth1/);
  await expect(area).toHaveValue(/# two ECUs/);

  // 保存すると、ファイルの中身が変わる（実行されるコマンドは変わらない編集なので、既定で保存できる）
  const saved = join(process.env['VEHICLE_SIL_GUI_E2E_ROOT'] ?? '', 'design-ab.yaml');
  expect(readFileSync(saved, 'utf8')).not.toContain('ETH_EXT');
  await expect(page.getByTestId('save')).toBeEnabled();
  await page.getByTestId('save').click();
  await expect(page.getByTestId('saved')).toBeVisible({ timeout: RUN_TIMEOUT });
  expect(readFileSync(saved, 'utf8')).toContain('ETH_EXT: eth1');
  expect(readFileSync(saved, 'utf8')).toContain('# two ECUs');

  // ダウンロードできる
  const download = page.waitForEvent('download');
  await page.getByTestId('download').click();
  expect((await download).suggestedFilename()).toBe('design-ab.yaml');

  if (shots) {
    mkdirSync(shots, { recursive: true });
    await page.setViewportSize({ width: 1024, height: 1400 });
    await page.screenshot({ path: join(shots, 'design-1024.png'), fullPage: true });
  }
});
