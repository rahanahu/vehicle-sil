// End-to-end regressions for the Standard GUI Logic Analyzer (Option A + P0/P1/P2 refinements):
// 1. Zoomed in -> jump to failure -> exact failure virtual timestamp & pre-failure waveforms/events ->
//    signal search/presets/inline hide/restore & collapse/reopen preserve zoom, cursor, and selected event.
// 2. Simultaneous events at the same virtual timestamp list all sibling steps with explicit non-causal wording.
// 3. Eventless timeline (steps: []) omits the event lane cleanly without misleading "0 steps" chrome.
// 4. Run switch clears previous event selection, cursor, search query, presets, and hidden-signal state.
import { Page, expect, test } from '@playwright/test';
import { CONFIGS, GuiServer, REPLAY_LAUNCHER, launchGuiServer } from './support/gui-server';
import { findLayoutProblems } from './support/layout';
import { RUN_TIMEOUT, inSync } from './support/run-page';

const FAILS = 'scenarios/fails.yaml';

let server: GuiServer;
let base: string;

/** Realistic multi-signal failed timeline with simultaneous events (540 ms ×3) and end-edge bool flip (640 ms). */
function makeRepresentativeTimeline() {
  return {
    vehicle_sil_timeline: 1,
    vehicle_sil: '0.7.0',
    stage: 'run',
    file: 'simulation.yaml',
    variant: 'wet_road',
    params: [],
    time_step_ms: 10,
    scenario: {
      file: 'scenarios/aeb_emergency_braking.yaml',
      name: 'AEB emergency braking with override disturbance',
      status: 'failed',
      over_at_ms: 650,
      recorded_to_ms: 640,
      end_ms: null,
    },
    variables: [
      {
        name: 'ADAS.aeb.active',
        participant: 'ADAS',
        type: 'bool',
        causality: 'input',
        unit: '',
        min: null,
        max: null,
        items: [],
        ecu_step_ms: 10,
        written: true,
        watched: true,
      },
      {
        name: 'ADAS.aeb.state',
        participant: 'ADAS',
        type: 'enum',
        causality: 'output',
        unit: '',
        min: null,
        max: null,
        items: [
          { name: 'standby', value: 0 },
          { name: 'braking', value: 2 },
          { name: 'degraded', value: 3 },
        ],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'Powertrain.motor.torque',
        participant: 'Powertrain',
        type: 'real',
        causality: 'input',
        unit: 'Nm',
        min: -300,
        max: 400,
        items: [],
        ecu_step_ms: 10,
        written: true,
        watched: true,
      },
      {
        name: 'Brake.hyd.target_pressure',
        participant: 'Brake',
        type: 'real',
        causality: 'input',
        unit: 'bar',
        min: 0,
        max: 180,
        items: [],
        ecu_step_ms: 10,
        written: true,
        watched: true,
      },
      {
        name: 'Brake.hyd.pressure',
        participant: 'Brake',
        type: 'real',
        causality: 'output',
        unit: 'bar',
        min: 0,
        max: 180,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'Brake.diag.fault_latch',
        participant: 'Brake',
        type: 'bool',
        causality: 'output',
        unit: '',
        min: null,
        max: null,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
    ],
    steps: [
      {
        index: 1,
        line: 5,
        kind: 'set',
        name: 'enable AEB',
        text: 'set ADAS.aeb.active = true',
        status: 'passed',
        start_ms: 0,
        at_ms: 0,
        end_ms: 0,
        code: null,
        message: null,
        lines: [],
        watch: null,
        failed: null,
      },
      {
        index: 2,
        line: 9,
        kind: 'ramp',
        name: 'ramp torque down',
        text: 'ramp Powertrain.motor.torque to 0 over 200 ms',
        status: 'passed',
        start_ms: 200,
        at_ms: 400,
        end_ms: 400,
        code: null,
        message: null,
        lines: [],
        watch: null,
        failed: null,
      },
      {
        index: 8,
        line: 20,
        kind: 'set',
        name: 'override pulse',
        text: 'set ADAS.aeb.active = false',
        status: 'passed',
        start_ms: 540,
        at_ms: 540,
        end_ms: 540,
        code: null,
        message: null,
        lines: [],
        watch: null,
        failed: null,
      },
      {
        index: 9,
        line: 24,
        kind: 'set',
        name: 'torque disturbance',
        text: 'set Powertrain.motor.torque = 120',
        status: 'passed',
        start_ms: 540,
        at_ms: 540,
        end_ms: 540,
        code: null,
        message: null,
        lines: [],
        watch: null,
        failed: null,
      },
      {
        index: 10,
        line: 28,
        kind: 'ramp',
        name: 'braking pressure tracking',
        text: 'ramp Brake.hyd.target_pressure to 120 over 400 ms',
        status: 'failed',
        start_ms: 540,
        at_ms: 640,
        end_ms: 940,
        code: 'scenario-failed',
        message:
          'at 640 ms Brake.hyd.pressure near Brake.hyd.target_pressure ± 15, was 88 while 118',
        lines: [],
        watch: {
          mode: 'during',
          from_ms: 540,
          to_ms: 940,
          conditions: [
            {
              variable: 'Brake.hyd.pressure',
              compare: [
                { op: 'near', value: null, variable: 'Brake.hyd.target_pressure', tolerance: 15 },
              ],
            },
            {
              variable: 'Brake.diag.fault_latch',
              compare: [{ op: 'eq', value: false, tolerance: null }],
            },
          ],
        },
        failed: {
          at_ms: 640,
          code: 'scenario-failed',
          conditions: [
            {
              condition: 0,
              value: 88,
              sample_ms: 640,
              compared: [{ variable: 'Brake.hyd.target_pressure', value: 118, sample_ms: 640 }],
            },
            {
              condition: 1,
              value: true,
              sample_ms: 640,
            },
          ],
          writes: [],
          write: null,
        },
      },
    ],
    series: [
      {
        variable: 'ADAS.aeb.active',
        source: 'written',
        encoding: 'all',
        columns: ['at_ms', 'value'],
        samples: 2,
        first_ms: 0,
        last_ms: 540,
        not_applied_from: null,
        points: [
          [0, true],
          [540, false],
        ],
      },
      {
        variable: 'ADAS.aeb.state',
        source: 'sampled',
        encoding: 'changes',
        columns: ['at_ms', 'value'],
        samples: 3,
        first_ms: 0,
        last_ms: 640,
        not_applied_from: null,
        points: [
          [0, 0],
          [200, 2],
          [620, 3],
        ],
      },
      {
        variable: 'Powertrain.motor.torque',
        source: 'written',
        encoding: 'all',
        columns: ['at_ms', 'value'],
        samples: 3,
        first_ms: 0,
        last_ms: 540,
        not_applied_from: null,
        points: [
          [0, 200],
          [400, 0],
          [540, 120],
        ],
      },
      {
        variable: 'Brake.hyd.target_pressure',
        source: 'sampled',
        encoding: 'changes',
        columns: ['at_ms', 'value'],
        samples: 3,
        first_ms: 0,
        last_ms: 640,
        not_applied_from: null,
        points: [
          [0, 0],
          [540, 90],
          [640, 118],
        ],
      },
      {
        variable: 'Brake.hyd.pressure',
        source: 'sampled',
        encoding: 'changes',
        columns: ['at_ms', 'value'],
        samples: 3,
        first_ms: 0,
        last_ms: 640,
        not_applied_from: null,
        points: [
          [0, 0],
          [540, 86],
          [640, 88],
        ],
      },
      {
        variable: 'Brake.diag.fault_latch',
        source: 'sampled',
        encoding: 'changes',
        columns: ['at_ms', 'value'],
        samples: 2,
        first_ms: 0,
        last_ms: 640,
        not_applied_from: null,
        points: [
          [0, false],
          [640, true],
        ],
      },
    ],
    lines: [],
    findings: [],
    limits: { points_per_series: 10000, points_total: 100000, dropped: [] },
  };
}

/** Eventless timeline (steps: []) for verifying pure waveform mode without empty event lane. */
function makeEventlessTimeline() {
  return {
    vehicle_sil_timeline: 1,
    vehicle_sil: '0.7.0',
    stage: 'run',
    file: 'simulation.yaml',
    variant: null,
    params: [],
    time_step_ms: 10,
    scenario: {
      file: 'scenarios/passive_capture.yaml',
      name: 'Passive cruise recording',
      status: 'passed',
      over_at_ms: 510,
      recorded_to_ms: 500,
      end_ms: 500,
    },
    variables: [
      {
        name: 'Chassis.veh.speed',
        participant: 'Chassis',
        type: 'real',
        causality: 'output',
        unit: 'km/h',
        min: 0,
        max: 160,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'Brake.hyd.pressure',
        participant: 'Brake',
        type: 'real',
        causality: 'output',
        unit: 'bar',
        min: 0,
        max: 180,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
    ],
    steps: [],
    series: [
      {
        variable: 'Chassis.veh.speed',
        source: 'sampled',
        encoding: 'changes',
        columns: ['at_ms', 'value'],
        samples: 2,
        first_ms: 0,
        last_ms: 500,
        not_applied_from: null,
        points: [
          [0, 80],
          [250, 82],
        ],
      },
      {
        variable: 'Brake.hyd.pressure',
        source: 'sampled',
        encoding: 'changes',
        columns: ['at_ms', 'value'],
        samples: 1,
        first_ms: 0,
        last_ms: 500,
        not_applied_from: null,
        points: [[0, 0]],
      },
    ],
    lines: [],
    findings: [],
    limits: { points_per_series: 10000, points_total: 100000, dropped: [] },
  };
}

test.beforeAll(async () => {
  test.skip(test.info().project.name !== 'static', 'only with the built screen');
  server = await launchGuiServer({
    launcher: REPLAY_LAUNCHER,
    root: CONFIGS,
    env: { REPLAY_TICK_MS: '10' },
    port: 0,
  });
  base = new URL('/', server.url).href;
});

test.afterAll(async () => {
  await server?.stop();
});

async function startFailingRun(page: Page): Promise<string> {
  const response = await page.request.post(new URL('api/run', base).href, {
    headers: { Authorization: `Bearer ${server.token}`, Origin: new URL(base).origin },
    data: { file: 'simulation.yaml', scenario: FAILS },
  });
  expect(response.status(), await response.text()).toBe(200);
  return ((await response.json()) as { run_id: string }).run_id;
}

test('logic analyzer: zoom-preserving failure jump, signal search/hide/restore preservation, simultaneous events, and responsive layout', async ({
  page,
}) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  const fixtureJson = JSON.stringify(makeRepresentativeTimeline());
  await page.route('**/api/timeline/*', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: fixtureJson,
    });
  });

  const runId = await startFailingRun(page);
  await page.goto(`${base}#token=${server.token}`);
  await inSync(page);

  const card = page.getByTestId('scenario-card');
  await expect(card.getByTestId('scenario-timeline')).toHaveAttribute(
    'href',
    new RegExp(`%2Fapi%2Ftimeline%2F${runId}&`),
    { timeout: RUN_TIMEOUT },
  );

  // Full-width scenario card gives the logic analyzer wide horizontal breathing room.
  const cardBox = (await card.boundingBox())!;
  expect(cardBox.width).toBeGreaterThan(900);

  // Above-the-fold compact failure summary strip is visible immediately.
  const strip = card.getByTestId('timeline-failure-strip');
  await expect(strip).toBeVisible();
  await expect(strip).toContainText('640 ms (#10 braking pressure tracking)');

  const timeline = card.getByTestId('timeline-element');
  await expect(timeline).toBeVisible();

  // Event lane sits BELOW variable waveform lanes on the shared virtual-time axis.
  const laneOrder = await timeline.evaluate((el) => {
    const chart = el.shadowRoot!.getElementById('chart')!;
    return [...chart.children]
      .filter((c) => c.classList.contains('lane'))
      .map((c) =>
        c.classList.contains('axis') ? 'axis' : c.classList.contains('steps') ? 'steps' : 'var',
      );
  });
  expect(laneOrder[0]).toBe('axis');
  expect(laneOrder.at(-1)).toBe('steps');

  // End-edge bool transition at recorded_to_ms (640 ms) draws crisp → true badge.
  await expect(timeline.locator('.state-edge-label')).toContainText('→ true');

  // 1. Zoom into [150, 300] (150 ms span), then click "失敗時刻へ移動" -> preserves exact 150 ms span!
  await timeline.evaluate((el) => {
    const t = el as HTMLElement & { from: number; to: number };
    t.from = 150;
    t.to = 300;
  });
  await expect(timeline.locator('#range')).toContainText('150 ms から 300 ms');

  await card.getByTestId('timeline-jump-failure').click();
  const afterJump = await timeline.evaluate((el) => {
    const t = el as HTMLElement & {
      from: number;
      to: number;
      cursor: number | null;
      step: number | null;
    };
    return { from: t.from, to: t.to, cursor: t.cursor, step: t.step };
  });
  expect(afterJump.to - afterJump.from).toBeCloseTo(150, 1);
  expect(afterJump.from).toBeCloseTo(520, 1);
  expect(afterJump.to).toBeCloseTo(670, 1);
  expect(afterJump.cursor).toBe(640);
  expect(afterJump.step).toBe(10);
  await expect(card.getByTestId('timeline-readout')).toContainText('カーソル: 640.0 ms');
  await expect(card.getByTestId('timeline-readout')).toContainText('選んだ手順: 10');

  // Moving the pointer to the filter bar must NEVER drag the pinned cursor away!
  const search = card.getByTestId('timeline-signal-search');
  await search.hover();
  const cursorAfterHover = await timeline.evaluate(
    (el) => (el as HTMLElement & { cursor: number | null }).cursor,
  );
  expect(cursorAfterHover).toBe(640);

  // 2. Signal search, presets ("失敗直前に変化" includes ADAS.aeb.state!), inline hide & inline restore:
  await card.getByTestId('timeline-preset-changed').click();
  await expect(timeline.locator('.var-name')).toContainText([
    'ADAS.aeb.state',
    'Brake.hyd.pressure',
  ]);
  await card.getByTestId('timeline-preset-all').click();
  await expect(card.getByTestId('timeline-visible-count')).toHaveText('6 / 6');

  await search.fill('Brake');
  await expect(card.getByTestId('timeline-visible-count')).toHaveText('3 / 6');
  await card.getByTestId('timeline-signal-search-clear').click();
  await expect(card.getByTestId('timeline-visible-count')).toHaveText('6 / 6');

  // Click inline × on first variable lane -> hidden-strip appears inline -> restore it!
  await timeline.locator('button.lane-hide').first().click();
  const hiddenStrip = card.getByTestId('timeline-hidden-strip');
  await expect(hiddenStrip).toBeVisible();
  await expect(card.getByTestId('timeline-visible-count')).toHaveText('5 / 6');
  await hiddenStrip.getByTestId('timeline-restore-var').first().click();
  await expect(hiddenStrip).toHaveCount(0);
  await expect(card.getByTestId('timeline-visible-count')).toHaveText('6 / 6');

  // Collapse & reopen keeps the element and exact zoom/cursor/step intact!
  await card.getByTestId('timeline-toggle').click();
  await expect(timeline).toBeHidden();
  await card.getByTestId('timeline-toggle').click();
  await expect(timeline).toBeVisible();

  const afterFilters = await timeline.evaluate((el) => {
    const t = el as HTMLElement & {
      from: number;
      to: number;
      cursor: number | null;
      step: number | null;
    };
    return { from: t.from, to: t.to, cursor: t.cursor, step: t.step };
  });
  expect(afterFilters.from).toBeCloseTo(afterJump.from, 1);
  expect(afterFilters.to).toBeCloseTo(afterJump.to, 1);
  expect(afterFilters.cursor).toBe(640);
  expect(afterFilters.step).toBe(10);

  // 3. Simultaneous events at 540 ms (×3) show cluster badge and list ALL 3 siblings with non-causal wording.
  const cluster = timeline.locator('.sim-cluster');
  await expect(cluster).toHaveCount(1);
  await expect(cluster).toContainText('×3');
  await expect(cluster).toHaveAttribute('tabindex', '0');
  await expect(cluster).toHaveAttribute('role', 'button');
  await cluster.focus();
  await page.keyboard.press('Enter');
  const details = timeline.locator('#step-details');
  await expect(details.getByTestId('simultaneous-banner')).toContainText(
    '表示順は因果関係を示しません',
  );
  await expect(details.locator('[data-sim-step]')).toHaveCount(3);
  await expect(details).toContainText('手順 8');
  await expect(details).toContainText('手順 9');
  await expect(details).toContainText('手順 10');

  // Layout check in light & dark modes at desktop width
  expect(await findLayoutProblems(page)).toEqual([]);
  await page.emulateMedia({ colorScheme: 'dark' });
  expect(await findLayoutProblems(page)).toEqual([]);
});

test('logic analyzer: eventless timeline (steps: []) omits event lane cleanly without 0-step chrome', async ({
  page,
}) => {
  const eventlessJson = JSON.stringify(makeEventlessTimeline());
  await page.route('**/api/timeline/*', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: eventlessJson,
    });
  });

  const runId = await startFailingRun(page);
  await page.goto(`${base}#token=${server.token}`);
  await inSync(page);

  const card = page.getByTestId('scenario-card');
  await expect(card.getByTestId('scenario-timeline')).toHaveAttribute(
    'href',
    new RegExp(`%2Fapi%2Ftimeline%2F${runId}&`),
    { timeout: RUN_TIMEOUT },
  );

  const timeline = card.getByTestId('timeline-element');
  await expect(timeline).toBeVisible();
  await expect(card.getByTestId('timeline-failure-strip')).toHaveCount(0);
  await expect(timeline.locator('.lane.steps')).toHaveCount(0);
  await expect(timeline.locator('#step-details')).toBeHidden();
  await expect(timeline.locator('.var-name')).toHaveText([
    'Chassis.veh.speed',
    'Brake.hyd.pressure',
  ]);
  await expect(card).not.toContainText('0 個の手順');
});

test('logic analyzer: switching to a new Run clears previous event selection, cursor, and signal filter state', async ({
  page,
}) => {
  const fixtureJson = JSON.stringify(makeRepresentativeTimeline());
  await page.route('**/api/timeline/*', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: fixtureJson,
    });
  });

  const run1 = await startFailingRun(page);
  await page.goto(`${base}#token=${server.token}`);
  await inSync(page);

  const card = page.getByTestId('scenario-card');
  await expect(card.getByTestId('scenario-timeline')).toHaveAttribute(
    'href',
    new RegExp(`%2Fapi%2Ftimeline%2F${run1}&`),
    { timeout: RUN_TIMEOUT },
  );

  const timeline = card.getByTestId('timeline-element');
  await expect(card.getByTestId('timeline-failure-strip')).toBeVisible();

  // Dirty every piece of state on Run 1: select step #8, search "Brake", hide a signal, switch preset
  await card.getByTestId('timeline-signal-search').fill('Brake');
  await expect(card.getByTestId('timeline-visible-count')).toHaveText('3 / 6');
  await timeline.locator('button.lane-hide').first().click();
  await expect(card.getByTestId('timeline-hidden-strip')).toBeVisible();
  await card.getByTestId('timeline-preset-inputs').click();

  // Start Run 2 (new session & timeline URL)
  const run2 = await startFailingRun(page);
  expect(run2).not.toBe(run1);
  await expect(card.getByTestId('scenario-timeline')).toHaveAttribute(
    'href',
    new RegExp(`%2Fapi%2Ftimeline%2F${run2}&`),
    { timeout: RUN_TIMEOUT },
  );
  await expect(card.getByTestId('timeline-failure-strip')).toBeVisible();

  // Verify ALL Run 1 filter/hidden state was strictly wiped on Run switch
  await expect(card.getByTestId('timeline-signal-search')).toHaveValue('');
  await expect(card.getByTestId('timeline-hidden-strip')).toHaveCount(0);
  await expect(card.getByTestId('timeline-preset-all')).toHaveAttribute('aria-pressed', 'true');
  await expect(card.getByTestId('timeline-visible-count')).toHaveText('6 / 6');
});
