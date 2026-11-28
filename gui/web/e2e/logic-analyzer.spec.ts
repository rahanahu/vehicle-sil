// End-to-end regressions for the Standard GUI Logic Analyzer (Option A + P0/P1/P2 refinements):
// 1. Zoomed in -> jump to failure -> exact failure virtual timestamp & pre-failure waveforms/events ->
//    signal search/presets/inline hide/restore & collapse/reopen preserve zoom, cursor, and selected event.
// 2. Simultaneous events at the same virtual timestamp list all sibling steps with explicit non-causal wording.
// 3. Eventless timeline (steps: []) omits the event lane cleanly without misleading "0 steps" chrome.
// 4. Run switch clears previous event selection, cursor, search query, presets, and hidden-signal state.
import { Page, expect, test } from '@playwright/test';
import { CONFIGS, GuiServer, REPLAY_LAUNCHER, launchGuiServer } from './support/gui-server';
import { findLayoutProblems } from './support/layout';
import { RUN_TIMEOUT, inSync, start } from './support/run-page';

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

  // End-edge bool transition at recorded_to_ms (640 ms) draws ONE crisp → true label (no colliding duplicate).
  const edgeLabels = timeline.locator('.state-edge-label');
  await expect(edgeLabels).toHaveCount(1);
  await expect(edgeLabels).toContainText('→ true');

  // Built-in #toolbar is hidden so there are zero duplicate toolbar/Participants controls.
  await expect(timeline.locator('#toolbar')).toBeHidden();

  // 1. Brush-drag zoom from whole-run into ~[150, 300] ms, Shift+drag pan, then click "失敗時刻へ移動" -> preserves exact zoomed span!
  await card.getByTestId('timeline-whole-run').click();
  await expect(card.getByTestId('timeline-range')).toContainText('0–977.6 ms');

  const plotBox = (await timeline.locator('.lane-plot').nth(1).boundingBox())!;
  const midY = plotBox.y + plotBox.height * 0.5;
  await page.mouse.move(plotBox.x + plotBox.width * 0.15, midY);
  await page.mouse.down();
  await page.mouse.move(plotBox.x + plotBox.width * 0.3, midY, { steps: 5 });
  await page.mouse.up();

  const afterZoom = await timeline.evaluate((el) => {
    const t = el as HTMLElement & { from: number; to: number };
    return { from: t.from, to: t.to };
  });
  const zoomedSpan = afterZoom.to - afterZoom.from;
  expect(zoomedSpan).toBeGreaterThan(100);
  expect(zoomedSpan).toBeLessThan(200);
  expect(afterZoom.to).toBeLessThan(400);
  await expect(card.getByTestId('timeline-range')).not.toContainText('0–977.6 ms');

  // Continuous Shift+drag panning shifts the viewport smoothly by > 50 px without breaking mid-drag!
  const startX = plotBox.x + plotBox.width * 0.6;
  await page.keyboard.down('Shift');
  await page.mouse.move(startX, midY);
  await page.mouse.down();
  await page.mouse.move(startX - 120, midY, { steps: 6 });
  await page.mouse.up();
  await page.keyboard.up('Shift');
  const afterPan = await timeline.evaluate((el) => {
    const t = el as HTMLElement & { from: number; to: number };
    return { from: t.from, to: t.to };
  });
  expect(afterPan.from).toBeGreaterThan(afterZoom.from + 15);
  expect(afterPan.to - afterPan.from).toBeCloseTo(zoomedSpan, 1);

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
  expect(afterJump.to - afterJump.from).toBeCloseTo(zoomedSpan, 1);
  expect(afterJump.from).toBeCloseTo(640 - zoomedSpan * 0.8, 1);
  expect(afterJump.to).toBeCloseTo(640 + zoomedSpan * 0.2, 1);
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

  // 3. Simultaneous events at 540 ms (×3): clicking/activating pins cursor at 540.0 ms (NOT 640 ms!) and lists all 3 siblings.
  const cluster = timeline.locator('.sim-cluster');
  await expect(cluster).toHaveCount(1);
  await expect(cluster).toContainText('×3');
  await expect(cluster).toHaveAttribute('tabindex', '0');
  await expect(cluster).toHaveAttribute('role', 'button');
  await cluster.focus();
  await page.keyboard.press('Enter');
  await expect(card.getByTestId('timeline-readout')).toContainText('カーソル: 540.0 ms');
  const details = timeline.locator('#step-details');
  await expect(details.getByTestId('simultaneous-banner')).toContainText(
    '表示順は因果関係を示しません',
  );
  await expect(details.locator('[data-sim-step]')).toHaveCount(3);
  await expect(details).toContainText('手順 8');
  await expect(details).toContainText('手順 9');
  await expect(details).toContainText('手順 10');

  // Whole-run view + clicking an already-visible event bar preserves whole-run viewport!
  await card.getByTestId('timeline-whole-run').click();
  const wholeBefore = await timeline.evaluate((el) => {
    const t = el as HTMLElement & { from: number; to: number };
    return { from: t.from, to: t.to };
  });
  await timeline.locator('g.step[data-step="2"]').click();
  const wholeAfter = await timeline.evaluate((el) => {
    const t = el as HTMLElement & { from: number; to: number; step: number | null };
    return { from: t.from, to: t.to, step: t.step };
  });
  expect(wholeAfter.step).toBe(2);
  expect(wholeAfter.from).toBeCloseTo(wholeBefore.from, 1);
  expect(wholeAfter.to).toBeCloseTo(wholeBefore.to, 1);

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

test('logic analyzer: switching to a new Run clears cursor, step, viewport, and filters, and ignores in-flight clicks', async ({
  page,
}) => {
  const warnings: string[] = [];
  page.on('console', (m) => {
    if (m.type() === 'warning' || m.type() === 'error') warnings.push(m.text());
  });

  const run1Json = JSON.stringify(makeRepresentativeTimeline());
  const run2Data = makeEventlessTimeline();
  run2Data.scenario.recorded_to_ms = 3000;
  run2Data.scenario.over_at_ms = 3010;
  run2Data.scenario.end_ms = 3000;
  run2Data.series[0]!.last_ms = 3000;
  run2Data.series[0]!.points = [
    [0, 50],
    [1500, 90],
    [3000, 100],
  ];
  run2Data.series[1]!.last_ms = 3000;
  const run2Json = JSON.stringify(run2Data);

  let run1Id = '';
  let releaseRun2: (() => void) | null = null;
  let markRun2Requested: (() => void) | null = null;
  const run2Requested = new Promise<void>((resolve) => {
    markRun2Requested = resolve;
  });
  const run2Gate = new Promise<void>((resolve) => {
    releaseRun2 = resolve;
  });

  await page.route('**/api/timeline/*', async (route) => {
    const url = route.request().url();
    if (run1Id && url.endsWith(`/api/timeline/${run1Id}`)) {
      await route.fulfill({ status: 200, contentType: 'application/json', body: run1Json });
      return;
    }
    markRun2Requested?.();
    await run2Gate;
    await route.fulfill({ status: 200, contentType: 'application/json', body: run2Json });
  });

  run1Id = await startFailingRun(page);
  await page.goto(`${base}#token=${server.token}`);
  await inSync(page);

  const card = page.getByTestId('scenario-card');
  await expect(card.getByTestId('scenario-timeline')).toHaveAttribute(
    'href',
    new RegExp(`%2Fapi%2Ftimeline%2F${run1Id}&`),
    { timeout: RUN_TIMEOUT },
  );

  const timeline = card.getByTestId('timeline-element');
  await expect(card.getByTestId('timeline-failure-strip')).toBeVisible();

  // Dirty EVERY piece of state on Run 1: select step #8, pin cursor, click -150 ms, search "Brake", hide a signal, switch preset
  await card.getByTestId('timeline-window-150').click();
  await timeline.locator('g.step[data-step="8"]').click();
  await expect(card.getByTestId('timeline-readout')).toContainText('選んだ手順: 8');
  await expect(card.getByTestId('timeline-readout')).toContainText('カーソル: 540.0 ms');
  await card.getByTestId('timeline-signal-search').fill('Brake');
  await expect(card.getByTestId('timeline-visible-count')).toHaveText('3 / 6');
  await timeline.locator('button.lane-hide').first().click();
  await expect(card.getByTestId('timeline-hidden-strip')).toBeVisible();
  await card.getByTestId('timeline-preset-inputs').click();

  // Start Run 2 (distinct 0..3000 ms eventless timeline) and verify in-flight click cannot leak hiddenVariables
  const run2Id = await startFailingRun(page);
  expect(run2Id).not.toBe(run1Id);
  await run2Requested;
  // While Run 2 fetch is in-flight, dispatching a stale variablehide from Run 1 must be ignored.
  await timeline.evaluate((el) => {
    el.dispatchEvent(
      new CustomEvent('variablehide', {
        detail: { variable: 'Chassis.veh.speed' },
        bubbles: true,
        cancelable: true,
      }),
    );
  });
  releaseRun2!();

  await expect(card.getByTestId('scenario-timeline')).toHaveAttribute(
    'href',
    new RegExp(`%2Fapi%2Ftimeline%2F${run2Id}&`),
    { timeout: RUN_TIMEOUT },
  );
  await expect(timeline.locator('.var-name')).toHaveText([
    'Chassis.veh.speed',
    'Brake.hyd.pressure',
  ]);

  // Verify ALL Run 1 state was strictly wiped on Run switch: cursor, step, viewport, search, preset, hidden chips
  await expect(card.getByTestId('timeline-signal-search')).toHaveValue('');
  await expect(card.getByTestId('timeline-hidden-strip')).toHaveCount(0);
  await expect(card.getByTestId('timeline-preset-all')).toHaveAttribute('aria-pressed', 'true');
  await expect(card.getByTestId('timeline-visible-count')).toHaveText('2 / 2');
  await expect(card.getByTestId('timeline-readout')).not.toContainText('選んだ手順: 8');
  await expect(card.getByTestId('timeline-readout')).not.toContainText('540.0 ms');

  // Viewport shows Run 2's full 0..3000+ ms range (NOT Run 1's -150 ms window!)
  const run2View = await timeline.evaluate((el) => {
    const t = el as HTMLElement & { from: number; to: number };
    return { from: t.from, to: t.to };
  });
  expect(run2View.from).toBe(0);
  expect(run2View.to).toBeGreaterThanOrEqual(3000);
  expect(warnings).toEqual([]);
});

test('logic analyzer: pre-failure -300 ms preset on a short run never shrinks narrower than -150 ms or warns', async ({
  page,
}) => {
  const warnings: string[] = [];
  page.on('console', (m) => {
    if (m.type() === 'warning' || m.type() === 'error') warnings.push(m.text());
  });

  const shortTl = makeRepresentativeTimeline();
  shortTl.scenario.recorded_to_ms = 250;
  shortTl.scenario.over_at_ms = 260;
  shortTl.steps = [
    {
      index: 1,
      line: 5,
      kind: 'set',
      name: 'init',
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
      ...shortTl.steps[4]!,
      index: 2,
      start_ms: 200,
      at_ms: 250,
      end_ms: 300,
      watch: {
        ...shortTl.steps[4]!.watch!,
        from_ms: 200,
        to_ms: 300,
      },
      failed: {
        ...shortTl.steps[4]!.failed!,
        at_ms: 250,
      },
    },
  ];
  shortTl.series.forEach((s) => {
    s.last_ms = 250;
    s.points = s.points.filter((p) => (p[0] as number) <= 250);
  });

  await page.route('**/api/timeline/*', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(shortTl),
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
  await expect(card.getByTestId('timeline-failure-strip')).toBeVisible();

  await card.getByTestId('timeline-window-150').click();
  const v150 = await timeline.evaluate((el) => {
    const t = el as HTMLElement & { from: number; to: number };
    return { from: t.from, to: t.to };
  });
  await card.getByTestId('timeline-window-300').click();
  const v300 = await timeline.evaluate((el) => {
    const t = el as HTMLElement & { from: number; to: number };
    return { from: t.from, to: t.to };
  });
  expect(v300.to - v300.from).toBeGreaterThanOrEqual(v150.to - v150.from);
  expect(v300.from).toBe(0);
  expect(v300.to).toBeGreaterThanOrEqual(300);
  expect(warnings).toEqual([]);
});

test('logic analyzer: selecting an extra recorded signal in Run setup records and displays it for search, hide, and restore', async ({
  page,
}) => {
  await page.goto(`${base}#token=${server.token}`);
  await inSync(page);

  // Select simulation.yaml and a passing scenario (throttle.yaml)
  const cfgSelect = page.getByTestId('config-select');
  await cfgSelect.click();
  await page.getByRole('option', { name: 'simulation.yaml', exact: true }).click();
  await expect(page.getByTestId('check-outcome')).toBeVisible({ timeout: RUN_TIMEOUT });

  await page.getByTestId('scenario-select').click();
  await page.getByRole('option', { name: 'throttle.yaml', exact: true }).click();
  await expect(page.getByTestId('check-scenario')).toContainText('throttle.yaml', {
    timeout: RUN_TIMEOUT,
  });

  // Open Recorded signals disclosure and select Dashboard.dashboard.speed (not used by scenario)
  const disclosure = page.getByTestId('record-signals');
  await expect(disclosure).toBeVisible();
  await disclosure.locator('summary').click();
  const extraCheck = page.waitForResponse(
    (r) =>
      r.url().endsWith('/api/check') &&
      Array.isArray((r.request().postDataJSON() as Record<string, unknown>)['record']) &&
      ((r.request().postDataJSON() as Record<string, unknown>)['record'] as string[]).includes(
        'Dashboard.dashboard.speed',
      ),
  );
  await page
    .getByTestId('record-signal-option-Dashboard.dashboard.speed')
    .getByRole('checkbox')
    .check();
  await extraCheck;
  await expect(page.getByTestId('record-signals-count')).toContainText('1');

  const runReqPromise = page.waitForRequest(
    (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
  );
  const runId = await start(page);
  const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
  expect(runBody['record']).toEqual(['Dashboard.dashboard.speed']);

  const card = page.getByTestId('scenario-card');
  await expect(card.getByTestId('scenario-timeline')).toHaveAttribute(
    'href',
    new RegExp(`%2Fapi%2Ftimeline%2F${runId}&`),
    { timeout: RUN_TIMEOUT },
  );
  await card.getByTestId('timeline-toggle').click();
  const timeline = card.getByTestId('timeline-element');
  await expect(timeline).toBeVisible();
  await expect(card.getByTestId('timeline-visible-count')).toContainText('3 / 3', {
    timeout: RUN_TIMEOUT,
  });

  // Search for the extra recorded signal, hide it via inline ×, and restore it via chip
  await card.getByTestId('timeline-signal-search').fill('dashboard.speed');
  await expect(card.getByTestId('timeline-visible-count')).toContainText('1 / 3');
  await expect(timeline.locator('button.lane-hide')).toHaveCount(1);
  await timeline.locator('button.lane-hide').first().click();
  await expect(card.getByTestId('timeline-visible-count')).toContainText('0 / 3');
  const restoreChip = card.locator(
    '[data-testid="timeline-restore-var"][data-variable="Dashboard.dashboard.speed"]',
  );
  await expect(restoreChip).toBeVisible();
  await restoreChip.click();
  await expect(card.getByTestId('timeline-visible-count')).toContainText('1 / 3');
});

test('logic analyzer: post-failure tail recording input sends record_after_fail_ms and shows Around failure framing', async ({
  page,
}) => {
  await page.goto(`${base}#token=${server.token}`);
  await inSync(page);

  const cfgSelect = page.getByTestId('config-select');
  await cfgSelect.click();
  await page.getByRole('option', { name: 'simulation.yaml', exact: true }).click();
  await expect(page.getByTestId('check-outcome')).toBeVisible({ timeout: RUN_TIMEOUT });

  await page.getByTestId('scenario-select').click();
  await page.getByRole('option', { name: 'fails.yaml', exact: true }).click();
  await expect(page.getByTestId('check-scenario')).toContainText('fails.yaml', {
    timeout: RUN_TIMEOUT,
  });

  // Set post-failure tail recording to 300 ms and also pick Dashboard.dashboard.speed
  const tailInput = page.getByTestId('record-after-fail-ms');
  await expect(tailInput).toBeVisible();
  const tailCheck = page.waitForResponse(
    (r) =>
      r.url().endsWith('/api/check') &&
      (r.request().postDataJSON() as Record<string, unknown>)['record_after_fail_ms'] === 300,
  );
  await tailInput.fill('300');
  await tailCheck;

  const disclosure = page.getByTestId('record-signals');
  await disclosure.locator('summary').click();
  const dashSpeedBox = page
    .getByTestId('record-signal-option-Dashboard.dashboard.speed')
    .getByRole('checkbox');
  if (!(await dashSpeedBox.isChecked())) {
    const recCheck = page.waitForResponse(
      (r) =>
        r.url().endsWith('/api/check') &&
        Array.isArray((r.request().postDataJSON() as Record<string, unknown>)['record']) &&
        ((r.request().postDataJSON() as Record<string, unknown>)['record'] as string[]).includes(
          'Dashboard.dashboard.speed',
        ),
    );
    await dashSpeedBox.check();
    await recCheck;
  }

  const runReqPromise = page.waitForRequest(
    (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
  );
  const runId = await start(page);
  const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
  expect(runBody['record_after_fail_ms']).toBe(300);
  expect(runBody['record']).toEqual(['Dashboard.dashboard.speed']);

  const card = page.getByTestId('scenario-card');
  await expect(card.getByTestId('scenario-timeline')).toHaveAttribute(
    'href',
    new RegExp(`%2Fapi%2Ftimeline%2F${runId}&`),
    { timeout: RUN_TIMEOUT },
  );
  const timeline = card.getByTestId('timeline-element');
  await expect(timeline).toBeVisible();

  // Around failure button appears because recorded_to_ms (400) > failed.at_ms (100)
  const aroundBtn = card.getByTestId('timeline-window-around-failure');
  await expect(aroundBtn).toBeVisible();
  await card.getByTestId('timeline-window-150').click();
  const preView = await timeline.evaluate((el) => {
    const t = el as HTMLElement & { from: number; to: number };
    return { from: t.from, to: t.to };
  });
  expect(preView.to).toBeLessThan(200);

  await aroundBtn.click();
  const aroundView = await timeline.evaluate((el) => {
    const t = el as HTMLElement & { from: number; to: number; cursor: number | null };
    return { from: t.from, to: t.to, cursor: t.cursor };
  });
  expect(aroundView.from).toBe(0);
  expect(aroundView.to).toBeGreaterThanOrEqual(400);
  expect(aroundView.cursor).toBe(100);
});

test('logic analyzer: manual run with Recorded signals auto-opens clean manual timeline card without scenario steps or warnings', async ({
  page,
}) => {
  await page.goto(`${base}#token=${server.token}`);
  await inSync(page);

  const cfgSelect = page.getByTestId('config-select');
  await cfgSelect.click();
  await page.getByRole('option', { name: 'simulation.yaml', exact: true }).click();
  await expect(page.getByTestId('check-outcome')).toBeVisible({ timeout: RUN_TIMEOUT });

  // Ensure manual run (Scenario = None): Recorded signals is visible and Record after failure is hidden
  await page.getByTestId('scenario-select').click();
  await page.getByRole('option', { name: 'なし（自分で操作する）', exact: true }).click();
  await expect(page.getByTestId('record-after-fail-ms')).toHaveCount(0);
  const disclosure = page.getByTestId('record-signals');
  await expect(disclosure).toBeVisible();
  await disclosure.locator('summary').click();

  const pickSignal = async (signalName: string) => {
    const box = page.getByTestId(`record-signal-option-${signalName}`).getByRole('checkbox');
    if (!(await box.isChecked())) {
      const checkResp = page.waitForResponse(
        (r) =>
          r.url().endsWith('/api/check') &&
          Array.isArray((r.request().postDataJSON() as Record<string, unknown>)['record']) &&
          ((r.request().postDataJSON() as Record<string, unknown>)['record'] as string[]).includes(
            signalName,
          ),
      );
      await box.check();
      await checkResp;
    }
  };
  await pickSignal('Engine.engine.speed');
  await pickSignal('Dashboard.dashboard.speed');
  await expect(page.getByTestId('record-signals-count')).toContainText('2');

  // Set stop_at_ms = 1000 so the manual run finishes cleanly without needing Stop
  const stopCheck = page.waitForResponse(
    (r) =>
      r.url().endsWith('/api/check') &&
      (r.request().postDataJSON() as Record<string, unknown>)['stop_at_ms'] === 1000,
  );
  await page.getByTestId('stop-at-ms').fill('1000');
  await stopCheck;

  const runReqPromise = page.waitForRequest(
    (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
  );
  const runId = await start(page);
  const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
  expect(runBody['scenario']).toBeUndefined();
  expect(runBody['record']).toEqual(['Dashboard.dashboard.speed', 'Engine.engine.speed']);

  // Manual timeline card opens automatically with no scenario card and no failure/warning chrome
  const card = page.getByTestId('manual-timeline-card');
  await expect(card).toBeVisible({ timeout: RUN_TIMEOUT });
  await expect(page.getByTestId('scenario-card')).toHaveCount(0);
  await expect(card.getByTestId('manual-timeline-link')).toHaveAttribute(
    'href',
    new RegExp(`%2Fapi%2Ftimeline%2F${runId}&`),
  );
  await expect(card.getByTestId('timeline-failure-strip')).toHaveCount(0);
  const timeline = card.getByTestId('timeline-element');
  await expect(timeline).toBeVisible();
  await expect(card.getByTestId('timeline-visible-count')).toContainText('2 / 2', {
    timeout: RUN_TIMEOUT,
  });

  // Search, hide (×), and restore work cleanly on the manual timeline
  await card.getByTestId('timeline-signal-search').fill('dashboard.speed');
  await expect(card.getByTestId('timeline-visible-count')).toContainText('1 / 2');
  await timeline.locator('button.lane-hide').first().click();
  await expect(card.getByTestId('timeline-visible-count')).toContainText('0 / 2');
  const restoreChip = card.locator(
    '[data-testid="timeline-restore-var"][data-variable="Dashboard.dashboard.speed"]',
  );
  await expect(restoreChip).toBeVisible();
  await restoreChip.click();
  await expect(card.getByTestId('timeline-visible-count')).toContainText('1 / 2');
});

test('logic analyzer: Record virtual-time ECU events toggle renders severity ticks, synchronized ECU Events table, filtering, and click-to-pin cursor correlation', async ({
  page,
}) => {
  await page.goto(`${base}#token=${server.token}`);
  await inSync(page);

  const cfgSelect = page.getByTestId('config-select');
  await cfgSelect.click();
  await page.getByRole('option', { name: 'simulation.yaml', exact: true }).click();
  await expect(page.getByTestId('check-outcome')).toBeVisible({ timeout: RUN_TIMEOUT });

  // Select failing scenario and opt in to Record virtual-time ECU events
  await page.getByTestId('scenario-select').click();
  await page.getByRole('option', { name: 'fails.yaml', exact: true }).click();
  await expect(page.getByTestId('check-outcome')).toBeVisible({ timeout: RUN_TIMEOUT });

  const recordEventsCb = page.getByTestId('record-events').getByRole('checkbox');
  await expect(recordEventsCb).toBeVisible();
  await expect(recordEventsCb).not.toBeChecked();
  const checkPromise = page.waitForResponse(
    (r) =>
      r.url().endsWith('/api/check') &&
      (r.request().postDataJSON() as Record<string, unknown>)['record_events'] === true,
  );
  await recordEventsCb.check();
  await checkPromise;

  const runReqPromise = page.waitForRequest(
    (r) => r.url().endsWith('/api/run') && r.method() === 'POST',
  );
  await start(page);
  const runBody = (await runReqPromise).postDataJSON() as Record<string, unknown>;
  expect(runBody['record_events']).toBe(true);

  const card = page.getByTestId('scenario-card');
  await expect(card).toBeVisible({ timeout: RUN_TIMEOUT });
  const timeline = card.getByTestId('timeline-element');
  await expect(timeline).toBeVisible({ timeout: RUN_TIMEOUT });

  // Embedded timeline shows severity ticks and synchronized ECU Events table even though hide="tables"
  const rows = timeline.locator('[data-testid="ecu-event-row"]');
  await expect(rows).toHaveCount(4, { timeout: RUN_TIMEOUT });
  await expect(timeline.locator('[data-testid="ecu-event-tick"]')).toHaveCount(1);

  // "Visible range only" narrows rows to the zoomed failure window, and whole-run reveals all 3 grouped ticks
  const visibleOnly = timeline.locator('[data-testid="ecu-events-visible-only"]');
  await visibleOnly.check();
  await expect(rows).toHaveCount(1);
  await card.getByTestId('timeline-whole-run').click();
  await expect(timeline.locator('[data-testid="ecu-event-tick"]')).toHaveCount(3);
  await expect(rows).toHaveCount(4);
  await visibleOnly.uncheck();

  // Clicking the 50 ms event row pins the cursor at 50.0 ms and updates Δt column
  await rows.nth(2).click();
  await expect(card.getByTestId('timeline-readout')).toContainText('50.0 ms');
  await expect(rows.nth(2)).toHaveAttribute('aria-selected', 'true');
  await expect(rows.nth(2).locator('.ev-delta')).toHaveText('0 ms');
  await expect(rows.nth(3).locator('.ev-delta')).toHaveText('+50 ms');

  // Participant and text filters narrow the ECU Events table cleanly
  await timeline.locator('[data-testid="ecu-events-participant"]').selectOption('Dashboard');
  await expect(rows).toHaveCount(1);
  await timeline.locator('[data-testid="ecu-events-participant"]').selectOption('');
  await expect(rows).toHaveCount(4);
  await timeline.locator('[data-testid="ecu-events-search"]').fill('speed below target');
  await expect(rows).toHaveCount(1);
  await timeline.locator('[data-testid="ecu-events-search"]').fill('');
  await expect(rows).toHaveCount(4);
});
