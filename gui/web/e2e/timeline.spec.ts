// The timeline page (gui/timeline/timeline.html with page.js and vehicle-sil-timeline.js) with the timeline references that the
// launcher's tests compare (tests/launch/timeline), and the page's fixtures (gui/timeline/tests/fixtures) for
// what the references do not have. The page is a plain file, not part of the app: the test serves it at an
// address of its own, under the Content-Security-Policy that the GUI server sends. The browser's language is
// English here (the page follows it when the address has no ?lang=); one test asks for Japanese with ?lang=ja.
import { Locator, Page, expect, test } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { REPOSITORY } from './support/gui-server';

const ORIGIN = 'http://timeline.test';
const PAGE = join(REPOSITORY, 'gui/timeline');
const REFERENCES = join(REPOSITORY, 'tests/launch/timeline');
const FIXTURES = join(PAGE, 'tests/fixtures');
/** The element that holds the data of a page that a tool made into one file. */
const EMBEDDED = '<script type="application/json" id="timeline-data"></script>';
/** The policy of the GUI server (gui/server/src/vehicle_sil_gui/server.py), which will serve the page too. */
const CSP =
  "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self'; object-src 'none'";

test.use({ locale: 'en-US' });

/** Serves the page at ORIGIN and the references at ORIGIN/data/; collects console errors. */
async function serve(page: Page): Promise<string[]> {
  const problems: string[] = [];
  page.on('console', (message) => {
    if (message.type() === 'error' || message.type() === 'warning') problems.push(message.text());
  });
  page.on('pageerror', (error) => problems.push(String(error)));
  await page.route(`${ORIGIN}/**`, async (route) => {
    const { pathname } = new URL(route.request().url());
    if (pathname === '/embedded.html') {
      // The page with a file in it, the way a tool that makes one HTML file puts it there.
      const data = readFileSync(join(REFERENCES, 'write_refused.timeline.json'), 'utf8');
      const json = JSON.parse(data);
      json.scenario.name = 'a name with </script><b>tags</b> & more';
      const text = JSON.stringify(json).replace(
        /[<>&]/g,
        (c) => `\\u${c.charCodeAt(0).toString(16).padStart(4, '0')}`,
      );
      const html = readFileSync(join(PAGE, 'timeline.html'), 'utf8').replace(
        EMBEDDED,
        EMBEDDED.replace('><', `>${text}<`),
      );
      await route.fulfill({
        body: html,
        contentType: 'text/html',
        headers: { 'Content-Security-Policy': CSP },
      });
      return;
    }
    const file = pathname.startsWith('/data/')
      ? join(REFERENCES, pathname.slice('/data/'.length))
      : join(PAGE, pathname.slice(1));
    let body: Buffer;
    try {
      body = readFileSync(file);
    } catch {
      await route.fulfill({ status: 404, body: 'not found' });
      return;
    }
    const contentType = file.endsWith('.html')
      ? 'text/html'
      : file.endsWith('.js')
        ? 'text/javascript'
        : 'application/json';
    await route.fulfill({ body, contentType, headers: { 'Content-Security-Policy': CSP } });
  });
  return problems;
}

/** The lane of a variable. */
function lane(page: Page, name: string): Locator {
  return page.locator('#chart .lane').filter({
    has: page.locator('.var-name', { hasText: new RegExp(`^${name.replace('.', '\\.')}$`) }),
  });
}

/** Moves the pointer over a lane's chart at virtual time t (from the range the page says it shows). */
async function hoverAt(page: Page, where: Locator, t: number): Promise<void> {
  const range = (await page.locator('#range').textContent()) ?? '';
  const [, from, to] = /^Showing ([\d.]+) ms to ([\d.]+) ms/.exec(range) ?? [];
  const box = await where.locator('svg.plot').boundingBox();
  if (!box || from === undefined) throw new Error(`no chart or range: ${range}`);
  const x = box.x + ((t - Number(from)) / (Number(to) - Number(from))) * box.width;
  await page.mouse.move(x, box.y + box.height / 2);
}

/** Whether the element is what a pointer at its centre reaches (nothing drawn later covers it there). */
async function onTopAtCentre(element: Locator): Promise<boolean> {
  await element.scrollIntoViewIfNeeded();
  return element.evaluate((e) => {
    const r = e.getBoundingClientRect();
    // In the element's shadow root: the document would answer with the element itself.
    const where = e.getRootNode() as Document | ShadowRoot;
    return where.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2) === e;
  });
}

test('timeline page: opens a failed run from ?src= on the failure, with its steps, lanes and reason', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html?src=data/fails_beside.timeline.json`);

  // The page's own h1; the element's title is an h2 under it
  await expect(page.getByRole('heading', { level: 1 })).toHaveText('Scenario timeline');
  await expect(page.getByRole('heading', { level: 2 })).toHaveText(
    'the expect beside a ramp fails at a sample',
  );
  const failure = page.getByTestId('failure');
  await expect(failure.getByRole('heading', { level: 3 })).toHaveText('Step 2 failed at 140 ms');
  // The launcher's sentence as it is, and the condition with its value
  await expect(page.getByTestId('failure-message')).toHaveText(
    'not true during the ramp (from 50 ms to 250 ms): at 140 ms Body.body.speed le 200, but it was 210 (its sample of 140 ms)',
  );
  await expect(page.getByTestId('failed-conditions')).toHaveText(
    'Body.body.speed le 200: it was 210 (sample of 140 ms)',
  );
  // A lane per variable; the lane of the failed condition is marked
  await expect(page.locator('#chart .lane:not(.axis):not(.steps)')).toHaveCount(2);
  await expect(page.locator('#chart .lane.failing .var-name')).toHaveText('Body.body.speed');
  // It opens on the failure (step 1 at 0 ms is out of view), and "Whole run" shows all
  await expect(page.locator('#range')).toHaveText(
    /^Showing [1-4]\d(\.\d+)? ms to 1[45]\d(\.\d+)? ms of 0 ms/,
  );
  await expect(page.locator('#chart .lane.steps .step-label')).toHaveText(['#2 ramp']);
  await page.getByRole('button', { name: 'Whole run' }).click();
  await expect(page.locator('#range')).toHaveText(/^Showing 0 ms to ([\d.]+) ms of 0 ms to \1 ms$/);
  await expect(page.locator('#chart .lane.steps .step-label')).toHaveText(['#1 set', '#2 ramp']);
  await expect(page.getByTestId('steps-table').locator('tbody tr')).toHaveCount(3);
  expect(problems).toEqual([]);
});

test('timeline page: a refused write says which writes are in the ECU and which are not', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html`);
  await page.locator('#file').setInputFiles(join(REFERENCES, 'write_refused.timeline.json'));
  await expect(page.getByTestId('failure').getByRole('heading', { level: 3 })).toHaveText(
    'Step 2 failed at 40 ms',
  );
  await expect(page.getByTestId('failure').getByRole('heading', { level: 4 })).toHaveText(
    'Writes for 40 ms',
  );
  // The launcher says the ECU rejected the write: the page does not say it never reached the ECU.
  await expect(page.getByTestId('failed-writes-explained')).toHaveText(
    'Applied: in the ECU, although the step failed. Not applied: the ECU refused it, or it came after the refused write in the same command, or it was not sent.',
  );
  const rows = page.getByTestId('failed-writes').locator('tbody tr');
  await expect(rows).toHaveText([
    /Body\.mode\s*auto \(2\)\s*✓ applied\s*$/,
    /Body\.level\s*13\s*✗ not applied\s*the cause of the failure$/,
  ]);
  expect(problems).toEqual([]);
});

test('timeline page: a bool and an enum write that the ECU did not apply stay visible', async ({
  page,
}) => {
  // As the launcher writes it: the refused write, the one another step sent after it, and the value the ECU
  // kept are all at 40 ms, the end of the record.
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html?src=tests/fixtures/state_not_applied.timeline.json`);
  await page.getByRole('button', { name: 'Whole run' }).click();
  for (const [name, title, later, kept] of [
    ['Body.mode', 'Sent, and the ECU did not apply it: auto (2) at 40 ms', 'on (1)', 'off (0)'],
    ['Body.lamp', 'Sent, and the ECU did not apply it: true at 40 ms', 'false', 'false'],
  ]) {
    const where = lane(page, name);
    const notApplied = where.locator('rect.state.sent.not-applied');
    await expect(notApplied).toHaveCount(1);
    await expect(notApplied.locator('title')).toHaveText(title);
    // The mark of the failed write is on it and says the same.
    const mark = where.locator('g.mark.write');
    await expect(mark.locator('title')).toHaveText(
      /: not applied \(the cause of the failure\)$|: not applied$/,
    );
    expect(await onTopAtCentre(mark.locator('rect')), `${name}: the mark is covered`).toBe(true);
    // The point sent after it, at the same time: beside them, not under them.
    const notKnown = where.locator('rect.state.sent.not-known');
    await expect(notKnown.locator('title')).toHaveText(
      `Sent; not known whether the ECU applied it: ${later} at 40 ms`,
    );
    expect(await onTopAtCentre(notKnown), `${name}: the later point is covered`).toBe(true);
    // The value the ECU kept (step 1 set it and passed) is drawn too, but under them, not on them.
    await expect(where.locator('rect.state.held title')).toHaveText(
      new RegExp(`: ${kept.replace(/[()]/g, '\\$&')} from 40 ms to 40 ms$`),
    );
    // Too narrow for words.
    await expect(where.locator('.state-label.not-applied, .state-label.not-known')).toHaveCount(0);
    await expect(where.locator('.legend')).toContainText('not applied');
    await expect(where.locator('.legend')).toContainText('kept in the ECU');
  }
  // The read-out at 40 ms has the last point of that time.
  await page.locator('#chart').focus();
  for (let i = 0; i < 4; i++) await page.keyboard.press('ArrowRight');
  await expect(lane(page, 'Body.mode').locator('.readout')).toHaveText(
    /^sent on \(1\) \(not known whether the ECU applied it\)/,
  );
  expect(problems).toEqual([]);
});

test('timeline page: a write whose result is not known is drawn and worded as not known', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html?src=tests/fixtures/applied_unknown.timeline.json`);
  await page.getByRole('button', { name: 'Whole run' }).click();
  await expect(page.getByTestId('failed-writes').locator('tbody tr')).toHaveText([
    /Body\.mode\s*auto \(2\)\s*\? not known\s*$/,
    /Body\.level\s*13\s*\? not known\s*$/,
  ]);
  await expect(page.getByTestId('failed-writes-explained')).toHaveText(
    'Not known: no result says whether the ECU applied it.',
  );
  const level = lane(page, 'Body.level');
  // Not the line of a value the ECU kept, and not the mark of a write it did not apply
  await expect(level.locator('path.series.held, path.series.not-applied')).toHaveCount(0);
  await expect(level.locator('path.series.not-known title')).toHaveText(
    'Sent; not known whether the ECU applied it',
  );
  // The point is at 40 ms, the end of the record: the arrow keys land on it exactly.
  await page.locator('#chart').focus();
  for (let i = 0; i < 4; i++) await page.keyboard.press('ArrowRight');
  await expect(level.locator('.readout')).toHaveText(
    /^sent 13 \(not known whether the ECU applied it\)/,
  );
  await expect(level.locator('.readout')).not.toContainText('not applied');
  expect(problems).toEqual([]);
});

test("timeline page: the read-out says where points were dropped at the file's limits", async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html?src=tests/fixtures/dropped_ramp.timeline.json`);
  const level = lane(page, 'Body.level');
  await hoverAt(page, level, 5000);
  await expect(level.locator('.readout')).toHaveText("sent: values dropped at the file's limits");
  expect(problems).toEqual([]);
});

test('timeline page: hover updates every signal without resizing the cursor time', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html?src=data/fails_beside.timeline.json`);
  await page.locator('vehicle-sil-timeline').evaluate((el) => el.setAttribute('compact', ''));
  await page.getByRole('button', { name: 'Whole run' }).click();
  const axis = page.locator('#chart .axis');
  await axis.scrollIntoViewIfNeeded();
  const time = axis.locator('#cursor-time');
  const height = (await axis.boundingBox())?.height;
  const level = lane(page, 'Body.level').locator('.hover-readout');
  const speed = lane(page, 'Body.body.speed').locator('.hover-readout');
  await expect(level).toBeHidden();

  await hoverAt(page, axis, 25);
  await expect(level).toBeVisible();
  await expect(level).toContainText('sent 14 · seen 14');
  await expect(speed).toBeVisible();
  await expect(speed).toContainText('seen 140');
  expect((await axis.boundingBox())?.height).toBe(height);

  await hoverAt(page, axis, 125);
  await expect(time).toHaveText(/^Cursor \d+(\.\d+)? ms$/);
  await expect(level).toContainText('sent 20 · seen 20');
  await expect(speed).toContainText('seen 200');
  expect((await axis.boundingBox())?.height).toBe(height);

  await page.mouse.move(0, 0);
  await expect(level).toBeHidden();
  await expect(speed).toBeHidden();
  expect(problems).toEqual([]);
});

test('timeline page: the step inspector explains expectations and the cursor spans the chart', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html?src=data/waits.timeline.json`);
  const details = page.getByRole('region', { name: 'Step details' });
  await expect(details).toContainText('Body.body.speed ge 100');
  await expect(details).toContainText('at least once between 0 ms and 200 ms');
  await expect(details).toContainText('decided 10 ms');

  const chart = page.locator('#chart');
  const axis = chart.locator('.axis');
  await axis.scrollIntoViewIfNeeded();
  await hoverAt(page, axis, 90);
  await expect(details).toContainText('Body.body.speed near 200 ± 0.5');
  await expect(details).toContainText('Body.pulse eq false');
  await expect(details).toContainText('at least once between 80 ms and 580 ms');
  await expect(details).toContainText('decided 80 ms');

  const cursor = chart.locator('.cursor-line');
  await expect(cursor).toBeVisible();
  const line = (await cursor.boundingBox())!;
  const plot = (await axis.locator('svg.plot').boundingBox())!;
  const group = (await chart.locator('.group').last().boundingBox())!;
  const board = (await chart.boundingBox())!;
  expect(line.x).toBeGreaterThan(plot.x);
  expect(line.y + line.height).toBeGreaterThan(group.y + group.height);
  expect(line.y + line.height).toBeCloseTo(board.y + board.height - 1);

  await hoverAt(page, axis, 125);
  await expect(details).toContainText('Must match throughout 110 ms to 140 ms');
  expect(problems).toEqual([]);
});

test('timeline page: a file is opened with the keyboard alone, and data in the page is read as text', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html`);
  await page.keyboard.press('Tab');
  await expect(page.getByRole('button', { name: 'Open a file' })).toBeFocused();
  const chooser = page.waitForEvent('filechooser');
  await page.keyboard.press('Enter');
  await (await chooser).setFiles(join(FIXTURES, 'dropped_ramp.timeline.json'));
  await expect(page.getByRole('heading', { level: 2 })).toHaveText(
    'a long ramp loses points at the limits',
  );
  await expect(page.locator('#facts')).toContainText('timeline file dropped_ramp.timeline.json');

  await page.goto(`${ORIGIN}/embedded.html`);
  await expect(page.getByRole('heading', { level: 2 })).toHaveText(
    'a name with </script><b>tags</b> & more',
  );
  expect(await page.locator('b').count()).toBe(0);
  expect(problems).toEqual([]);
});

test('timeline page: text from the file stays text, and files it cannot read are refused with a reason', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html`);
  const data = JSON.parse(readFileSync(join(REFERENCES, 'write_refused.timeline.json'), 'utf8'));
  const evil = '<img src=x onerror="window.__evil=1"></script><script>window.__evil=2</script>';
  data.scenario.name = evil;
  data.steps[1].message = evil;
  data.variables[0].unit = evil;
  await page.locator('#file').setInputFiles({
    name: 'evil.timeline.json',
    mimeType: 'application/json',
    buffer: Buffer.from(JSON.stringify(data)),
  });
  await expect(page.getByRole('heading', { level: 2 })).toHaveText(evil);
  await expect(page.getByTestId('failure-message')).toHaveText(evil);
  await expect(page.locator('#facts')).toContainText('timeline file evil.timeline.json');
  expect(
    await page.evaluate(() => (window as unknown as { __evil?: number }).__evil),
  ).toBeUndefined();
  expect(await page.locator('img, script:not([src]):not([type="application/json"])').count()).toBe(
    0,
  );

  await page.locator('#file').setInputFiles({
    name: 'next.timeline.json',
    mimeType: 'application/json',
    buffer: Buffer.from(JSON.stringify({ ...data, vehicle_sil_timeline: 2 })),
  });
  await expect(page.getByRole('alert')).toContainText(
    /"next\.timeline\.json" is timeline format 2 \(it was written by vehicle-sil [^)]+\), and this page reads format 1 only\./,
  );
  // Nothing of the file before is left to be taken for this one
  await expect(page.locator('#view')).toBeHidden();
  await expect(page.getByRole('heading', { level: 2 })).toHaveText('Scenario timeline');
  await expect(page.locator('#facts')).toBeEmpty();

  await page.goto(
    `${ORIGIN}/timeline.html?src=${encodeURIComponent('http://elsewhere.test/a.json')}`,
  );
  await expect(page.getByRole('alert')).toContainText(
    "?src= must name a file on this page's own server (http://timeline.test)",
  );
  expect(problems).toEqual([]);
});

test('timeline page: ?lang=ja shows it in Japanese, and a long read-out is shown in full', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(
    `${ORIGIN}/timeline.html?src=tests/fixtures/state_not_applied.timeline.json&lang=ja`,
  );
  await expect(page.locator('html')).toHaveAttribute('lang', 'ja');
  await expect(page.getByTestId('failure').getByRole('heading', { level: 3 })).toHaveText(
    /^手順 \d+ が [\d.]+ ms で失敗しました$/,
  );
  await expect(page.getByRole('button', { name: 'ファイルを開く' })).toBeVisible();
  await expect(page.getByTestId('failed-writes')).toContainText('入らなかった');
  await expect(page.locator('#chart')).toHaveAttribute('aria-label', /^シナリオの手順と変数/);
  await page.getByRole('button', { name: '実行の全体' }).click();
  await page.locator('#chart').focus();
  for (let i = 0; i < 4; i++) await page.keyboard.press('ArrowRight');
  // The longest read-out of the lane: it wraps instead of being cut.
  const readout = lane(page, 'Body.mode').locator('.readout');
  await expect(readout).toHaveText(/^送った値 on \(1\)（ECU に入ったか不明）/);
  expect(await readout.evaluate((e) => e.scrollWidth <= e.clientWidth)).toBe(true);
  expect(problems).toEqual([]);
});

test('timeline page: attributes a template failure to the template file, line and call stack under CSP', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html`);
  const data = JSON.parse(readFileSync(join(REFERENCES, 'fails_beside.timeline.json'), 'utf8'));
  data.scenario.case = '定格';
  data.steps[1].file = 'templates/sub.yaml';
  data.steps[1].line = 6;
  data.steps[1].name = 'Inner failure step';
  data.steps[1].call_stack = [
    {
      file: 'scenarios/main.yaml',
      line: 4,
      name: 'Outer call',
      params: { target: 200 },
    },
  ];
  data.lines[1].file = 'templates/sub.yaml';
  await page.locator('#file').setInputFiles({
    name: 'template_failure.timeline.json',
    mimeType: 'application/json',
    buffer: Buffer.from(JSON.stringify(data)),
  });

  await expect(page.locator('#facts')).toContainText('case 定格');
  const failure = page.getByTestId('failure');
  await expect(failure).toContainText('Inner failure step');
  await expect(failure).toContainText('line 6 of templates/sub.yaml');
  await expect(page.getByTestId('failure-call-stack')).toContainText(
    'line 4 of scenarios/main.yaml · Outer call · target = 200',
  );
  const stepsTable = page.getByTestId('steps-table');
  await expect(stepsTable).toContainText('templates/sub.yaml:6');
  await expect(stepsTable).toContainText('Inner failure step');
  expect(problems).toEqual([]);
});

test('timeline page: sanitizes XSS in case, non-string step.file, null lines, and call stack under CSP', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html`);
  const data = JSON.parse(readFileSync(join(REFERENCES, 'fails_beside.timeline.json'), 'utf8'));
  const casePayload = '<script>window.__xss="case"</script>';
  const stepPayload = '<script>window.__xss="step"</script>Step2';
  const frameFile = 'templates/<img src=x onerror="window.__xss=1">.yaml';
  const frameParam = '<svg onload="window.__xss=2">';
  data.scenario.case = casePayload;
  data.steps[1].file = 123;
  data.steps[1].line = null;
  data.steps[1].name = stepPayload;
  data.steps[1].call_stack = [
    {
      file: frameFile,
      line: null,
      name: 'Outer',
      params: { p: frameParam },
    },
  ];
  data.steps[2].file = 'templates/sub.yaml';
  data.steps[2].line = null;
  data.steps[2].status = 'skipped';
  data.steps[2].call_stack = null;

  await page.locator('#file').setInputFiles({
    name: 'xss_template.timeline.json',
    mimeType: 'application/json',
    buffer: Buffer.from(JSON.stringify(data)),
  });

  await expect(page.locator('#facts')).toContainText(casePayload);
  const failure = page.getByTestId('failure');
  await expect(failure).toContainText(stepPayload);
  await expect(failure).toContainText(data.scenario.file);
  const failureText = (await failure.textContent()) ?? '';
  expect(failureText).not.toContain('line null');
  expect(failureText).not.toContain('of 123');
  expect(failureText).not.toContain('line  of');

  const callStack = page.getByTestId('failure-call-stack');
  await expect(callStack).toContainText(frameFile);
  await expect(callStack).toContainText(`p = ${frameParam}`);

  const stepsText = (await page.getByTestId('steps-table').textContent()) ?? '';
  expect(stepsText).toContain('templates/sub.yaml');
  expect(stepsText).not.toContain('templates/sub.yaml:');
  expect(stepsText).not.toContain(':null');

  expect(
    await page.evaluate(() => (window as unknown as { __xss?: unknown }).__xss),
  ).toBeUndefined();
  const shadowDangerous = await page.locator('vehicle-sil-timeline').evaluate((el) => {
    const root = el.shadowRoot;
    if (!root) return -1;
    return root.querySelectorAll('script, img, iframe, [onerror], [onload]').length;
  });
  expect(shadowDangerous).toBe(0);
  expect(problems).toEqual([]);
});
