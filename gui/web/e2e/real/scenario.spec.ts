// シナリオの実行を、本物のランチャーと例の ECU で通す。ランチャーが Control Server を持てば、
// 実行中は見るだけ: 変数の値は見え、書く部品は理由を言って押せない。時刻の操作（ランチャーの標準入力の命令）と
// 停止は、ふつうの実行と同じく効く。持たなければ飛ばす。
import { expect, test } from '@playwright/test';
import { mkdirSync, rmSync, unlinkSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { GUI_PORT, guiToken } from '../support/gui-server';
import { scaled } from '../support/timeouts';
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

test('real extra recorded signal: select unused ECU signal in Run setup, run scenario, and search/display/hide/restore it in logic analyzer', async ({
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
    !features.includes('record-signals') || !features.includes('timeline'),
    'launcher does not advertise record-signals / timeline',
  );

  const rootDir = process.env['VEHICLE_SIL_GUI_E2E_ROOT'];
  if (!rootDir) throw new Error('VEHICLE_SIL_GUI_E2E_ROOT is not set');
  const scenarioPath = join(rootDir, 'scenarios', 'engine_only_record_e2e.yaml');
  writeFileSync(
    scenarioPath,
    [
      'vehicle_sil_scenario: 1',
      'name: engine only throttle response',
      'steps:',
      '  - name: apply throttle',
      '    at_ms: 0',
      '    set:',
      '      Engine.engine.throttle: 60',
      '  - name: engine reaches speed',
      '    wait_until:',
      '      Engine.engine.speed: { ge: 1800 }',
      '    timeout_ms: 800',
      '  - name: ramp down',
      '    ramp:',
      '      Engine.engine.throttle: { to: 10, over_ms: 200 }',
      '  - name: engine slows down',
      '    expect:',
      '      Engine.engine.speed: { le: 1400 }',
      '    within_ms: 300',
      '',
    ].join('\n'),
    'utf8',
  );

  await openRun(page);
  await stopIfRunning(page);
  try {
    await choose(page, 'simulation.yaml');
    await page.getByTestId('scenario-select').click();
    await page.getByRole('option', { name: 'engine_only_record_e2e.yaml', exact: true }).click();
    await expect(page.getByTestId('check-scenario')).toContainText('engine_only_record_e2e.yaml', {
      timeout: RUN_TIMEOUT,
    });

    // Open Recorded signals disclosure and select Dashboard.dashboard.engine_speed (unused by scenario)
    const disclosure = page.getByTestId('record-signals');
    await expect(disclosure).toBeVisible();
    await disclosure.locator('summary').click();
    const recordCheck = page.waitForResponse(
      (r) =>
        r.url().endsWith('/api/check') &&
        Array.isArray((r.request().postDataJSON() as Record<string, unknown>)['record']) &&
        ((r.request().postDataJSON() as Record<string, unknown>)['record'] as string[]).includes(
          'Dashboard.dashboard.engine_speed',
        ),
    );
    await page
      .getByTestId('record-signal-option-Dashboard.dashboard.engine_speed')
      .getByRole('checkbox')
      .check();
    await recordCheck;
    await expect(page.getByTestId('record-signals-count')).toContainText('1');
    await expect(page.getByTestId('check-outcome')).toContainText('通った', {
      timeout: RUN_TIMEOUT,
    });

    await page.getByTestId('config-card').getByRole('radio', { name: '最速', exact: true }).click();
    const runReqPromise = page.waitForRequest(
      (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
    );
    const runId = await start(page);
    const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
    expect(runBody['record']).toEqual(['Dashboard.dashboard.engine_speed']);

    await expect(result(page, runId)).toContainText('成功', { timeout: RUN_TIMEOUT });

    // Verify timeline JSON marks Dashboard.dashboard.engine_speed as recorded-only (watched: false, recorded: true)
    const tlResp = await page.request.get(new URL(`api/timeline/${runId}`, target.base).href, {
      headers,
    });
    expect(tlResp.status()).toBe(200);
    const tlJson = (await tlResp.json()) as {
      variables: { name: string; written: boolean; watched: boolean; recorded?: boolean }[];
      series: { variable: string; source: string; points: [number, number][] }[];
    };
    const extraVar = tlJson.variables.find((v) => v.name === 'Dashboard.dashboard.engine_speed');
    expect(extraVar).toMatchObject({ written: false, watched: false, recorded: true });
    const extraSeries = tlJson.series.find(
      (s) => s.variable === 'Dashboard.dashboard.engine_speed' && s.source === 'sampled',
    );
    expect(extraSeries).toBeDefined();
    expect(Math.max(...extraSeries!.points.map((p) => p[1]))).toBeGreaterThanOrEqual(1500);

    // Open the recording in the standard GUI logic analyzer
    const scenarioCard = page.getByTestId('scenario-card');
    await expect(scenarioCard.getByTestId('timeline-toggle')).toBeVisible({ timeout: RUN_TIMEOUT });
    await scenarioCard.getByTestId('timeline-toggle').click();
    const timeline = scenarioCard.getByTestId('timeline-element');
    await expect(timeline).toBeVisible();
    await expect(scenarioCard.getByTestId('timeline-visible-count')).toContainText('3 / 3', {
      timeout: RUN_TIMEOUT,
    });
    await expect(timeline.locator('button.lane-hide')).toHaveCount(3);
    await expect(timeline).toContainText('Dashboard.dashboard.engine_speed');

    // Search, hide (×), and restore the extra recorded signal
    await scenarioCard.getByTestId('timeline-signal-search').fill('dashboard.engine_speed');
    await expect(scenarioCard.getByTestId('timeline-visible-count')).toContainText('1 / 3');
    await expect(timeline.locator('button.lane-hide')).toHaveCount(1);
    await timeline.locator('button.lane-hide').first().click();
    await expect(scenarioCard.getByTestId('timeline-visible-count')).toContainText('0 / 3');
    const restoreChip = scenarioCard.locator(
      '[data-testid="timeline-restore-var"][data-variable="Dashboard.dashboard.engine_speed"]',
    );
    await expect(restoreChip).toBeVisible();
    await restoreChip.click();
    await expect(scenarioCard.getByTestId('timeline-visible-count')).toContainText('1 / 3');
  } finally {
    try {
      unlinkSync(scenarioPath);
    } catch {
      // ignore
    }
    await stopRun(page, target, RUN_TIMEOUT);
  }
});

test('representative failing run: extra non-judgment signal plus post-failure tail recording shows failure marker and post-failure waveforms on both signals', async ({
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
    !features.includes('record-signals') ||
      !features.includes('record-after-fail') ||
      !features.includes('timeline'),
    'launcher does not advertise record-signals / record-after-fail / timeline',
  );

  const rootDir = process.env['VEHICLE_SIL_GUI_E2E_ROOT'];
  if (!rootDir) throw new Error('VEHICLE_SIL_GUI_E2E_ROOT is not set');
  const scenarioPath = join(rootDir, 'scenarios', 'representative_post_fail_e2e.yaml');
  writeFileSync(
    scenarioPath,
    [
      'vehicle_sil_scenario: 1',
      'name: representative post-failure tail recording',
      'steps:',
      '  - name: apply throttle',
      '    at_ms: 0',
      '    set:',
      '      Engine.engine.throttle: 50',
      '  - name: wait for engine speed',
      '    wait_until:',
      '      Engine.engine.speed: { ge: 1700 }',
      '    timeout_ms: 200',
      '',
    ].join('\n'),
    'utf8',
  );

  await openRun(page);
  await stopIfRunning(page);
  try {
    await choose(page, 'simulation.yaml');
    await page.getByTestId('scenario-select').click();
    await page
      .getByRole('option', { name: 'representative_post_fail_e2e.yaml', exact: true })
      .click();
    await expect(page.getByTestId('check-scenario')).toContainText(
      'representative_post_fail_e2e.yaml',
      { timeout: RUN_TIMEOUT },
    );

    // Configure 300 ms post-failure tail recording
    const tailInput = page.getByTestId('record-after-fail-ms');
    await expect(tailInput).toBeVisible();
    const tailCheck = page.waitForResponse(
      (r) =>
        r.url().endsWith('/api/check') &&
        (r.request().postDataJSON() as Record<string, unknown>)['record_after_fail_ms'] === 300,
    );
    await tailInput.fill('300');
    await tailCheck;

    // Select extra non-judgment signal Dashboard.dashboard.engine_speed
    const disclosure = page.getByTestId('record-signals');
    await expect(disclosure).toBeVisible();
    await disclosure.locator('summary').click();
    const dashSpeedBox = page
      .getByTestId('record-signal-option-Dashboard.dashboard.engine_speed')
      .getByRole('checkbox');
    if (!(await dashSpeedBox.isChecked())) {
      const recordCheck = page.waitForResponse(
        (r) =>
          r.url().endsWith('/api/check') &&
          Array.isArray((r.request().postDataJSON() as Record<string, unknown>)['record']) &&
          ((r.request().postDataJSON() as Record<string, unknown>)['record'] as string[]).includes(
            'Dashboard.dashboard.engine_speed',
          ),
      );
      await dashSpeedBox.check();
      await recordCheck;
    }
    await expect(page.getByTestId('record-signals-count')).toContainText('1');
    await expect(page.getByTestId('check-outcome')).toContainText('通った', {
      timeout: RUN_TIMEOUT,
    });

    await page.getByTestId('config-card').getByRole('radio', { name: '最速', exact: true }).click();
    const runReqPromise = page.waitForRequest(
      (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
    );
    const runId = await start(page);
    const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
    expect(runBody['record']).toEqual(['Dashboard.dashboard.engine_speed']);
    expect(runBody['record_after_fail_ms']).toBe(300);

    await expect(result(page, runId)).toContainText('失敗', { timeout: RUN_TIMEOUT });

    // Verify timeline JSON: failure at 200 ms, recorded_to_ms = 500 ms, post-failure waveform changes on BOTH signals
    const tlResp = await page.request.get(new URL(`api/timeline/${runId}`, target.base).href, {
      headers,
    });
    expect(tlResp.status()).toBe(200);
    const tlJson = (await tlResp.json()) as {
      scenario: { status: string; recorded_to_ms: number; over_at_ms: number };
      steps: {
        index: number;
        status: string;
        at_ms: number | null;
        failed: { at_ms: number } | null;
      }[];
      variables: { name: string; written: boolean; watched: boolean; recorded?: boolean }[];
      series: {
        variable: string;
        source: string;
        last_ms: number | null;
        points: [number, number][];
      }[];
    };
    expect(tlJson.scenario.status).toBe('failed');
    expect(tlJson.scenario.recorded_to_ms).toBe(500);
    const failedStep = tlJson.steps.find((s) => s.status === 'failed');
    expect(failedStep?.failed?.at_ms).toBe(200);

    const extraVar = tlJson.variables.find((v) => v.name === 'Dashboard.dashboard.engine_speed');
    expect(extraVar).toMatchObject({ written: false, watched: false, recorded: true });

    for (const signalName of ['Engine.engine.speed', 'Dashboard.dashboard.engine_speed']) {
      const series = tlJson.series.find((s) => s.variable === signalName && s.source === 'sampled');
      expect(series, `sampled series for ${signalName}`).toBeDefined();
      expect(series!.last_ms).toBeGreaterThan(200);
      const beforeOrAt = series!.points.filter((p) => p[0] <= 200);
      const afterFail = series!.points.filter((p) => p[0] > 200 && p[0] <= 500);
      expect(afterFail.length, `${signalName} should have post-failure changes`).toBeGreaterThan(0);
      const valAtFail = beforeOrAt.at(-1)?.[1] ?? 0;
      const valAtEnd = afterFail.at(-1)![1];
      expect(valAtEnd, `${signalName} should continue rising after failure`).toBeGreaterThan(
        valAtFail,
      );
    }

    // Standard GUI logic analyzer auto-expands on failure with red failure marker and Around failure framing
    const scenarioCard = page.getByTestId('scenario-card');
    await expect(scenarioCard.getByTestId('timeline-toggle')).toHaveAttribute(
      'aria-expanded',
      'true',
      { timeout: RUN_TIMEOUT },
    );
    await expect(scenarioCard.getByTestId('timeline-failure-strip')).toContainText('200 ms', {
      timeout: RUN_TIMEOUT,
    });
    const timeline = scenarioCard.getByTestId('timeline-element');
    await expect(timeline).toBeVisible();
    // SVG vertical .fail-line has 0 bounding-box width; verify it is rendered across all lanes with positive height
    await expect(timeline.locator('.fail-line')).toHaveCount(4);
    const failLineMetrics = await timeline
      .locator('.fail-line')
      .first()
      .evaluate((el) => {
        const style = getComputedStyle(el);
        return {
          x1: Number(el.getAttribute('x1')),
          y1: Number(el.getAttribute('y1')),
          y2: Number(el.getAttribute('y2')),
          display: style.display,
          visibility: style.visibility,
        };
      });
    expect(failLineMetrics.x1).toBeGreaterThan(0);
    expect(failLineMetrics.y2).toBeGreaterThan(failLineMetrics.y1);
    expect(failLineMetrics.display).not.toBe('none');
    expect(failLineMetrics.visibility).not.toBe('hidden');
    await expect(scenarioCard.getByTestId('timeline-visible-count')).toContainText('3 / 3');
    await expect(timeline).toContainText('Dashboard.dashboard.engine_speed');

    const aroundBtn = scenarioCard.getByTestId('timeline-window-around-failure');
    await expect(aroundBtn).toBeVisible();
    await aroundBtn.click();
    const aroundView = await timeline.evaluate((el) => {
      const t = el as HTMLElement & { from: number; to: number; cursor: number | null };
      return { from: t.from, to: t.to, cursor: t.cursor };
    });
    expect(aroundView.from).toBe(0);
    expect(aroundView.to).toBeGreaterThanOrEqual(500);
    expect(aroundView.cursor).toBe(200);

    // Non-judgment signal Dashboard.dashboard.engine_speed is searchable, hideable, and restorable
    await scenarioCard.getByTestId('timeline-signal-search').fill('dashboard.engine_speed');
    await expect(scenarioCard.getByTestId('timeline-visible-count')).toContainText('1 / 3');
    await expect(timeline.locator('button.lane-hide')).toHaveCount(1);
    await timeline.locator('button.lane-hide').first().click();
    await expect(scenarioCard.getByTestId('timeline-visible-count')).toContainText('0 / 3');
    const restoreChip = scenarioCard.locator(
      '[data-testid="timeline-restore-var"][data-variable="Dashboard.dashboard.engine_speed"]',
    );
    await expect(restoreChip).toBeVisible();
    await restoreChip.click();
    await expect(scenarioCard.getByTestId('timeline-visible-count')).toContainText('1 / 3');
  } finally {
    try {
      unlinkSync(scenarioPath);
    } catch {
      // ignore
    }
    await stopRun(page, target, RUN_TIMEOUT);
  }
});

test('real manual run with Recorded signals: writes scenario-free timeline and opens cleanly in standard GUI logic analyzer', async ({
  page,
}) => {
  test.setTimeout(90_000);
  const target: Target = { base: `http://127.0.0.1:${GUI_PORT}/`, token: guiToken() };
  const headers = { Authorization: `Bearer ${target.token}`, Origin: new URL(target.base).origin };
  const app = (await (
    await page.request.get(new URL('api/app', target.base).href, { headers })
  ).json()) as {
    variables?: { available: boolean };
    launcher?: { version?: { features?: string[] } };
  };
  const features = app.launcher?.version?.features ?? [];
  test.skip(
    !app.variables?.available ||
      !features.includes('record-signals') ||
      !features.includes('timeline') ||
      !features.includes('manual-timeline'),
    'launcher does not advertise variables / record-signals / timeline / manual-timeline',
  );

  await openRun(page);
  await stopIfRunning(page);
  try {
    await choose(page, 'simulation.yaml');
    // Ensure Scenario = None (manual run)
    await page.getByTestId('scenario-select').click();
    await page.getByRole('option', { name: 'なし（自分で操作する）', exact: true }).click();
    await expect(page.getByTestId('record-after-fail-ms')).toHaveCount(0);

    // Select Engine.engine.speed and Dashboard.dashboard.engine_speed in Recorded signals
    const disclosure = page.getByTestId('record-signals');
    await expect(disclosure).toBeVisible();
    await disclosure.locator('summary').click();
    for (const signalName of ['Engine.engine.speed', 'Dashboard.dashboard.engine_speed']) {
      const box = page.getByTestId(`record-signal-option-${signalName}`).getByRole('checkbox');
      if (!(await box.isChecked())) {
        const recCheck = page.waitForResponse(
          (r) =>
            r.url().endsWith('/api/check') &&
            Array.isArray((r.request().postDataJSON() as Record<string, unknown>)['record']) &&
            (
              (r.request().postDataJSON() as Record<string, unknown>)['record'] as string[]
            ).includes(signalName),
        );
        await box.check();
        await recCheck;
      }
    }
    await expect(page.getByTestId('record-signals-count')).toContainText('2');
    await expect(page.getByTestId('check-outcome')).toContainText('通った', {
      timeout: RUN_TIMEOUT,
    });

    // Start paused at virtual time 0, write throttle changes, step to 600 ms, then stop cleanly
    await page.getByTestId('start-paused').getByRole('checkbox').check();
    await page.getByTestId('config-card').getByRole('radio', { name: '最速', exact: true }).click();
    const runReqPromise = page.waitForRequest(
      (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
    );
    const runId = await start(page);
    const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
    expect(runBody['scenario']).toBeUndefined();
    expect(runBody['record']).toEqual(['Dashboard.dashboard.engine_speed', 'Engine.engine.speed']);

    await waitForVariables(page, target, 'Engine', RUN_TIMEOUT);
    const w1 = await page.request.post(new URL('api/write', target.base).href, {
      headers,
      data: { run_id: runId, at_ms: 0, writes: { Engine: { 'engine.throttle': 60 } } },
    });
    expect(w1.status(), await w1.text()).toBe(200);
    const w2 = await page.request.post(new URL('api/write', target.base).href, {
      headers,
      data: { run_id: runId, at_ms: 350, writes: { Engine: { 'engine.throttle': 15 } } },
    });
    expect(w2.status(), await w2.text()).toBe(200);

    await page.getByTestId('step-ms').fill('600');
    await page.getByTestId('step').click();
    await expect(page.getByTestId('sim-time')).toContainText('0.600 s', { timeout: RUN_TIMEOUT });

    await press(stopButton(page));
    await expect(result(page, runId)).toBeVisible({ timeout: RUN_TIMEOUT });

    // Verify manual-run timeline JSON has scenario: null, empty steps/lines, and real sampled waveforms
    const tlResp = await page.request.get(new URL(`api/timeline/${runId}`, target.base).href, {
      headers,
    });
    expect(tlResp.status()).toBe(200);
    const tlJson = (await tlResp.json()) as {
      scenario: unknown;
      recorded_to_ms: number | null;
      steps: unknown[];
      lines: unknown[];
      variables: { name: string; written: boolean; watched: boolean; recorded?: boolean }[];
      series: { variable: string; source: string; points: [number, number][] }[];
    };
    expect(tlJson.scenario).toBeNull();
    expect(tlJson.recorded_to_ms).toBeGreaterThanOrEqual(500);
    expect(tlJson.steps).toEqual([]);
    expect(tlJson.lines).toEqual([]);
    for (const signalName of ['Engine.engine.speed', 'Dashboard.dashboard.engine_speed']) {
      const v = tlJson.variables.find((item) => item.name === signalName);
      expect(v).toMatchObject({ written: false, watched: false, recorded: true });
      const s = tlJson.series.find(
        (item) => item.variable === signalName && item.source === 'sampled',
      );
      expect(s, `sampled series for ${signalName}`).toBeDefined();
      expect(s!.points.length).toBeGreaterThan(2);
      expect(Math.max(...s!.points.map((p) => p[1]))).toBeGreaterThan(1000);
    }

    // Standard GUI logic analyzer opens automatically in the manual timeline card with zero scenario chrome or warning
    const card = page.getByTestId('manual-timeline-card');
    await expect(card).toBeVisible({ timeout: RUN_TIMEOUT });
    await expect(page.getByTestId('scenario-card')).toHaveCount(0);
    await expect(card.getByTestId('manual-timeline-link')).toHaveAttribute(
      'href',
      new RegExp(`%2Fapi%2Ftimeline%2F${runId}&`),
    );
    await expect(card.getByTestId('timeline-toggle')).toHaveAttribute('aria-expanded', 'true', {
      timeout: RUN_TIMEOUT,
    });
    await expect(card.getByTestId('timeline-failure-strip')).toHaveCount(0);
    const timeline = card.getByTestId('timeline-element');
    await expect(timeline).toBeVisible();
    await expect(timeline.locator('[data-testid="not-decided"]')).toHaveCount(0);
    await expect(card.getByTestId('timeline-visible-count')).toContainText('2 / 2', {
      timeout: RUN_TIMEOUT,
    });
    await expect(timeline).toContainText('Engine.engine.speed');
    await expect(timeline).toContainText('Dashboard.dashboard.engine_speed');

    // Search, hide (×), and restore work cleanly on the manual timeline
    await card.getByTestId('timeline-signal-search').fill('dashboard.engine_speed');
    await expect(card.getByTestId('timeline-visible-count')).toContainText('1 / 2');
    await expect(timeline.locator('button.lane-hide')).toHaveCount(1);
    await timeline.locator('button.lane-hide').first().click();
    await expect(card.getByTestId('timeline-visible-count')).toContainText('0 / 2');
    const restoreChip = card.locator(
      '[data-testid="timeline-restore-var"][data-variable="Dashboard.dashboard.engine_speed"]',
    );
    await expect(restoreChip).toBeVisible();
    await restoreChip.click();
    await expect(card.getByTestId('timeline-visible-count')).toContainText('1 / 2');
  } finally {
    await stopRun(page, target, RUN_TIMEOUT);
  }
});

test('real vehicle_sil_launch: virtual-timestamped ECU events correlate with waveforms on engine_disconnect', async ({
  page,
}) => {
  test.setTimeout(scaled(90_000));
  const target: Target = { base: `http://127.0.0.1:${GUI_PORT}/`, token: guiToken() };
  const headers = { Authorization: `Bearer ${target.token}`, Origin: new URL(target.base).origin };
  const appResp = await page.request.get(new URL('api/app', target.base).href, { headers });
  const appJson = (await appResp.json()) as {
    launcher?: { version?: { features?: string[] } };
  };
  const features = appJson.launcher?.version?.features ?? [];
  test.skip(
    !features.includes('record-events') || !features.includes('timeline'),
    'launcher does not advertise record-events / timeline',
  );

  await openRun(page);
  await stopIfRunning(page);
  try {
    await choose(page, 'simulation.yaml');
    const scCheck = page.waitForResponse(
      (r) =>
        r.url().endsWith('/api/check') &&
        (r.request().postDataJSON() as Record<string, unknown>)['scenario'] ===
          'scenarios/engine_disconnect.yaml',
    );
    await page.getByRole('combobox', { name: 'シナリオ' }).click();
    await page.getByRole('option', { name: 'engine_disconnect.yaml', exact: true }).click();
    await scCheck;

    // Opt in to recording virtual-timestamped structured ECU events
    const recordEventsCb = page.getByTestId('record-events').getByRole('checkbox');
    await expect(recordEventsCb).toBeVisible();
    await expect(recordEventsCb).not.toBeChecked();
    const evCheck = page.waitForResponse(
      (r) =>
        r.url().endsWith('/api/check') &&
        (r.request().postDataJSON() as Record<string, unknown>)['record_events'] === true,
    );
    await recordEventsCb.check();
    await evCheck;

    await expect(page.getByTestId('check-outcome')).toContainText('通った', {
      timeout: RUN_TIMEOUT,
    });

    const runReqPromise = page.waitForRequest(
      (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
    );
    const runId = await start(page);
    const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
    expect(runBody['record_events']).toBe(true);
    await expect(result(page, runId)).toBeVisible({ timeout: RUN_TIMEOUT });

    // Verify timeline JSON contains grounded virtual-timestamped ECU events including bus disconnect at 300 ms
    const tlResp = await page.request.get(new URL(`api/timeline/${runId}`, target.base).href, {
      headers,
    });
    expect(tlResp.status()).toBe(200);
    const tlJson = (await tlResp.json()) as {
      events?: { at_ms: number; participant: string; level: string; text: string }[];
      limits?: { events_dropped?: unknown[] };
    };
    expect(Array.isArray(tlJson.events)).toBe(true);
    const events = tlJson.events ?? [];
    expect(events.length).toBeGreaterThanOrEqual(2);
    const disconnectEvent = events.find((e) => e.participant === 'Engine' && e.at_ms === 300);
    expect(
      disconnectEvent,
      `expected Engine event at 300 ms in ${JSON.stringify(events)}`,
    ).toBeDefined();

    // Open the inline logic analyzer on the passed Scenario card
    const card = page.getByTestId('scenario-card');
    await expect(card).toBeVisible({ timeout: RUN_TIMEOUT });
    const toggle = card.getByTestId('timeline-toggle');
    if ((await toggle.getAttribute('aria-expanded')) !== 'true') {
      await toggle.click();
    }
    const timeline = card.getByTestId('timeline-element');
    await expect(timeline).toBeVisible({ timeout: RUN_TIMEOUT });

    // Severity ticks and synchronized ECU Events table render inside the embedded logic analyzer
    await expect(timeline.locator('[data-testid="ecu-event-tick"]').first()).toBeVisible({
      timeout: RUN_TIMEOUT,
    });
    const rows = timeline.locator('[data-testid="ecu-event-row"]');
    await expect(rows.first()).toBeVisible({ timeout: RUN_TIMEOUT });

    // Click the 300 ms Engine disconnect event row and verify waveform cursor pins to 300.0 ms
    const disconnectRow = timeline
      .locator('[data-testid="ecu-event-row"][data-event-at="300"]')
      .first();
    await expect(disconnectRow).toBeVisible();
    await disconnectRow.click();
    await expect(card.getByTestId('timeline-readout')).toContainText('300.0 ms');
    await expect(disconnectRow).toHaveAttribute('aria-selected', 'true');
    await expect(disconnectRow.locator('.ev-delta')).toHaveText('0 ms');
  } finally {
    await stopRun(page, target, RUN_TIMEOUT);
  }
});

test('real vehicle_sil_launch: virtual-timestamped CAN frames decode J1939 and correlate with waveforms on throttle_step', async ({
  page,
}) => {
  test.setTimeout(scaled(90_000));
  const target: Target = { base: `http://127.0.0.1:${GUI_PORT}/`, token: guiToken() };
  const headers = { Authorization: `Bearer ${target.token}`, Origin: new URL(target.base).origin };
  const appResp = await page.request.get(new URL('api/app', target.base).href, { headers });
  const appJson = (await appResp.json()) as {
    launcher?: { version?: { features?: string[] } };
  };
  const features = appJson.launcher?.version?.features ?? [];
  test.skip(
    !features.includes('record-can') || !features.includes('timeline'),
    'launcher does not advertise record-can / timeline',
  );

  await openRun(page);
  await stopIfRunning(page);
  try {
    await choose(page, 'simulation.yaml');
    const scCheck = page.waitForResponse(
      (r) =>
        r.url().endsWith('/api/check') &&
        (r.request().postDataJSON() as Record<string, unknown>)['scenario'] ===
          'scenarios/throttle_step.yaml',
    );
    await page.getByRole('combobox', { name: 'シナリオ' }).click();
    await page.getByRole('option', { name: 'throttle_step.yaml', exact: true }).click();
    await scCheck;

    // Opt in to recording virtual-timestamped CAN frames
    const recordCanCb = page.getByTestId('record-can').getByRole('checkbox');
    await expect(recordCanCb).toBeVisible();
    await expect(recordCanCb).not.toBeChecked();
    const canCheck = page.waitForResponse(
      (r) =>
        r.url().endsWith('/api/check') &&
        (r.request().postDataJSON() as Record<string, unknown>)['record_can'] === true,
    );
    await recordCanCb.check();
    await canCheck;

    await expect(page.getByTestId('check-outcome')).toContainText('通った', {
      timeout: RUN_TIMEOUT,
    });

    const runReqPromise = page.waitForRequest(
      (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
    );
    const runId = await start(page);
    const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
    expect(runBody['record_can']).toBe(true);
    await expect(result(page, runId)).toBeVisible({ timeout: RUN_TIMEOUT });

    // Verify timeline JSON contains real grounded virtual-timestamped J1939 CAN frames on J1939_CAN
    const tlResp = await page.request.get(new URL(`api/timeline/${runId}`, target.base).href, {
      headers,
    });
    expect(tlResp.status()).toBe(200);
    const tlJson = (await tlResp.json()) as {
      can_frames?: {
        at_ms: number;
        network: string;
        id: number;
        extended: boolean;
        dlc: number;
        data: string;
      }[];
      limits?: { can_dropped?: unknown[] };
    };
    expect(Array.isArray(tlJson.can_frames)).toBe(true);
    const frames = tlJson.can_frames ?? [];
    expect(frames.length).toBeGreaterThan(0);
    const engineSpeedFrame = frames.find(
      (f) => f.network === 'J1939_CAN' && f.extended && ((f.id >>> 8) & 0xffff) === 0xff10,
    );
    expect(
      engineSpeedFrame,
      `expected J1939 engine speed (PGN 0xFF10) frame in ${JSON.stringify(frames.slice(0, 5))}`,
    ).toBeDefined();
    const addrClaimFrame = frames.find(
      (f) => f.network === 'J1939_CAN' && f.extended && ((f.id >>> 16) & 0xff) === 0xee,
    );
    expect(
      addrClaimFrame,
      `expected J1939 Address Claimed (PGN 0xEE00) frame in ${JSON.stringify(frames.slice(0, 5))}`,
    ).toBeDefined();

    // Open the inline logic analyzer on the passed Scenario card
    const card = page.getByTestId('scenario-card');
    await expect(card).toBeVisible({ timeout: RUN_TIMEOUT });
    const toggle = card.getByTestId('timeline-toggle');
    if ((await toggle.getAttribute('aria-expanded')) !== 'true') {
      await toggle.click();
    }
    const timeline = card.getByTestId('timeline-element');
    await expect(timeline).toBeVisible({ timeout: RUN_TIMEOUT });

    // Synchronized CAN Frames table renders J1939 decode (PGN 0xFF10) inside the embedded logic analyzer
    const rows = timeline.locator('[data-testid="can-frame-row"]');
    await expect(rows.first()).toBeVisible({ timeout: RUN_TIMEOUT });
    await timeline.locator('[data-testid="can-frames-search"]').fill('0xFF10');
    await expect(rows.first()).toBeVisible();
    await expect(rows.first()).toContainText('PGN 0xFF10');

    // Click an engine-speed CAN frame row and verify waveform cursor pins to its virtual time with Δt = 0 ms
    const targetRow = rows.nth(1);
    await expect(targetRow).toBeVisible();
    await targetRow.click();
    await expect(targetRow).toHaveAttribute('aria-selected', 'true');
    await expect(targetRow.locator('.can-delta')).toHaveText('0 ms');
  } finally {
    await stopRun(page, target, RUN_TIMEOUT);
  }
});
