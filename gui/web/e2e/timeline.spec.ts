// The timeline page (gui/timeline/timeline.html and viewer.js) with the timeline references that the
// launcher's tests compare (tests/launch/timeline). The page is a plain file, not part of the app: the test
// serves it at an address of its own, under the Content-Security-Policy that the GUI server sends.
import { Page, expect, test } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { REPOSITORY } from './support/gui-server';

const ORIGIN = 'http://timeline.test';
const PAGE = join(REPOSITORY, 'gui/timeline');
const REFERENCES = join(REPOSITORY, 'tests/launch/timeline');
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
  const rows = page.getByTestId('failed-writes').locator('tbody tr');
  await expect(rows).toHaveText([
    /Body\.mode\s*auto \(2\)\s*✓ applied\s*$/,
    /Body\.level\s*13\s*✗ not applied\s*refused: the cause of the failure$/,
  ]);
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

  await page.goto(
    `${ORIGIN}/timeline.html?src=${encodeURIComponent('http://elsewhere.test/a.json')}`,
  );
  await expect(page.getByRole('alert')).toContainText(
    "?src= must name a file on this page's own server (http://timeline.test)",
  );
  expect(problems).toEqual([]);
});
