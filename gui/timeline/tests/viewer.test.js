// Unit tests of the parts of viewer.js that need no DOM: reading a file, the model, and the layout
// (time to position, the rows of the steps, held segments, thinning out). They read the timeline references
// that the launcher's tests compare (tests/launch/timeline), so the page is tried on what the launcher writes.
// Run with `npm run test:timeline` in gui/web (scripts/verify-web.py runs it).
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const T = require('../viewer.js');

const REFERENCES = path.join(__dirname, '..', '..', '..', 'tests', 'launch', 'timeline');

function reference(name) {
  return JSON.parse(fs.readFileSync(path.join(REFERENCES, `${name}.timeline.json`), 'utf8'));
}

function model(data) {
  const read = T.readTimeline(data);
  assert.equal(read.error, undefined, read.error);
  return T.buildModel(read.timeline);
}

const series = (t, v, extra) => ({
  source: 'written',
  t,
  v,
  base: null,
  notAppliedFrom: null,
  ...extra,
});

test('every reference file is read, with a lane for each variable and nothing it cannot draw', () => {
  const names = fs.readdirSync(REFERENCES).filter((f) => f.endsWith('.timeline.json'));
  assert.ok(names.length >= 5, 'the references are there');
  for (const name of names) {
    const text = fs.readFileSync(path.join(REFERENCES, name), 'utf8');
    const read = T.readTimeline(text, name);
    assert.equal(read.error, undefined, `${name}: ${read.error}`);
    const m = T.buildModel(read.timeline);
    assert.equal(m.vars.length, read.timeline.variables.length, name);
    assert.deepEqual(
      m.vars.flatMap((v) => v.notes),
      [],
      name,
    );
    assert.equal(m.inputs.length + m.others.length, m.vars.length, name);
    assert.equal(Boolean(m.failure), read.timeline.scenario.status === 'failed', name);
    assert.ok(m.domain[1] >= (read.timeline.scenario.recorded_to_ms ?? 0), name);
  }
});

test('a file that is not a timeline of format 1 and stage run is refused with what to do', () => {
  const good = reference('waits');
  const cases = [
    ['{', /is not JSON/],
    [[], /no "vehicle_sil_timeline" key/],
    [{ ...good, vehicle_sil_timeline: 2 }, /format 2 .*reads format 1 only/],
    [{ ...good, stage: 'preview' }, /stage "preview".*draws only "run"/],
    [{ ...good, steps: undefined }, /no "steps" array/],
    [{ ...good, series: [1] }, /series\[0\] is not an object/],
    [{ ...good, scenario: null }, /no "scenario" object/],
  ];
  for (const [input, pattern] of cases) {
    const read = T.readTimeline(
      typeof input === 'string' ? input : JSON.stringify(input),
      'x.json',
    );
    assert.match(read.error ?? '', pattern);
    assert.match(read.error, /"x\.json"/);
  }
  assert.match(
    T.readTimeline({ ...good, vehicle_sil_timeline: 2 }).error,
    /written by vehicle-sil/,
  );
});

test('keys the page does not know, and keys that later versions add, change nothing', () => {
  const plain = model(reference('motions'));
  const data = reference('motions');
  data.events = [{ kind: 'first_clip', at_ms: 30, text: 'clipped' }];
  data.future = { anything: true };
  data.series[0].clipped = [1, 2];
  data.lines[0].background = true;
  data.lines[0].stopped = 'replaced';
  data.lines[0].stopped_at_ms = 10;
  data.steps[0].background = [0];
  const m = model(data);
  assert.deepEqual(
    m.vars.map((v) => [v.name, v.notes, v.written?.t, v.sampled?.t]),
    plain.vars.map((v) => [v.name, v.notes, v.written?.t, v.sampled?.t]),
  );
  assert.deepEqual(m.domain, plain.domain);
});

test('values the page does not know are named, not drawn wrongly', () => {
  const data = reference('waits');
  data.series.push({
    variable: 'Body.level',
    source: 'computed',
    encoding: 'all',
    points: [[0, 1]],
  });
  data.series.push({
    variable: 'Body.body.speed',
    source: 'sampled',
    encoding: 'minmax',
    points: [[0, 1, 2, 3]],
  });
  data.steps[1].watch.mode = 'between';
  data.steps[4].watch.conditions[0].compare[0].op = 'within';
  data.steps[0].kind = 'freeze';
  data.steps[0].status = 'paused';
  data.lines[0].kind = 'freeze';
  const m = model(data);
  const level = m.byName.get('Body.level');
  const speed = m.byName.get('Body.body.speed');
  assert.ok(level.notes.some((n) => /does not know the source "computed"/.test(n)));
  assert.ok(speed.notes.some((n) => /does not know the encoding "minmax"/.test(n)));
  assert.ok(speed.notes.some((n) => /step 2 .*does not know the mode "between"/.test(n)));
  assert.ok(speed.notes.some((n) => /step 5 .*does not know "within"/.test(n)));
  // The known ones are still drawn: the sampled series that came first, the wait of step 4.
  assert.equal(speed.sampled.t.length, 7);
  assert.deepEqual(
    speed.bands.map((b) => b.step),
    [4],
  );
  assert.equal(m.lines[0].kind, 'freeze');
});

test('a refused write: which writes are in the ECU, which are not, and which one is the cause', () => {
  const m = model(reference('write_refused'));
  const f = m.failure;
  assert.equal(f.step.index, 2);
  assert.equal(f.at, 40);
  assert.equal(f.code, 'scenario-write');
  assert.deepEqual(
    f.writes.map((w) => [w.variable, w.valueText, w.applied, w.cause]),
    [
      ['Body.mode', 'auto (2)', true, false],
      ['Body.level', '13', false, true],
    ],
  );
  assert.equal(m.byName.get('Body.level').failing, true);
  assert.equal(m.byName.get('Body.mode').failing, false, 'an applied write is not a failure');
  // The ECU kept 7: the written series holds 7 up to the end, and 13 is drawn as not applied.
  const segs = T.holdSegments(m.byName.get('Body.level').written, m.recordedTo, null);
  assert.deepEqual(
    segs.map((x) => [x.from, x.to, x.v, x.state]),
    [
      [0, 40, 7, 'sent'],
      [40, 40, 13, 'not-applied'],
      [40, 40, 7, 'held'],
    ],
  );
  assert.deepEqual(T.valueAt(m.byName.get('Body.level').written, 40, 40), {
    value: 7,
    notApplied: 13,
  });
});

test('a condition that failed: its lane, its value and its band; the other condition of the step is not red', () => {
  const m = model(reference('fails_beside'));
  assert.deepEqual(m.failure.conditions, [
    {
      variable: 'Body.body.speed',
      compare: 'le 200',
      value: 210,
      valueText: '210',
      sampleMs: 140,
      mode: 'during',
    },
  ]);
  const speed = m.byName.get('Body.body.speed');
  assert.equal(speed.failing, true);
  assert.deepEqual(
    speed.bands.map((b) => [b.step, b.mode, b.from, b.to, b.failed]),
    [[2, 'during', 50, 250, true]],
  );
  const level = m.byName.get('Body.level');
  assert.equal(level.failing, false);
  // Step 3 did not run, so its condition has no interval and no band.
  assert.deepEqual(
    level.bands.map((b) => [b.step, b.failed, b.status]),
    [[2, false, 'failed']],
  );
  assert.deepEqual(speed.marks, [{ kind: 'condition', at: 140, value: 210, text: '210' }]);
});

test('a wait that held is a band up to the time it held, with its limit beside', () => {
  const m = model(reference('waits'));
  const speed = m.byName.get('Body.body.speed');
  const wait = speed.bands.find((b) => b.step === 4);
  assert.deepEqual([wait.mode, wait.from, wait.to, wait.limit], ['once', 80, 80, 580]);
  const within = speed.bands.find((b) => b.step === 2);
  assert.deepEqual([within.from, within.to, within.limit], [0, 10, 200]);
});

test('the page opens on the failure: a little before the failed step to the end of the record', () => {
  const failed = model(reference('fails_beside'));
  const [t0, t1] = T.initialView(failed);
  assert.ok(t0 > 0 && t0 < 50, `starts before the failed step (${t0})`);
  assert.ok(t1 > 140 && t1 <= failed.domain[1], `ends after the record (${t1})`);
  const passed = model(reference('motions'));
  assert.deepEqual(T.initialView(passed), passed.domain);
});

test('time to position and back, and round axis ticks', () => {
  const scale = T.makeScale(100, 300, 400);
  assert.equal(scale.x(100), 0);
  assert.equal(scale.x(300), 400);
  assert.equal(scale.x(150), 100);
  assert.equal(scale.t(100), 150);
  assert.deepEqual(T.niceTicks(0, 100, 5), [0, 20, 40, 60, 80, 100]);
  assert.deepEqual(T.niceTicks(23, 153.5, 4), [50, 100, 150]);
});

test('steps that overlap go on separate rows, others share one', () => {
  const { rows, count } = T.packRows(
    [
      { x0: 0, x1: 50 },
      { x0: 20, x1: 60 },
      { x0: 55, x1: 80 },
      { x0: 70, x1: 90 },
    ],
    4,
  );
  assert.deepEqual(rows, [0, 1, 0, 1]);
  assert.equal(count, 2);
});

test('points from not_applied_from on are not applied, and the ECU keeps the value before them', () => {
  const segs = T.holdSegments(
    series([0, 10, 20, 30], [1, 2, 3, 4], { notAppliedFrom: 2 }),
    50,
    null,
  );
  assert.deepEqual(
    segs.map((x) => [x.from, x.to, x.v, x.state]),
    [
      [0, 10, 1, 'sent'],
      [10, 20, 2, 'sent'],
      [20, 30, 3, 'not-applied'],
      [30, 50, 4, 'not-applied'],
      [20, 50, 2, 'held'],
    ],
  );
  const s = series([0, 10, 20, 30], [1, 2, 3, 4], { notAppliedFrom: 2 });
  assert.deepEqual(T.valueAt(s, 25, 50), { value: 2, notApplied: 3 });
  assert.deepEqual(T.valueAt(s, 15, 50), { value: 2 });
  assert.equal(T.valueAt(s, -1, 50), null);
});

test('points are not joined across the interval whose points were dropped at the limits', () => {
  const gap = { from: 10, before: 30, dropped: 5 };
  const segs = T.holdSegments(series([0, 10, 20, 30, 40], [1, 2, 3, 4, 5]), 60, gap);
  assert.deepEqual(
    segs.map((x) => [x.from, x.to, x.state]),
    [
      [0, 10, 'sent'],
      [10, 10, 'alone'],
      [20, 20, 'alone'],
      [30, 40, 'sent'],
      [40, 60, 'sent'],
    ],
  );
  // A hold that would cross the start of the interval stops there.
  const across = T.holdSegments(series([0, 30], [1, 2]), 60, { from: 10, before: 30 });
  assert.deepEqual(
    across.map((x) => [x.from, x.to]),
    [
      [0, 10],
      [30, 60],
    ],
  );
});

test('a sampled value lasts until last_ms, not beyond', () => {
  const s = { ...series([0, 10], [5, 6]), source: 'sampled', lastMs: 30 };
  assert.deepEqual(T.valueAt(s, 30, 100), { value: 6 });
  assert.equal(T.valueAt(s, 31, 100), null);
  const segs = T.holdSegments(s, 30, null);
  assert.deepEqual(
    segs.map((x) => [x.from, x.to, x.state]),
    [
      [0, 10, 'seen'],
      [10, 30, 'seen'],
    ],
  );
});

test('many points are thinned out to about 2 per pixel column, keeping the min and max', () => {
  const n = 100000;
  const t = Array.from({ length: n }, (_, i) => i);
  const v = t.map(
    (i) => Math.sin(i / 50) * 100 + (i === 41234 ? 500 : 0) - (i === 77777 ? 500 : 0),
  );
  const segs = T.holdSegments(series(t, v), n, null);
  const scale = T.makeScale(0, n, 500);
  const shown = T.decimate(segs, scale);
  assert.ok(shown.length <= 1000, `${shown.length} segments`);
  assert.equal(Math.max(...shown.map((x) => x.max)), Math.max(...v));
  assert.equal(Math.min(...shown.map((x) => x.min)), Math.min(...v));
  // Few points in view: kept as they are.
  const zoomed = T.decimate(segs, T.makeScale(1000, 1100, 500));
  assert.ok(zoomed.every((x) => x.min === x.v && x.max === x.v));
  assert.ok(zoomed.length >= 100 && zoomed.length <= 103);
});

test('a lane starts in the declared range unless the values would be a flat line in it', () => {
  const lane = (min, max, values) => ({
    info: { type: 'integer', min, max },
    written: series(
      values.map((_, i) => i),
      values,
    ),
    sampled: null,
    bands: [],
    marks: [],
  });
  assert.equal(T.defaultFit(lane(0, 100, [14, 30])), true);
  assert.equal(T.defaultFit(lane(0, 100, [0, 90])), false);
  assert.equal(T.defaultFit(lane(null, null, [0, 90])), true);
  assert.deepEqual(T.laneRange(lane(0, 100, [14, 30]), false), [0, 100]);
  const [lo, hi] = T.laneRange(lane(0, 100, [14, 30]), true);
  assert.ok(lo < 14 && lo > 12 && hi > 30 && hi < 32);
  assert.deepEqual(T.laneRange(lane(0, 100, [7, 7]), true), [6.3, 7.7]);
});

test('neighbouring segments with the same value are joined for bands', () => {
  const segs = T.holdSegments(series([0, 10, 20, 30], [0, 0, 2, 2]), 40, null);
  assert.deepEqual(
    T.mergeEqual(segs).map((x) => [x.from, x.to, x.v]),
    [
      [0, 20, 0],
      [20, 40, 2],
    ],
  );
});

test('values are written the way the launcher writes them', () => {
  const mode = { type: 'enum', items: [{ name: 'auto', value: 2 }] };
  assert.equal(T.fmtValue(2, mode), 'auto (2)');
  assert.equal(T.fmtValue(3, mode), '3');
  assert.equal(T.fmtValue(true), 'true');
  assert.equal(T.fmtValue(null), 'null');
  assert.equal(T.fmtValue(0.5348311458534407), '0.534831');
  assert.equal(T.fmtValue(0.5348311458534407, null, true), '0.5348311458534407');
  assert.equal(T.compareText([{ op: 'near', value: 200, tolerance: 0.5 }]), 'near 200 ± 0.5');
  assert.equal(T.fmtMs(12.5), '12.5 ms');
});

test('viewer.js can be put inline in a page: it has no closing script tag and no comment opener', () => {
  const source = fs.readFileSync(path.join(__dirname, '..', 'viewer.js'), 'utf8');
  assert.doesNotMatch(source, /<\/script/i);
  assert.doesNotMatch(source, /<!--/);
});
