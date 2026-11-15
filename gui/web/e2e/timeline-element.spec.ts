// The element <vehicle-sil-timeline> (gui/timeline/vehicle-sil-timeline.js) on pages of a user's own: the plain
// example gui/timeline/example.html, two elements on one page, each property, attribute and event, and that the
// page's CSS and the element's do not meet. The pages are served at an address of their own under a stricter
// policy than the GUI server's (no inline style at all), so the element must bring its CSS without a <style>.
import { Locator, Page, expect, test } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { REPOSITORY } from './support/gui-server';

const ORIGIN = 'http://element.test';
const TIMELINE = join(REPOSITORY, 'gui/timeline');
const REFERENCES = join(REPOSITORY, 'tests/launch/timeline');
const CSP =
  "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; object-src 'none'";

test.use({ locale: 'en-US' });

/**
 * Serves the element at ORIGIN, the references at ORIGIN/data/, gui/timeline/example.html at ORIGIN/example.html
 * (with fails_beside as its run.timeline.json), and pages of the test: html (at ORIGIN/test.html) with its
 * script (ORIGIN/test.js) and its CSS (ORIGIN/test.css). Collects console errors.
 */
async function serve(
  page: Page,
  html = '',
  script = '',
  css = '',
  answer?: (
    path: string,
    headers: Record<string, string>,
  ) => { status: number; body: string } | null,
): Promise<string[]> {
  const problems: string[] = [];
  page.on('console', (m) => {
    if (m.type() === 'error' || m.type() === 'warning') problems.push(m.text());
  });
  page.on('pageerror', (e) => problems.push(String(e)));
  await page.route(`${ORIGIN}/**`, async (route) => {
    const { pathname } = new URL(route.request().url());
    const own = answer?.(pathname, route.request().headers());
    const files: Record<string, [string | Buffer, string] | undefined> = {
      '/test.html': [
        `<!doctype html><html lang="en"><head><meta charset="utf-8" />
        <link rel="stylesheet" href="test.css" /></head><body>${html}
        <script src="vehicle-sil-timeline.js"></script><script src="test.js"></script></body></html>`,
        'text/html',
      ],
      '/test.js': [script, 'text/javascript'],
      '/test.css': [css, 'text/css'],
      '/example.html': [readFileSync(join(TIMELINE, 'example.html')), 'text/html'],
      '/run.timeline.json': [
        readFileSync(join(REFERENCES, 'fails_beside.timeline.json')),
        'application/json',
      ],
      '/vehicle-sil-timeline.js': [
        readFileSync(join(TIMELINE, 'vehicle-sil-timeline.js')),
        'text/javascript',
      ],
    };
    if (own) {
      await route.fulfill({ status: own.status, body: own.body, contentType: 'application/json' });
      return;
    }
    let file = files[pathname];
    if (!file && pathname.startsWith('/data/')) {
      try {
        file = [
          readFileSync(join(REFERENCES, pathname.slice('/data/'.length))),
          'application/json',
        ];
      } catch {
        file = undefined;
      }
    }
    if (!file) {
      await route.fulfill({ status: 404, body: 'not found' });
      return;
    }
    await route.fulfill({
      body: file[0],
      contentType: file[1],
      headers: { 'Content-Security-Policy': CSP },
    });
  });
  return problems;
}

/** Collects the element's events (type and detail) in window.__events. */
const RECORD = `
window.__events = [];
for (const el of document.querySelectorAll('vehicle-sil-timeline'))
  for (const type of ['load', 'error', 'cursorchange', 'stepselect', 'viewchange'])
    el.addEventListener(type, (e) => window.__events.push([el.id, e.type, e.detail]));
`;

async function events(page: Page): Promise<[string, string, unknown][]> {
  return page.evaluate(
    () => (window as unknown as { __events: [string, string, unknown][] }).__events,
  );
}

/** Sets properties of the element with this id (in one go, as a framework does). */
async function set(page: Page, id: string, values: Record<string, unknown>): Promise<void> {
  await page.evaluate(
    ([id, values]) => Object.assign(document.getElementById(id as string)!, values),
    [id, values] as const,
  );
}

function lanes(el: Locator): Locator {
  return el.locator('#chart .lane:not(.axis):not(.steps) .var-name');
}

test('timeline element: the plain example shows its file with one script and one tag', async ({
  page,
}) => {
  const problems = await serve(page);
  await page.goto(`${ORIGIN}/example.html`);
  const el = page.locator('vehicle-sil-timeline');
  await expect(el.getByRole('heading', { level: 1 })).toHaveText(
    'the expect beside a ramp fails at a sample',
  );
  await expect(el.getByTestId('failure').getByRole('heading', { level: 2 })).toHaveText(
    'Step 2 failed at 140 ms',
  );
  await expect(el.locator('#facts')).toContainText('timeline file run.timeline.json');
  // Its CSS came in without a <style> (the policy allows none)
  await expect(el.getByRole('button', { name: 'Whole run' })).toHaveCSS('border-radius', '6px');
  const shots = process.env['VEHICLE_SIL_GUI_SHOTS'];
  if (shots) {
    await page.setViewportSize({ width: 1280, height: 900 });
    await page.screenshot({ path: `${shots}/element-plain-light-1280.png`, fullPage: true });
  }
  expect(problems).toEqual([]);
});

test('timeline element: two on one page keep their own file, language, cursor and events', async ({
  page,
}) => {
  const problems = await serve(
    page,
    `<vehicle-sil-timeline id="a" src="data/fails_beside.timeline.json" from="100" to="140"></vehicle-sil-timeline>
     <vehicle-sil-timeline id="b" lang="ja"></vehicle-sil-timeline>`,
    `${RECORD}
     fetch('data/motions.timeline.json').then((r) => r.json()).then((json) => {
       const b = document.getElementById('b');
       b.fileName = 'motions.timeline.json';
       b.data = json;
     });`,
  );
  await page.goto(`${ORIGIN}/test.html`);
  const a = page.locator('#a');
  const b = page.locator('#b');
  await expect(a.getByTestId('failure')).toBeVisible();
  await expect(b.getByTestId('passed').getByRole('heading')).toHaveText('通りました: 6 個の手順');
  await expect(b.locator('#facts')).toContainText('motions.timeline.json');
  await expect(a.getByRole('button', { name: 'Whole run' })).toBeVisible();
  await expect(b.getByRole('button', { name: '実行の全体' })).toBeVisible();
  await expect(lanes(a)).toHaveCount(2);
  // The attributes from and to give the first view
  await expect(a.locator('#range')).toHaveText(/^Showing 100 ms to 140 ms of /);
  await expect(lanes(b)).toHaveCount(6);

  // load: once each, with what was loaded; set from outside, nothing else is told back
  const loads = (await events(page)).sort(([x], [y]) => x.localeCompare(y));
  expect(loads).toEqual([
    [
      'a',
      'load',
      {
        file: 'fails_beside.timeline.json',
        scenario: 'the expect beside a ramp fails at a sample',
        status: 'failed',
        start: 0,
        end: expect.any(Number),
      },
    ],
    [
      'b',
      'load',
      {
        file: 'motions.timeline.json',
        scenario: 'set, ramp and waves, with an expect beside',
        status: 'passed',
        start: 0,
        end: expect.any(Number),
      },
    ],
  ]);

  // The user moves the cursor of a with the keys: a says so, b does not move
  await a.locator('#chart').focus();
  await page.keyboard.press('ArrowRight');
  await expect(a.locator('#cursor-time')).toHaveText(/^Cursor /);
  await expect(b.locator('#cursor-time')).toHaveText('仮想時刻');
  // The user chooses a step in the table of b, and shows the whole run of a
  await b.getByTestId('steps-table').getByRole('button', { name: '#3' }).click();
  await expect(b.getByTestId('steps-table').locator('tr.selected')).toHaveCount(1);
  await a.getByRole('button', { name: 'Whole run' }).click();
  const told = (await events(page)).slice(2).map(([id, type]) => `${id} ${type}`);
  expect(told).toEqual(['a cursorchange', 'b viewchange', 'b stepselect', 'a viewchange']);
  const detail = (await events(page)).slice(2);
  expect(detail[0]![2]).toEqual({ cursor: expect.any(Number) });
  expect(detail[2]![2]).toEqual({ step: 3 });
  expect(detail[3]![2]).toEqual({ from: 0, to: expect.any(Number) });
  // A click on a step of the chart chooses it too
  await b.locator('#chart .lane.steps g.step[data-step="4"]').click();
  await expect
    .poll(async () => (await events(page)).at(-1)?.slice(1))
    .toEqual(['stepselect', { step: 4 }]);
  await expect(b.locator('#chart g.step.selected')).toHaveAttribute('data-step', '4');
  if (process.env['VEHICLE_SIL_GUI_SHOTS']) {
    await page.setViewportSize({ width: 1280, height: 900 });
    await page.screenshot({
      path: `${process.env['VEHICLE_SIL_GUI_SHOTS']}/element-two-light-1280.png`,
      fullPage: true,
    });
  }
  expect(problems).toEqual([]);
});

test('timeline element: properties set from outside change what it shows, and are not told back', async ({
  page,
}) => {
  const problems = await serve(
    page,
    `<vehicle-sil-timeline id="t" src="data/motions.timeline.json"></vehicle-sil-timeline>`,
    RECORD,
  );
  await page.goto(`${ORIGIN}/test.html`);
  const el = page.locator('#t');
  await expect(lanes(el)).toHaveCount(6);
  const range = el.locator('#range');

  // The view, the cursor and the step, as a screen keeps two things in step
  await set(page, 't', { from: 100, to: 200, cursor: 150 });
  await expect(range).toHaveText(/^Showing 100 ms to 200 ms of /);
  await expect(el.locator('#cursor-time')).toHaveText('Cursor 150 ms');
  await expect(el.locator('#cursor-steps')).toHaveText('#4 wave (passed)');
  expect(await page.evaluate(() => document.querySelector('vehicle-sil-timeline')!.from)).toBe(100);
  await set(page, 't', { step: 2 });
  await expect(el.getByTestId('steps-table').locator('tr.selected td').first()).toHaveText('#2');
  await expect(range).not.toHaveText(/^Showing 100 ms to 200 ms/);
  await set(page, 't', { step: null });
  await expect(el.getByTestId('steps-table').locator('tr.selected')).toHaveCount(0);

  // Which lanes, in which order, and their labels
  await set(page, 't', { variables: ['Door.mode', 'Body.*'], labels: { 'Door.mode': 'Mode' } });
  await expect(lanes(el)).toHaveText(['Door.mode', 'Body.level', 'Body.body.speed']);
  await expect(el.locator('.lane .var-label')).toHaveText(['Mode']);
  await expect(el.locator('#chart .group')).toHaveCount(0);
  await page.evaluate(() =>
    document.getElementById('t')!.setAttribute('variables', 'Door.lamp, Door.ratio'),
  );
  await expect(lanes(el)).toHaveText(['Door.lamp', 'Door.ratio']);
  await set(page, 't', { variables: null });
  await expect(lanes(el)).toHaveCount(6);

  // Parts left out, the compact lanes and the theme
  const head = el.locator('.lane:not(.axis):not(.steps) .lane-head').first();
  const wide = (await head.boundingBox())!.width;
  await set(page, 't', { hide: 'header summary toolbar help tables steps readout legend' });
  for (const part of ['#head', '#summary', '#toolbar', '#help', '#tables', '.lane.steps'])
    await expect(el.locator(part)).toBeHidden();
  await expect(el.locator('.readout').first()).toBeHidden();
  await expect(el.locator('.legend .key').first()).toBeHidden();
  await expect(el).toHaveAttribute(
    'hide',
    'header summary toolbar help tables steps readout legend',
  );
  await set(page, 't', { hide: '', compact: true });
  await expect(el.locator('#head')).toBeVisible();
  await expect.poll(async () => (await head.boundingBox())!.width).toBeLessThan(wide);
  await set(page, 't', { theme: 'dark' });
  await expect(el).toHaveCSS('background-color', 'rgb(19, 21, 24)');
  await set(page, 't', { theme: 'light', compact: false });
  await expect(el).toHaveCSS('background-color', 'rgb(247, 247, 245)');

  // The language, words of the page's own (and a language it adds), and back
  await set(page, 't', { lang: 'ja' });
  await expect(el.getByRole('button', { name: '実行の全体' })).toBeVisible();
  await set(page, 't', {
    messages: { en: { 'page.whole': 'Everything' }, de: { 'page.whole': 'Alles' } },
    lang: 'de',
  });
  await expect(el.getByRole('button', { name: 'Alles' })).toBeVisible();
  await expect(el.getByRole('button', { name: 'Zoom out' })).toBeVisible();
  await set(page, 't', { lang: 'en' });
  await expect(el.getByRole('button', { name: 'Everything' })).toBeVisible();

  // Nothing of this was told back as the user's doing
  expect((await events(page)).map(([, type]) => type)).toEqual(['load']);
  expect(problems).toEqual([]);
});

test('timeline element: src with a token and headers; data before src; errors say why', async ({
  page,
}) => {
  const asked: Record<string, string>[] = [];
  const problems = await serve(
    page,
    `<vehicle-sil-timeline id="t"></vehicle-sil-timeline>`,
    RECORD,
    '',
    (path, headers) => {
      if (path !== '/api/timeline/1') return null;
      asked.push(headers);
      if (headers['authorization'] !== 'Bearer secret')
        return { status: 401, body: '{"error":{"code":"unauthorized","message":"no"}}' };
      return {
        status: 200,
        body: readFileSync(join(REFERENCES, 'write_refused.timeline.json'), 'utf8'),
      };
    },
  );
  await page.goto(`${ORIGIN}/test.html`);
  const el = page.locator('#t');
  await expect(el.getByRole('heading', { level: 1 })).toHaveText('Scenario timeline');

  await set(page, 't', { src: '/api/timeline/1' });
  await expect(el.getByRole('alert')).toContainText(
    'HTTP 401: the server did not accept the token',
  );
  await expect(el.getByRole('alert')).toContainText('token property');
  expect((await events(page)).at(-1)!.slice(1)).toEqual([
    'error',
    { message: expect.stringContaining('HTTP 401'), status: 401 },
  ]);
  // Without the property, the token that the GUI's screens keep in the tab
  await page.evaluate(() => {
    sessionStorage.setItem('vehicle-sil-gui-token', 'secret');
    document.querySelector('vehicle-sil-timeline')!.reload();
  });
  await expect(el.getByRole('heading', { level: 1 })).toHaveText('an ECU refuses a write');
  expect(asked.at(-1)).toMatchObject({ authorization: 'Bearer secret' });
  // The property comes first, with more headers
  await set(page, 't', { token: 'other', headers: { 'X-Screen': 'mine' } });
  await expect(el.getByRole('alert')).toContainText('HTTP 401');
  await set(page, 't', { token: 'secret' });
  await expect(el.getByRole('heading', { level: 1 })).toHaveText('an ECU refuses a write');
  expect(asked.at(-1)).toMatchObject({ authorization: 'Bearer secret', 'x-screen': 'mine' });

  // data is shown instead of src while it is set
  const data = JSON.parse(readFileSync(join(REFERENCES, 'motions.timeline.json'), 'utf8'));
  await set(page, 't', { data });
  await expect(el.getByTestId('passed')).toBeVisible();
  await set(page, 't', { data: null });
  await expect(el.getByRole('heading', { level: 1 })).toHaveText('an ECU refuses a write');
  const before = asked.length;
  await page.evaluate(() => document.querySelector('vehicle-sil-timeline')!.reload());
  await expect.poll(() => asked.length).toBe(before + 1);

  // A file it cannot read, and an address on another server
  await set(page, 't', { data: '{"vehicle_sil_timeline": 2}', fileName: 'next.json' });
  await expect(el.getByRole('alert')).toContainText('"next.json" is timeline format 2');
  await expect(el.locator('#view')).toBeHidden();
  await set(page, 't', { data: null, fileName: '', src: 'http://elsewhere.test/a.json' });
  await expect(el.getByRole('alert')).toContainText(
    "src must name a file on this page's own server (http://element.test)",
  );
  // The browser reports the refused request itself
  expect(problems.filter((p) => !/status of 401/.test(p))).toEqual([]);
});

test("timeline element: the page's CSS does not reach in, its CSS does not leak out, and colours can be set", async ({
  page,
}) => {
  const problems = await serve(
    page,
    `<p class="muted" id="outside">outside</p>
     <vehicle-sil-timeline id="t" src="data/fails_beside.timeline.json"></vehicle-sil-timeline>`,
    '',
    `button { color: rgb(255, 0, 0) !important; }
     .lane { display: none; }
     #t { --bad: rgb(1, 2, 3); }`,
  );
  await page.goto(`${ORIGIN}/test.html`);
  const el = page.locator('#t');
  await expect(el.getByTestId('failure')).toBeVisible();
  await expect(el.getByRole('button', { name: 'Whole run' })).not.toHaveCSS(
    'color',
    'rgb(255, 0, 0)',
  );
  await expect(el.locator('#chart .lane').first()).toBeVisible();
  // The element's .muted is grey; the page's .muted is the page's own
  await expect(page.locator('#outside')).toHaveCSS('color', 'rgb(0, 0, 0)');
  // A colour set on the element from outside
  await expect(el.getByTestId('failure').getByRole('heading', { level: 2 })).toHaveCSS(
    'color',
    'rgb(1, 2, 3)',
  );
  expect(problems).toEqual([]);
});

test('timeline element: a property set before the script defines the element is used', async ({
  page,
}) => {
  const data = readFileSync(join(REFERENCES, 'fails_beside.timeline.json'), 'utf8');
  await page.route(`${ORIGIN}/late.html`, (route) =>
    route.fulfill({
      contentType: 'text/html',
      headers: { 'Content-Security-Policy': CSP },
      body: '<!doctype html><body><vehicle-sil-timeline></vehicle-sil-timeline><script src="late.js"></script></body>',
    }),
  );
  await page.route(`${ORIGIN}/late.js`, (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: `document.querySelector('vehicle-sil-timeline').data = ${JSON.stringify(data)};
        const s = document.createElement('script'); s.src = 'vehicle-sil-timeline.js'; document.body.append(s);`,
    }),
  );
  await page.route(`${ORIGIN}/vehicle-sil-timeline.js`, (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: readFileSync(join(TIMELINE, 'vehicle-sil-timeline.js')),
    }),
  );
  await page.goto(`${ORIGIN}/late.html`);
  await expect(page.locator('vehicle-sil-timeline').getByTestId('failure')).toBeVisible();
});

test('timeline element: an older load that answers late is not shown over a newer file', async ({
  page,
}) => {
  const problems = await serve(
    page,
    `<vehicle-sil-timeline id="t"></vehicle-sil-timeline>`,
    RECORD,
  );
  let answered = false;
  await page.route(`${ORIGIN}/slow.json`, async (route) => {
    await new Promise((resolve) => setTimeout(resolve, 500));
    answered = true;
    await route
      .fulfill({ body: readFileSync(join(REFERENCES, 'motions.timeline.json')) })
      .catch(() => undefined); // the element may have stopped the request
  });
  await page.goto(`${ORIGIN}/test.html`);
  const el = page.locator('#t');
  await set(page, 't', { src: 'slow.json' });
  await set(page, 't', {
    data: readFileSync(join(REFERENCES, 'fails_beside.timeline.json'), 'utf8'),
  });
  await expect(el.getByTestId('failure')).toBeVisible();
  await expect.poll(() => answered).toBe(true);
  await page.waitForTimeout(200);
  await expect(el.getByTestId('failure')).toBeVisible();
  expect((await events(page)).map(([, type]) => type)).toEqual(['load']);
  expect(problems).toEqual([]);
});
