// The timeline page (gui/timeline/timeline.html and viewer.js) with the timeline references that the
// launcher's tests compare (tests/launch/timeline), and the page's fixtures (gui/timeline/tests/fixtures) for
// what the references do not have. The page is a plain file, not part of the app: the test serves it at an
// address of its own, under the Content-Security-Policy that the GUI server sends.
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
    return document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2) === e;
  });
}

test('timeline page: opens a failed run from ?src= on the failure, with its steps, lanes and reason', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html?src=data/fails_beside.timeline.json`);

  await expect(page.getByRole('heading', { level: 1 })).toHaveText(
    'the expect beside a ramp fails at a sample',
  );
  const failure = page.getByTestId('failure');
  await expect(failure.getByRole('heading', { level: 2 })).toHaveText('Step 2 failed at 140 ms');
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
  await expect(page.getByTestId('failure').getByRole('heading', { level: 2 })).toHaveText(
    'Step 2 failed at 40 ms',
  );
  await expect(page.getByTestId('failure').getByRole('heading', { level: 3 })).toHaveText(
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
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/timeline.html?src=tests/fixtures/state_not_applied.timeline.json`);
  await page.getByRole('button', { name: 'Whole run' }).click();
  for (const [name, title, kept] of [
    ['Body.mode', 'Sent, and the ECU did not apply it: auto (2) at 40 ms', 'off (0)'],
    ['Body.lamp', 'Sent, and the ECU did not apply it: true at 40 ms', 'false'],
  ]) {
    const where = lane(page, name);
    const notApplied = where.locator('rect.state.sent.not-applied');
    await expect(notApplied).toHaveCount(1);
    await expect(notApplied.locator('title')).toHaveText(title);
    expect(await onTopAtCentre(notApplied), `${name}: covered at its centre`).toBe(true);
    // The value the ECU kept is drawn too, but under it, not on it.
    await expect(where.locator('rect.state.held title')).toHaveText(
      new RegExp(`: ${kept.replace(/[()]/g, '\\$&')} from 40 ms to 60 ms$`),
    );
    // The point sent after it: not known whether the ECU applied it.
    await expect(where.locator('rect.state.sent.not-known title')).toHaveText(
      /^Sent; not known whether the ECU applied it: .* at 60 ms$/,
    );
    await expect(where.locator('.legend')).toContainText('not applied');
    await expect(where.locator('.legend')).toContainText('kept in the ECU');
  }
  const mode = lane(page, 'Body.mode');
  await hoverAt(page, mode, 50);
  await expect(mode.locator('.readout')).toHaveText(
    /^sent auto \(2\) \(not applied; the ECU has off \(0\)\)/,
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
  await hoverAt(page, level, 50);
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
  await expect(page.getByRole('heading', { level: 1 })).toHaveText(
    'a long ramp loses points at the limits',
  );
  await expect(page.locator('#facts')).toContainText('timeline file dropped_ramp.timeline.json');

  await page.goto(`${ORIGIN}/embedded.html`);
  await expect(page.getByRole('heading', { level: 1 })).toHaveText(
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
  await expect(page.getByRole('heading', { level: 1 })).toHaveText(evil);
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
  await expect(page.getByRole('heading', { level: 1 })).toHaveText('Scenario timeline');
  await expect(page.locator('#facts')).toBeEmpty();

  await page.goto(
    `${ORIGIN}/timeline.html?src=${encodeURIComponent('http://elsewhere.test/a.json')}`,
  );
  await expect(page.getByRole('alert')).toContainText(
    "?src= must name a file on this page's own server (http://timeline.test)",
  );
  expect(problems).toEqual([]);
});
