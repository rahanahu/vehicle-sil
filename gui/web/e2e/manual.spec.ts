// 自分で起動する participant（ランチャーの manual-participants）を、本物の GUI サーバーと流し直すランチャーで通す。
// simulation.yaml の録りでは、自分で起動する participant は参加しない（止めるまで待つ）。manual.yaml の録りでは、
// Dashboard が構成ファイルで manual: true で、後から参加し、終わっても残る（replay-fault: manual-stay）。
import { expect, test } from '@playwright/test';
import {
  RUN_TIMEOUT,
  choose,
  languageOf,
  openRun,
  press,
  result,
  start,
  stopButton,
  stopIfRunning,
} from './support/run-page';

test.beforeEach(async ({ page, locale }) => {
  await openRun(page, languageOf(locale));
  await stopIfRunning(page, languageOf(locale));
});

test('a participant switched to be started by the user: the command to type, then a stop before the start', async ({
  page,
}) => {
  await choose(page, 'simulation.yaml');
  const manual = page.getByTestId('manual-Dashboard').getByRole('switch');
  await expect(manual).not.toBeChecked();
  const checked = page.waitForRequest(
    (r) => r.url().endsWith('/api/check') && r.postDataJSON()?.manual?.includes('Dashboard'),
  );
  await manual.click();
  await checked;
  await expect(page.getByTestId('check-participant-Dashboard')).toContainText(
    'ランチャーは起動しません',
  );

  const sent = page.waitForRequest((r) => r.url().endsWith('/api/run'));
  const session = await start(page);
  expect((await sent).postDataJSON()).toMatchObject({ manual: ['Dashboard'] });
  const status = page.getByTestId('status-Dashboard');
  await expect(status).toContainText('自分で起動する: 参加を待っています', {
    timeout: RUN_TIMEOUT,
  });
  const command = page.getByTestId('manual-command-Dashboard');
  await expect(command.locator('pre')).toContainText('--name Dashboard --registry-uri');
  await expect(command.locator('code.directory')).not.toBeEmpty();
  await expect(command.getByRole('button')).toBeVisible();

  // 参加を待っている間に止める: シミュレーションは始まらない
  await press(stopButton(page));
  await expect(result(page, session)).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(status).toContainText('参加しなかった（自分で起動）');
  await expect(command).toHaveCount(0);
});

test('a participant the file says the user starts: it joins later, and stays after the end', async ({
  page,
}) => {
  await choose(page, 'manual.yaml');
  const manual = page.getByTestId('manual-Dashboard').getByRole('switch');
  await expect(manual).toBeChecked();
  await expect(manual).toBeDisabled();
  await expect(page.getByTestId('check-participant-Dashboard')).toContainText('manual: true');

  const session = await start(page);
  await expect(result(page, session)).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(result(page, session).getByTestId('result')).toContainText('成功');
  await expect(page.getByTestId('status-Dashboard')).toContainText('参加した（自分で起動）');
  await expect(page.getByTestId('manual-stays-Dashboard')).toContainText(
    '自分で終わらせてください',
  );
});

test('a registry on the network: the check notes it, and the command comes with the environment to set', async ({
  page,
}) => {
  // network.yaml: registry_uri は全部のアドレス（ループバックでない）。Dashboard は manual: true で、参加しない
  await choose(page, 'network.yaml');
  const note = page
    .getByTestId('check-card')
    .getByTestId('finding')
    .filter({ hasText: 'registry-on-network' });
  await expect(note).toHaveAttribute('data-severity', '2');

  const session = await start(page);
  const command = page.getByTestId('manual-command-Dashboard');
  // 変数は名前と値を分けて出し、別のボタンで 名前=値 を写す（コマンドのボタンは変数を写さない）
  const variable = command
    .getByTestId('manual-environment-Dashboard')
    .getByTestId('manual-variable-VEHICLE_SIL_LISTEN_ON_NETWORK');
  await expect(variable.locator('.variable-name')).toHaveText('VEHICLE_SIL_LISTEN_ON_NETWORK', {
    timeout: RUN_TIMEOUT,
  });
  await expect(variable.locator('.variable-value')).toHaveText('1');
  await expect(variable.getByRole('button')).toHaveAttribute(
    'title',
    'VEHICLE_SIL_LISTEN_ON_NETWORK=1',
  );
  await expect(command.locator('pre')).toContainText('--name Dashboard --registry-uri');
  await expect(command.locator('pre')).not.toContainText('VEHICLE_SIL_LISTEN_ON_NETWORK');

  await press(stopButton(page));
  await expect(result(page, session)).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(command).toHaveCount(0);
});
