// シナリオの実行を、本物のランチャーと例の ECU で通す。ランチャーが Control Server を持てば、
// 実行中は見るだけ: 変数の値は見え、書く部品は理由を言って押せない。時刻の操作（ランチャーの標準入力の命令）と
// 停止は、ふつうの実行と同じく効く。持たなければ飛ばす。
import { expect, test } from '@playwright/test';
import { mkdirSync, rmSync, unlinkSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { GUI_PORT, guiToken } from '../support/gui-server';
import {
  RUN_TIMEOUT,
  choose,
  openRun,
  press,
  result,
  start,
  stopButton,
  stopIfRunning,
} from '../support/run-page';
import {
  Target,
  numberOf,
  openVariables,
  scrollWidgetIntoView,
  startRun,
  stopRun,
  waitForVariables,
  widget,
} from '../support/variables-page';

test('a real scenario run: the variables are read but not written, the time and the stop button work', async ({
  page,
}) => {
  const target: Target = { base: `http://127.0.0.1:${GUI_PORT}/`, token: guiToken() };
  const headers = { Authorization: `Bearer ${target.token}`, Origin: new URL(target.base).origin };
  const app = (await (
    await page.request.get(new URL('api/app', target.base).href, { headers })
  ).json()) as {
    variables?: { available: boolean };
  };
  test.skip(
    !app.variables?.available,
    'variables are not available (a launcher without a control server, or a GUI server without gRPC)',
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
    await expect(page.getByTestId('variables-notice')).toContainText(
      'シナリオの実行中は書けません',
      {
        timeout: RUN_TIMEOUT,
      },
    );
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

test('real recording captures finalized end_ms, replays normally to that end time, and supports speed=max handover with manual write and step', async ({
  page,
}) => {
  test.setTimeout(60_000);
  const target: Target = { base: `http://127.0.0.1:${GUI_PORT}/`, token: guiToken() };
  const headers = { Authorization: `Bearer ${target.token}`, Origin: new URL(target.base).origin };
  const app = (await (
    await page.request.get(new URL('api/app', target.base).href, { headers })
  ).json()) as {
    variables?: { available: boolean };
    launcher?: { version?: { features?: string[] } };
  };
  test.skip(
    !app.variables?.available || !app.launcher?.version?.features?.includes('scenario-handover'),
    'variables or scenario-handover not available',
  );

  const rootDir = process.env['VEHICLE_SIL_GUI_E2E_ROOT'];
  if (!rootDir) throw new Error('VEHICLE_SIL_GUI_E2E_ROOT is not set');
  const recordedPath = join(rootDir, 'scenarios', 'recorded_real.yaml');

  await openRun(page);
  await stopIfRunning(page);
  try {
    // 1. Record a real Engine throttle write and verify mid-run vs post-run end_ms
    await startRun(page, target, 'forever.yaml');
    const runId = await waitForVariables(page, target, 'Engine', RUN_TIMEOUT);
    const write = await page.request.post(new URL('api/write', target.base).href, {
      headers,
      data: { run_id: runId, writes: { Engine: { 'engine.throttle': 60 } } },
    });
    expect(write.status(), await write.text()).toBe(200);
    await expect
      .poll(
        async () => {
          const rec = (await (
            await page.request.get(new URL('api/recording', target.base).href, { headers })
          ).json()) as {
            recording: { writes: number; end_ms: number | null; yaml: string | null } | null;
          };
          return rec.recording?.writes ?? 0;
        },
        { timeout: RUN_TIMEOUT },
      )
      .toBeGreaterThanOrEqual(1);

    const midRec = (await (
      await page.request.get(new URL('api/recording', target.base).href, { headers })
    ).json()) as { recording: { end_ms: number | null; yaml: string } };
    expect(midRec.recording.end_ms).toBeNull();
    expect(midRec.recording.yaml).not.toMatch(/^end_ms:/m);

    // Stop cleanly (force: false) so result.timing.sim_ms is recorded
    await openRun(page);
    await press(stopButton(page));
    await expect(result(page, runId)).toBeVisible({ timeout: RUN_TIMEOUT });

    const endRec = (await (
      await page.request.get(new URL('api/recording', target.base).href, { headers })
    ).json()) as { recording: { end_ms: number | null; yaml: string } };
    expect(endRec.recording.end_ms).toBeGreaterThan(0);
    const recordedEndMs = endRec.recording.end_ms!;
    expect(endRec.recording.yaml).toContain(`end_ms: ${recordedEndMs}`);
    expect(endRec.recording.yaml).not.toContain('#   - expect:');
    expect(endRec.recording.yaml).not.toContain('hold_for_ms:');

    // 2. Normal replay of the recorded YAML without manual edits reaches recordedEndMs
    writeFileSync(recordedPath, endRec.recording.yaml, 'utf8');
    await openRun(page);
    await choose(page, 'simulation.yaml');
    const replayCheck = page.waitForResponse(
      (r) =>
        r.url().endsWith('/api/check') &&
        (r.request().postDataJSON() as Record<string, unknown>)['scenario'] ===
          'scenarios/recorded_real.yaml',
    );
    await page.getByRole('combobox', { name: 'シナリオ' }).click();
    await page.getByRole('option', { name: 'recorded_real.yaml', exact: true }).click();
    await replayCheck;
    await expect(page.getByTestId('check-scenario')).toContainText('recorded_real.yaml');
    await page.getByTestId('after-scenario-stop').click();
    await page.getByTestId('stop-at-ms').fill('');
    await page.getByTestId('config-card').getByRole('radio', { name: '最速', exact: true }).click();
    const replayId = await start(page);
    await expect(result(page, replayId)).toContainText('成功', { timeout: RUN_TIMEOUT });
    const replaySession = (await (
      await page.request.get(new URL('api/session', target.base).href, { headers })
    ).json()) as {
      session: {
        result: { timing: { sim_ms: number } };
        scenario: { status: string; handed_over: boolean };
      };
    };
    expect(replaySession.session.result.timing.sim_ms).toBe(recordedEndMs);
    expect(replaySession.session.scenario).toMatchObject({ status: 'passed', handed_over: false });

    // 3. Handover at speed=max pauses at over_at_ms, clears/omits stop_at_ms, and allows Variables UI write + Step on same run
    await openRun(page);
    await choose(page, 'simulation.yaml');
    await page.getByTestId('stop-at-ms').fill('999');
    await page.getByRole('combobox', { name: 'シナリオ' }).click();
    await page.getByRole('option', { name: 'throttle_step.yaml', exact: true }).click();
    await page.getByTestId('after-scenario-pause').click();
    await expect(page.getByTestId('stop-at-ms')).toBeDisabled();
    await expect(page.getByTestId('stop-at-ms')).toHaveValue('');
    await page.getByTestId('config-card').getByRole('radio', { name: '最速', exact: true }).click();
    const handoverReqPromise = page.waitForRequest(
      (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
    );
    const handoverId = await start(page);
    const handoverReqBody = (await handoverReqPromise).postDataJSON() as Record<string, unknown>;
    expect(handoverReqBody['after_scenario']).toBe('pause');
    expect(handoverReqBody['stop_at_ms']).toBeUndefined();

    await expect(page.getByTestId('scenario-handover')).toBeVisible({ timeout: RUN_TIMEOUT });
    const simBefore = (await page.getByTestId('sim-time').textContent())?.trim();
    await page.waitForTimeout(300);
    await expect(page.getByTestId('sim-time')).toHaveText(simBefore!);

    await openVariables(page, target);
    let dashBefore = NaN;
    await expect
      .poll(
        async () => {
          dashBefore = await numberOf(page, 'Dashboard', 'dashboard.engine_speed');
          return Number.isFinite(dashBefore);
        },
        { timeout: RUN_TIMEOUT },
      )
      .toBe(true);
    await scrollWidgetIntoView(page, 'Engine', 'engine.throttle');
    const throttle = widget(page, 'Engine', 'engine.throttle');
    const throttleSlider = throttle.getByRole('slider', { name: 'engine.throttle' });
    await expect(throttleSlider).toBeEnabled();
    const writeReqPromise = page.waitForRequest(
      (r) => r.url().endsWith('/api/write') && r.method() === 'POST',
    );
    await throttleSlider.focus();
    await page.keyboard.press('End');
    const writeReqBody = (await writeReqPromise).postDataJSON() as { run_id: string };
    expect(writeReqBody.run_id).toBe(handoverId);
    await expect(throttle).toContainText('受け付けました', { timeout: RUN_TIMEOUT });

    await openRun(page);
    await page.getByTestId('step-ms').fill('200');
    await page.getByTestId('step').click();
    await expect(page.getByTestId('sim-time')).not.toHaveText(simBefore!, { timeout: RUN_TIMEOUT });
    await openVariables(page, target);
    await expect
      .poll(() => numberOf(page, 'Dashboard', 'dashboard.engine_speed'), { timeout: RUN_TIMEOUT })
      .toBeGreaterThan(Math.max(0, dashBefore));
    await stopRun(page, target, RUN_TIMEOUT);

    // 4. Failing scenario and mid-scenario Stop with handover selected never show handover
    await openRun(page);
    await choose(page, 'simulation.yaml');
    await page.getByRole('combobox', { name: 'シナリオ' }).click();
    await page.getByRole('option', { name: 'fails.yaml', exact: true }).click();
    await page.getByTestId('after-scenario-pause').click();
    await page.getByTestId('config-card').getByRole('radio', { name: '最速', exact: true }).click();
    const failId = await start(page);
    await expect(result(page, failId)).toContainText('失敗', { timeout: RUN_TIMEOUT });
    await expect(page.getByTestId('scenario-handover')).toHaveCount(0);

    const stopRunResp = await page.request.post(new URL('api/run', target.base).href, {
      headers,
      data: {
        file: 'forever.yaml',
        disable: [],
        scenario: 'scenarios/throttle_step.yaml',
        after_scenario: 'pause',
        speed: 0.01,
      },
    });
    expect(stopRunResp.status(), await stopRunResp.text()).toBe(200);
    await expect(page.getByTestId('scenario-card')).toBeVisible({ timeout: RUN_TIMEOUT });
    await press(stopButton(page));
    await expect(page.getByTestId('scenario-card').getByTestId('scenario-outcome')).toContainText(
      'シナリオは終わる前に止まりました',
      { timeout: RUN_TIMEOUT },
    );
    await expect(page.getByTestId('scenario-handover')).toHaveCount(0);
  } finally {
    try {
      unlinkSync(recordedPath);
    } catch {
      // ignore if never created
    }
    await stopRun(page, target, RUN_TIMEOUT);
  }
});

test('real multi-case scenario: variant/case selection, typed parameter override, variable comparison failure provenance, timeline auto-expand & jump, and same-settings rerun from virtual time 0', async ({
  page,
}) => {
  test.setTimeout(90_000);
  const target: Target = { base: `http://127.0.0.1:${GUI_PORT}/`, token: guiToken() };
  const headers = { Authorization: `Bearer ${target.token}`, Origin: new URL(target.base).origin };
  const app = (await (
    await page.request.get(new URL('api/app', target.base).href, { headers })
  ).json()) as {
    launcher?: { version?: { features?: string[] } };
  };
  const features = app.launcher?.version?.features ?? [];
  test.skip(
    !features.includes('scenario-cases') ||
      !features.includes('scenario-templates') ||
      !features.includes('scenario-variable-compare'),
    'launcher does not advertise scenario-cases / scenario-templates / scenario-variable-compare',
  );

  const rootDir = process.env['VEHICLE_SIL_GUI_E2E_ROOT'];
  if (!rootDir) throw new Error('VEHICLE_SIL_GUI_E2E_ROOT is not set');
  const templateDir = join(rootDir, 'scenarios', 'templates');
  const templatePath = join(templateDir, 'real_compare_check.yaml');
  const scenarioPath = join(rootDir, 'scenarios', 'real_cases_e2e.yaml');
  mkdirSync(templateDir, { recursive: true });
  writeFileSync(
    templatePath,
    [
      'vehicle_sil_scenario_template: 1',
      'parameters:',
      '  throttle:',
      '    type: integer',
      '    description: initial throttle command',
      '  max_lag_rpm:',
      '    type: integer',
      '    description: max rpm gap between Dashboard and Engine',
      '  label:',
      '    type: string',
      '    description: tag kept as string',
      'steps:',
      '  - name: "${label}: apply throttle"',
      '    at_ms: 0',
      '    set:',
      '      Engine.engine.throttle: "${throttle}"',
      '  - name: "${label}: track engine speed"',
      '    ramp:',
      '      Engine.engine.throttle: { to: 90, over_ms: 200 }',
      '    expect:',
      '      Dashboard.dashboard.engine_speed:',
      '        near: { variable: Engine.engine.speed }',
      '        tolerance: "${max_lag_rpm}"',
      '',
    ].join('\n'),
    'utf8',
  );
  writeFileSync(
    scenarioPath,
    [
      'vehicle_sil_scenario: 1',
      'name: real cases comparison',
      'params:',
      '  throttle: 40',
      '  max_lag_rpm: 600',
      '  label: "0001"',
      'cases:',
      '  rated:',
      '    params:',
      '      throttle: 50',
      '  big_only:',
      '    only: [big]',
      '    params:',
      '      throttle: 80',
      'steps:',
      '  - name: "run ${label} tracking check"',
      '    template: templates/real_compare_check.yaml',
      '    params:',
      '      throttle: "${throttle}"',
      '      max_lag_rpm: "${max_lag_rpm}"',
      '      label: "${label}"',
      '',
    ].join('\n'),
    'utf8',
  );

  await openRun(page);
  await stopIfRunning(page);
  try {
    // 1. Select variants.yaml -> calm variant guidance (not red error count)
    await choose(page, 'variants.yaml');
    await expect(page.getByTestId('check-outcome')).toContainText('バリアントを選んでください。');
    await expect(page.getByTestId('check-outcome')).not.toHaveClass(/failed/);
    await expect(page.getByTestId('blocked')).toContainText('バリアントを選んでください。');

    // Choose variant small
    await page.getByTestId('variant-select').click();
    await page.getByRole('option', { name: 'small', exact: true }).click();
    await expect(page.getByTestId('check-outcome')).toContainText('通った', {
      timeout: RUN_TIMEOUT,
    });

    // 2. Select multi-case scenario -> calm case guidance, big_only disabled with variant reason
    await page.getByTestId('scenario-select').click();
    await page.getByRole('option', { name: 'real_cases_e2e.yaml', exact: true }).click();
    await expect(page.getByTestId('check-outcome')).toContainText(
      'シナリオのケースを選んでください。',
      { timeout: RUN_TIMEOUT },
    );
    await expect(page.getByTestId('blocked')).toContainText('シナリオのケースを選んでください。');

    await page.getByTestId('case-select').click();
    const bigOpt = page.getByRole('option', { name: /big_only/ });
    await expect(bigOpt).toHaveAttribute('aria-disabled', 'true');
    await expect(bigOpt).toContainText('バリアント big のみ対象');
    await page.getByRole('option', { name: 'rated', exact: true }).click();
    await expect(page.getByTestId('check-outcome')).toContainText('通った', {
      timeout: RUN_TIMEOUT,
    });

    // 3. Override typed parameters: string "0123" stays string, integer 0 forces tight comparison failure
    await expect(page.getByTestId('scenario-params-panel')).toBeVisible();
    await page.getByTestId('param-input-label').fill('0123');
    await page.getByTestId('param-input-max_lag_rpm').fill('0');
    await expect(page.getByTestId('param-badge-max_lag_rpm')).toHaveText('上書き');
    await expect(page.getByTestId('check-outcome')).toContainText('通った', {
      timeout: RUN_TIMEOUT,
    });
    await page.getByTestId('config-card').getByRole('radio', { name: '最速', exact: true }).click();

    // 4. Start run and verify typed params payload
    const firstReqPromise = page.waitForRequest(
      (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
    );
    const firstId = await start(page);
    const firstBody = (await firstReqPromise).postDataJSON() as Record<string, unknown>;
    expect(firstBody).toMatchObject({
      file: 'variants.yaml',
      variant: 'small',
      scenario: 'scenarios/real_cases_e2e.yaml',
      case: 'rated',
      params: { label: '0123', max_lag_rpm: 0 },
    });
    expect(typeof (firstBody['params'] as Record<string, unknown>)['label']).toBe('string');
    expect(typeof (firstBody['params'] as Record<string, unknown>)['max_lag_rpm']).toBe('number');

    // 5. Verify failure provenance, two-signal comparison message, call_stack, and timeline auto-expand
    await expect(result(page, firstId)).toContainText('失敗', { timeout: RUN_TIMEOUT });
    const scenarioCard = page.getByTestId('scenario-card');
    await expect(scenarioCard.getByTestId('scenario-case')).toContainText('rated');
    await expect(scenarioCard.getByTestId('scenario-params')).toContainText('0123');
    await expect(scenarioCard.getByTestId('scenario-step').nth(1)).toContainText(
      'templates/real_compare_check.yaml:',
    );
    await expect(scenarioCard.getByTestId('scenario-step').nth(1)).toContainText(
      'while Engine.engine.speed was',
    );
    await expect(page.getByTestId('result-scenario-case')).toHaveText('rated');
    await expect(page.getByTestId('result-scenario-failure-location')).toContainText(
      'templates/real_compare_check.yaml:',
    );
    await expect(page.getByTestId('result-scenario-failure-message')).toContainText(
      'while Engine.engine.speed was',
    );
    await expect(page.getByTestId('result-scenario-call-stack')).toContainText(
      'real_cases_e2e.yaml:',
    );

    const toggle = scenarioCard.getByTestId('timeline-toggle');
    await expect(toggle).toHaveAttribute('aria-expanded', 'true');
    const timeline = scenarioCard.getByTestId('timeline-element');
    await expect(timeline).toBeVisible();
    await expect(scenarioCard.getByTestId('timeline-failure-strip')).toContainText('#2', {
      timeout: RUN_TIMEOUT,
    });
    await expect(scenarioCard.getByTestId('timeline-readout')).toContainText('選んだ手順: 2');

    const shots = process.env['VEHICLE_SIL_GUI_SHOTS'];
    if (shots) {
      await page.setViewportSize({ width: 1440, height: 1100 });
      await page.locator('mat-sidenav-content').evaluate((el) => el.scrollTo(0, 0));
      await page.waitForTimeout(300);
      await page.screenshot({
        path: `${shots}/run-cases-params-failed-timeline-1440.png`,
      });
      await scenarioCard.scrollIntoViewIfNeeded();
      await page.waitForTimeout(200);
      await scenarioCard.screenshot({
        path: `${shots}/run-scenario-card-timeline-1440.png`,
      });
    }

    // 6. Jump to step 1 from the step table row
    await scenarioCard.getByTestId('scenario-step-jump-1').click();
    await expect(scenarioCard.getByTestId('timeline-readout')).toContainText('選んだ手順: 1');

    // 7. Same-settings rerun starts a new session from virtual time 0 with identical intent
    const firstSession = (await (
      await page.request.get(new URL('api/session', target.base).href, { headers })
    ).json()) as { session: { id: string; result: { timing: { sim_ms: number } } } };

    const rerunReqPromise = page.waitForRequest(
      (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
    );
    const rerunResPromise = page.waitForResponse(
      (r) => r.url().endsWith('/api/run') && r.request().method() === 'POST',
    );
    await scenarioCard.getByTestId('scenario-rerun').click();
    const rerunBody = (await rerunReqPromise).postDataJSON() as Record<string, unknown>;
    expect(rerunBody).toMatchObject({
      file: 'variants.yaml',
      variant: 'small',
      scenario: 'scenarios/real_cases_e2e.yaml',
      case: 'rated',
      params: { label: '0123', max_lag_rpm: 0 },
    });
    const secondId = ((await (await rerunResPromise).json()) as { run_id: string }).run_id;
    expect(secondId).not.toBe(firstId);
    await expect(result(page, secondId)).toContainText('失敗', { timeout: RUN_TIMEOUT });

    const secondSession = (await (
      await page.request.get(new URL('api/session', target.base).href, { headers })
    ).json()) as {
      session: {
        id: string;
        result: { timing: { sim_ms: number }; scenario: { steps: { start_ms: number | null }[] } };
      };
    };
    expect(secondSession.session.id).toBe(secondId);
    expect(secondSession.session.result.scenario.steps[0]?.start_ms).toBe(0);
    expect(secondSession.session.result.timing.sim_ms).toBe(
      firstSession.session.result.timing.sim_ms,
    );
  } finally {
    try {
      unlinkSync(scenarioPath);
    } catch {
      // ignore
    }
    try {
      rmSync(templateDir, { recursive: true, force: true });
    } catch {
      // ignore
    }
    await stopRun(page, target, RUN_TIMEOUT);
  }
});
