// シナリオの実行を、本物のランチャーと例の ECU で通す。ランチャーが Control Server と scenario-control を持てば、
// 実行中は見るだけ: 変数の値は見え、書く部品は理由を言って押せない。時刻の操作（ランチャーの標準入力の命令）と
// 停止は、ふつうの実行と同じく効く。持たなければ飛ばす（その場合の画面は、流し直しの e2e が確かめる）。
import { expect, test } from '@playwright/test';
import { GUI_PORT, guiToken } from '../support/gui-server';
import { RUN_TIMEOUT, openRun, press, stopButton } from '../support/run-page';
import { Target, numberOf, openVariables, stopRun, widget } from '../support/variables-page';

test('a real scenario run: the variables are read but not written, the time and the stop button work', async ({
  page,
}) => {
  const target: Target = { base: `http://127.0.0.1:${GUI_PORT}/`, token: guiToken() };
  const headers = { Authorization: `Bearer ${target.token}`, Origin: new URL(target.base).origin };
  const app = (await (
    await page.request.get(new URL('api/app', target.base).href, { headers })
  ).json()) as {
    variables?: { available: boolean };
    launcher?: { version?: { features?: string[] } | null };
  };
  test.skip(
    !app.variables?.available || !app.launcher?.version?.features?.includes('scenario-control'),
    'the launcher has no control server, or none while a scenario runs (scenario-control)',
  );

  await stopRun(page, target, RUN_TIMEOUT);
  // 速さ 0.01: 例のシナリオ（仮想時間で 1 秒足らず）が 1 分以上かかり、確かめ終わるまで終わらない
  const response = await page.request.post(new URL('api/run', target.base).href, {
    headers,
    data: {
      file: 'forever.yaml',
      disable: [],
      scenario: 'scenarios/throttle_step.yaml',
      speed: 0.01,
    },
  });
  expect(response.status(), await response.text()).toBe(200);
  try {
    await openVariables(page, target);
    await expect(page.getByTestId('variables-notice')).toContainText('シナリオの実行中です', {
      timeout: RUN_TIMEOUT,
    });
    // シナリオが時刻 0 に書いたスロットル（50 %）が見える
    await expect
      .poll(() => numberOf(page, 'Engine', 'engine.throttle'), { timeout: RUN_TIMEOUT })
      .toBe(50);
    const throttle = widget(page, 'Engine', 'engine.throttle');
    const slider = throttle.getByRole('slider', { name: 'engine.throttle' });
    await expect(slider).toBeDisabled();
    await expect(throttle).toContainText('シナリオの実行中は書けません');
    // 押しても書かない（誤りも出ない）
    await slider.focus();
    await page.keyboard.press('End');
    await expect(throttle).toHaveAttribute('data-write', 'idle');
    await expect(throttle.getByTestId('widget-error')).toHaveText('');
    expect(await numberOf(page, 'Engine', 'engine.throttle')).toBe(50);

    // 時刻の操作と停止は、実行の画面から
    await openRun(page);
    const pause = page.getByTestId('pause');
    await expect(pause).not.toHaveAttribute('aria-disabled', 'true', { timeout: RUN_TIMEOUT });
    await pause.click();
    const resume = page.getByTestId('resume');
    await expect(resume).not.toHaveAttribute('aria-disabled', 'true', { timeout: RUN_TIMEOUT });
    await resume.click();
    await expect(pause).not.toHaveAttribute('aria-disabled', 'true', { timeout: RUN_TIMEOUT });
    await press(stopButton(page));
    await expect(page.getByTestId('scenario-card').getByTestId('scenario-outcome')).toContainText(
      'シナリオは終わる前に止まりました',
      { timeout: RUN_TIMEOUT },
    );
  } finally {
    await stopRun(page, target, RUN_TIMEOUT);
  }
});
