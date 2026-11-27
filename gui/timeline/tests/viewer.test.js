// Unit tests of the parts of viewer.js that need no DOM: reading a file, the model, and the layout
// (time to position, the rows of the steps, held segments, thinning out). They read the timeline references
// that the launcher's tests compare (tests/launch/timeline), so the page is tried on what the launcher writes,
// and the fixtures beside them (fixtures/) for what those do not have: a bool or an enum write that was not
// applied, a write whose result is not known, points dropped at the limits. tests/schema/test_schemas.py holds
// the fixtures to the schema of the file. Run with `npm run test:timeline` in gui/web (scripts/verify-web.py runs it).
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { bundle, OUT } = require('../bundle.js');

/**
 * Runs plain scripts of gui/timeline (no DOM) as a page would load them, with a global of their own (in this realm,
 * so that the tests compare their arrays and objects): that global.
 */
function run(code) {
  const global = {};
  new Function('globalThis', code)(global);
  return global;
}
const SOURCES = run(
  ['messages.js', 'viewer.js']
    .map((name) => fs.readFileSync(path.join(__dirname, '..', name), 'utf8'))
    .join('\n'),
);
const T = SOURCES.VehicleSilTimeline;

const REFERENCES = path.join(__dirname, '..', '..', '..', 'tests', 'launch', 'timeline');
const FIXTURES = path.join(__dirname, 'fixtures');

function reference(name) {
  return JSON.parse(fs.readFileSync(path.join(REFERENCES, `${name}.timeline.json`), 'utf8'));
}

function fixture(name) {
  return JSON.parse(fs.readFileSync(path.join(FIXTURES, `${name}.timeline.json`), 'utf8'));
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

test('every reference file and fixture is read, with a lane for each variable and nothing it cannot draw', () => {
  const files = [REFERENCES, FIXTURES].flatMap((dir) =>
    fs
      .readdirSync(dir)
      .filter((f) => f.endsWith('.timeline.json'))
      .map((f) => path.join(dir, f)),
  );
  assert.ok(files.length >= 8, 'the references and the fixtures are there');
  for (const file of files) {
    const name = path.basename(file);
    const text = fs.readFileSync(file, 'utf8');
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

test('the fixtures say only what the launcher writes: points within the record, counts that add up', () => {
  const files = [REFERENCES, FIXTURES].flatMap((dir) =>
    fs
      .readdirSync(dir)
      .filter((f) => f.endsWith('.timeline.json'))
      .map((f) => path.join(dir, f)),
  );
  for (const file of files) {
    const name = path.basename(file);
    const tl = JSON.parse(fs.readFileSync(file, 'utf8'));
    const to = tl.scenario.recorded_to_ms;
    // No point after the end of the record; a failed run's record ends at the failed step's time.
    for (const s of tl.series)
      for (const p of s.points)
        assert.ok(to !== null && p[0] <= to, `${name}: ${s.variable} ${p[0]}`);
    const failed = tl.steps.find((st) => st.status === 'failed');
    if (tl.scenario.status === 'failed') assert.equal(to, failed.at_ms, `${name}: recorded_to_ms`);
    // The points a limit counts: all of a sampled series, and of a written one all but the points of a set.
    const counted = (s) => {
      if (s.source === 'sampled') return s.points;
      const sets = tl.lines
        .filter((l) => l.variable === s.variable && l.kind === 'set')
        .map((l) => l.from_ms);
      return s.points.filter((p) => {
        const i = sets.indexOf(p[0]);
        if (i >= 0) sets.splice(i, 1);
        return i < 0;
      });
    };
    const { points_per_series: perSeries, points_total: total, dropped } = tl.limits;
    let all = 0;
    for (const s of tl.series) {
      const n = counted(s).length;
      all += n;
      assert.ok(n <= perSeries, `${name}: ${s.variable} ${s.source} has ${n} points`);
    }
    assert.ok(all <= total, name);
    for (const d of dropped) {
      const s = tl.series.find((x) => x.variable === d.variable && x.source === d.source);
      const kept = counted(s);
      assert.ok(d.dropped_from_ms <= d.dropped_before_ms, name);
      assert.equal(kept[0][0], d.dropped_before_ms, `${name}: the oldest point kept`);
      if (s.source === 'written' && all < total) assert.equal(kept.length, perSeries, name);
    }
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
  // Values that are not of their variable's type (a hand-edited or broken file): named, and not drawn.
  const bools = reference('motions');
  const lamp = bools.series.find((s) => s.variable === 'Door.lamp' && s.source === 'written');
  lamp.points = [
    [0, 'false'],
    [10, 1],
    [20, true],
  ];
  const ratio = bools.series.find((s) => s.variable === 'Door.ratio');
  ratio.points[1][1] = true;
  const typed = model(bools);
  assert.deepEqual(typed.byName.get('Door.lamp').notes, [
    'Note: 2 values of the written series are not true or false, as its type bool says, so they are not drawn (the first: "false" at 0 ms).'.slice(
      'Note: '.length,
    ),
  ]);
  assert.deepEqual(typed.byName.get('Door.lamp').written.v, [undefined, undefined, true]);
  assert.match(
    typed.byName.get('Door.ratio').notes.join(),
    /1 value of the written series is not a number/,
  );
  assert.equal(
    T.readoutText('sent', T.valueAt(typed.byName.get('Door.lamp').written, 5, 100)),
    'sent: a value that is not of the type (see the note)',
  );
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
    value: 13,
    state: 'not-applied',
    kept: { value: 7 },
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
    speed.bands.map((b) => [b.step, b.mode, b.from, b.to, b.until, b.failed]),
    [[2, 'during', 50, 140, 250, true]],
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

test('points from not_applied_from on: not applied where the writes say so, else not known', () => {
  // The failed step's write of point 2 was not applied; point 3 was sent after it: not known.
  const s = series([0, 10, 20, 30], [1, 2, 3, 4], {
    notAppliedFrom: 2,
    applied: new Map([[2, false]]),
    beforeApplied: true,
  });
  assert.deepEqual(
    T.holdSegments(s, 50, null).map((x) => [x.from, x.to, x.v, x.state]),
    [
      [0, 10, 1, 'sent'],
      [10, 20, 2, 'sent'],
      [20, 30, 3, 'not-applied'],
      [30, 50, 4, 'not-known'],
      // The ECU keeps 2 only as long as that is known: up to the point whose result is not known.
      [20, 30, 2, 'held'],
    ],
  );
  assert.deepEqual(T.valueAt(s, 25, 50), { value: 3, state: 'not-applied', kept: { value: 2 } });
  assert.deepEqual(T.valueAt(s, 35, 50), { value: 4, state: 'not-known' });
  assert.deepEqual(T.valueAt(s, 15, 50), { value: 2 });
  assert.equal(T.valueAt(s, -1, 50), null);
  // Every point not applied: the ECU keeps the value before them to the end.
  const all = series([0, 10, 20], [1, 2, 3], {
    notAppliedFrom: 1,
    applied: new Map([
      [1, false],
      [2, false],
    ]),
    beforeApplied: true,
  });
  assert.deepEqual(
    T.holdSegments(all, 50, null)
      .filter((x) => x.state === 'held')
      .map((x) => [x.from, x.to, x.v]),
    [[10, 50, 1]],
  );
  assert.deepEqual(T.valueAt(all, 30, 50).kept, { value: 1 });
  // Where the file does not say that the value before was applied, no value is drawn as kept.
  const unsaid = { ...all, beforeApplied: false };
  assert.ok(T.holdSegments(unsaid, 50, null).every((x) => x.state !== 'held'));
  assert.deepEqual(T.valueAt(unsaid, 30, 50).kept, { lastSent: 1 });
});

test('a write whose result is not known is never drawn or worded as not applied', () => {
  const m = model(fixture('applied_unknown'));
  assert.deepEqual(
    m.failure.writes.map((w) => [w.variable, w.applied, w.cause]),
    [
      ['Body.mode', null, false],
      ['Body.level', null, false],
    ],
  );
  for (const name of ['Body.level', 'Body.mode']) {
    const written = m.byName.get(name).written;
    const segs = T.holdSegments(written, m.recordedTo, null);
    assert.ok(
      segs.every((x) => x.state !== 'not-applied' && x.state !== 'held'),
      `${name}: ${JSON.stringify(segs)}`,
    );
    assert.ok(segs.some((x) => x.state === 'not-known'));
  }
  const level = m.byName.get('Body.level');
  const at40 = T.valueAt(level.written, 40, m.recordedTo, null);
  assert.deepEqual(at40, { value: 13, state: 'not-known' });
  const text = T.readoutText('sent', at40, level.info);
  assert.equal(text, 'sent 13 (not known whether the ECU applied it)');
  assert.doesNotMatch(text, /not applied|the ECU has/);
});

test('a bool and an enum write that was not applied stay visible above the value the ECU kept', () => {
  // As the launcher writes it: the record ends at the failed step's time, and a step at that same time sent
  // its write after the refused one (its result is not known). All of it is at 40 ms, with no length.
  const m = model(fixture('state_not_applied'));
  for (const [name, sentValue, laterValue, keptValue] of [
    ['Body.mode', 2, 1, 0],
    ['Body.lamp', true, false, false],
  ]) {
    const written = m.byName.get(name).written;
    // The value before was set by step 1, which passed: the file says it is in the ECU.
    assert.equal(written.beforeApplied, true, name);
    const segs = T.mergeEqual(T.holdSegments(written, m.recordedTo, null));
    assert.deepEqual(
      segs.map((x) => [x.from, x.to, x.v, x.state]),
      [
        [0, 40, keptValue, 'sent'],
        [40, 40, sentValue, 'not-applied'],
        [40, 40, laterValue, 'not-known'],
        [40, 40, keptValue, 'held'],
      ],
      name,
    );
    const layout = T.stateBandLayout(segs, 10, 20);
    const held = layout.findIndex((x) => x.seg.state === 'held');
    const shown = layout.filter(
      (x) => x.seg.state === 'not-applied' || x.seg.state === 'not-known',
    );
    assert.equal(held, 0, 'the kept value is drawn first');
    assert.equal(shown.length, 2);
    // Nothing that is drawn later covers them, and the kept value's strip is under them, not on them.
    for (const x of shown) {
      assert.ok(layout.indexOf(x) > held);
      assert.ok(x.y + x.height <= layout[held].y, `${name}: ${JSON.stringify(layout)}`);
    }
  }
  // At 40 ms the read-out has the last point of that time: the one sent after the refused write.
  const mode = m.byName.get('Body.mode');
  assert.equal(
    T.readoutText('sent', T.valueAt(mode.written, 40, m.recordedTo, null), mode.info),
    'sent on (1) (not known whether the ECU applied it)',
  );
  assert.equal(T.valueAt(mode.written, 41, m.recordedTo, null), null, 'nothing after the record');
});

test('the value the ECU kept is drawn only where the file says that value was applied', () => {
  // The run failed at a write that was refused at once, while the write of 9 for the same time (step 2, a set
  // whose result was not looked at: unfinished) was still waiting. Whether the ECU has 9 or 7 is not known.
  const data = reference('write_refused');
  const [set7, refused, expect] = data.steps;
  data.steps = [
    set7,
    {
      ...set7,
      index: 2,
      line: 8,
      text: 'set Body.level = 9',
      status: 'unfinished',
      start_ms: 40,
      at_ms: null,
      end_ms: null,
      lines: [1],
    },
    { ...refused, index: 3, lines: [2, 3] },
    { ...expect, index: 4 },
  ];
  data.lines = [
    data.lines[0],
    { ...data.lines[0], line: 8, step: 2, from_ms: 40, to_ms: 40, base: 9, text: 'Body.level = 9' },
    ...data.lines.slice(1).map((l) => ({ ...l, step: 3 })),
  ];
  const written = data.series.find((s) => s.variable === 'Body.level' && s.source === 'written');
  written.points = [
    [0, 7],
    [40, 9],
    [40, 13],
  ];
  written.not_applied_from = 2;
  data.steps[2].failed.writes.find((w) => w.variable === 'Body.level').point = 2;
  const m = model(data);
  const level = m.byName.get('Body.level');
  assert.equal(level.written.beforeApplied, false);
  const segs = T.holdSegments(level.written, m.recordedTo, null);
  assert.ok(
    segs.every((x) => x.state !== 'held'),
    JSON.stringify(segs),
  );
  const at = T.valueAt(level.written, 40, m.recordedTo, null);
  assert.deepEqual(at, { value: 13, state: 'not-applied', kept: { lastSent: 9 } });
  const text = T.readoutText('sent', at, level.info);
  assert.equal(
    text,
    'sent 13 (not applied; the ECU keeps what it had: the last value sent before is 9, and the file does not say whether it was applied)',
  );
  assert.doesNotMatch(text, /the ECU has/);
  // The same without the line of the 9: nothing says which step sent it.
  data.lines.splice(1, 1);
  assert.equal(model(data).byName.get('Body.level').written.beforeApplied, false);
  // As the launcher wrote it (7 set by step 1, which passed): the ECU has 7.
  const refusedOnly = model(reference('write_refused')).byName.get('Body.level');
  assert.equal(refusedOnly.written.beforeApplied, true);
  assert.equal(
    T.readoutText('sent', T.valueAt(refusedOnly.written, 40, 40, null), refusedOnly.info),
    'sent 13 (not applied; the ECU has 7)',
  );
});

test('inside the interval whose points were dropped, the read-out says so instead of an older value', () => {
  const m = model(fixture('dropped_ramp'));
  const level = m.byName.get('Body.level');
  const gap = level.gaps.written;
  assert.deepEqual(gap, { from: 10, before: 7990, dropped: 798 });
  // The ramp wrote every 10 ms from 10 ms to 8010 ms: those dropped and those kept.
  assert.equal(gap.dropped + level.written.t.filter((t) => t >= gap.before).length, 801);
  const inside = T.valueAt(level.written, 5000, m.recordedTo, gap);
  assert.deepEqual(inside, { missing: true });
  assert.equal(
    T.readoutText('sent', inside, level.info),
    "sent: values dropped at the file's limits",
  );
  assert.deepEqual(T.valueAt(level.written, 5, m.recordedTo, gap), { value: 5 });
  assert.deepEqual(T.valueAt(level.written, 8005, m.recordedTo, gap), { value: 80.905 });
});

test('the words of a failed write follow the file: refused, not known, not sent', () => {
  const refused = model(reference('write_refused')).failure;
  const words = T.writeWords(refused);
  assert.equal(words.heading, 'Writes for 40 ms');
  assert.match(words.explain, /^Applied: .* Not applied: the ECU refused it/);
  assert.doesNotMatch(words.explain, /never reached|Not known/);
  assert.deepEqual(refused.writes.map(words.cause), ['', 'the cause of the failure']);
  const unknown = T.writeWords(model(fixture('applied_unknown')).failure);
  assert.equal(unknown.explain, 'Not known: no result says whether the ECU applied it.');
  // A step that failed before it sent anything: one write with point null.
  const data = reference('write_refused');
  data.steps[1].failed.writes = [
    { variable: 'Body.level', at_ms: 40, value: 13, point: null, applied: false },
  ];
  data.steps[1].failed.write = 0;
  const notSent = model(data).failure;
  const said = T.writeWords(notSent);
  assert.deepEqual(notSent.writes.map(said.cause), ['not sent: the cause of the failure']);
  assert.doesNotMatch(said.heading + said.explain, /sent together|refused:/);
});

test('a failed write whose point went at the limits is not called "not sent"', () => {
  // point null also means that the point was dropped at the file's limits: limits.dropped names the series.
  const data = reference('write_refused');
  const level = data.steps[1].failed.writes.find((w) => w.variable === 'Body.level');
  level.point = null;
  const mode = data.steps[1].failed.writes.find((w) => w.variable === 'Body.mode');
  mode.point = null;
  mode.applied = null;
  data.limits.dropped = [
    {
      variable: 'Body.level',
      source: 'written',
      dropped: 1,
      dropped_from_ms: 40,
      dropped_before_ms: null,
    },
  ];
  const m = model(data);
  const words = T.writeWords(m.failure);
  assert.deepEqual(m.failure.writes.map(words.cause), [
    // Sent (its result is not known), so not "not sent"; and no series of it lost points.
    '',
    "the cause of the failure; its point was dropped at the file's limits",
  ]);
  const mark = m.byName.get('Body.level').marks.find((mk) => mk.kind === 'write');
  assert.equal(mark.sent, true);
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

test('the one file is what the sources make, and gives the same functions and the CSS', () => {
  assert.equal(fs.readFileSync(OUT, 'utf8').replace(/\r\n/g, '\n'), bundle());
  const one = run(bundle());
  assert.deepEqual(Object.keys(one.VehicleSilTimeline).sort(), Object.keys(T).sort());
  assert.equal(
    one.VehicleSilTimelineStyles,
    '\n' +
      fs
        .readFileSync(path.join(__dirname, '..', 'timeline.css'), 'utf8')
        .replace(/\r\n/g, '\n')
        .trimEnd() +
      '\n',
  );
  // No module system: a bundler takes it as a script that it imports for its effect.
  assert.doesNotMatch(bundle(), /\brequire\(|module\.exports|\bexport\s|^import\s/m);
});

test('the scripts can be put inline in a page: no closing script tag, no comment opener', () => {
  for (const name of ['viewer.js', 'messages.js', 'page.js', 'vehicle-sil-timeline.js']) {
    const source = fs.readFileSync(path.join(__dirname, '..', name), 'utf8');
    assert.doesNotMatch(source, /<\/script/i, name);
    assert.doesNotMatch(source, /<!--/, name);
  }
});

test('the colours: public --vst- properties read once with a default, never set, and the docs name them all', () => {
  const css = fs.readFileSync(path.join(__dirname, '..', 'timeline.css'), 'utf8');
  // Each colour is read into its private name; the public name is never set, so that an ancestor can set it
  const read = [...css.matchAll(/^ {2}--_([\w-]+): var\(\s*--vst-([\w-]+),\s*light-dark\(/gm)];
  assert.ok(read.length >= 20, 'the colours are read');
  for (const [, own, pub] of read) assert.equal(own, pub);
  const names = read.map((m) => m[2]);
  assert.equal(
    [...css.matchAll(/--vst-[\w-]+/g)].length,
    names.length,
    'a --vst- name used elsewhere',
  );
  assert.doesNotMatch(css, /--vst-[\w-]+\s*:/, 'a --vst- property is set');
  // Every private colour used is one of them, and every one is used
  const used = new Set([...css.matchAll(/var\(--_([\w-]+)\)/g)].map((m) => m[1]));
  assert.deepEqual([...used].sort(), [...names].sort());
  // The tables of the docs name exactly these
  for (const docs of ['gui.md', path.join('ja', 'gui.md')]) {
    const text = fs.readFileSync(path.join(__dirname, '..', '..', '..', 'docs', docs), 'utf8');
    const rows = text.split('\n').filter((line) => line.startsWith('| `--vst-'));
    const listed = rows.flatMap((row) => [...row.matchAll(/`--vst-([\w-]+)`/g)].map((m) => m[1]));
    assert.deepEqual(listed.sort(), [...names].sort(), docs);
  }
});

// --- the words of the page (messages.js)

const M = SOURCES.VehicleSilTimelineMessages;
const PAGE = fs.readFileSync(path.join(__dirname, '..', 'timeline.html'), 'utf8');
const VIEWER = ['viewer.js', 'page.js']
  .map((name) => fs.readFileSync(path.join(__dirname, '..', name), 'utf8'))
  .join('\n');
/**
 * Keys that viewer.js makes from a prefix and a value of the file (t(`status.${...}`) and the like), and that
 * page.js makes from the keys of the element it says in its own words (pageLoad.).
 */
const MADE = ['status.', 'type.', 'word.', 'col.', 'pageLoad.'];

/** Arguments that answer every name with a marker, so that a text that forgets one shows "undefined". */
const ANY_ARGS = new Proxy({}, { get: (_, name) => (name === 'count' ? 2 : `<${String(name)}>`) });

test('the dictionary has the same keys in English and Japanese, each of the same kind', () => {
  assert.deepEqual(Object.keys(M).sort(), ['en', 'ja']);
  assert.deepEqual(Object.keys(M.ja).sort(), Object.keys(M.en).sort());
  for (const key of Object.keys(M.en)) {
    for (const lang of ['en', 'ja']) {
      const value = M[lang][key];
      assert.equal(typeof value, typeof M.en[key], `${lang} ${key}`);
      assert.equal(Array.isArray(value), Array.isArray(M.en[key]), `${lang} ${key}`);
      if (typeof value === 'function') {
        const text = value(ANY_ARGS);
        assert.equal(typeof text, 'string', `${lang} ${key}`);
        assert.doesNotMatch(text, /undefined|\[object/, `${lang} ${key}: ${text}`);
      } else if (Array.isArray(value)) {
        for (const part of value) {
          if (Array.isArray(part))
            assert.ok(['strong', 'em', 'code'].includes(part[0]), `${lang} ${key}`);
          else assert.equal(typeof part, 'string', `${lang} ${key}`);
        }
      } else {
        assert.equal(typeof value, 'string', `${lang} ${key}`);
      }
    }
    // A Japanese text that is the English one is a key not translated (a few words are the same in both).
    if (typeof M.en[key] === 'string' && /[a-z]{4,} [a-z]{2,}/i.test(M.en[key]))
      assert.notEqual(M.ja[key], M.en[key], `ja ${key} is the English text`);
  }
});

test('every key the page names is in the dictionary, and every key of the dictionary is used', () => {
  const named = new Set();
  for (const match of VIEWER.matchAll(/'([a-z][A-Za-z]*\.[A-Za-z_]+)'/g)) named.add(match[1]);
  for (const match of PAGE.matchAll(/data-(?:text|label)="([^"]+)"/g)) named.add(match[1]);
  const keys = new Set(Object.keys(M.en));
  const missing = [...named].filter((k) => !keys.has(k) && k.split('.')[0] !== 'node');
  assert.deepEqual(
    missing,
    [],
    'keys that viewer.js, page.js or timeline.html name and messages.js lacks',
  );
  for (const prefix of MADE) {
    assert.ok(
      VIEWER.includes(`\`${prefix}\${`),
      `viewer.js or page.js makes the keys ${prefix}...`,
    );
  }
  const unused = [...keys].filter((k) => !named.has(k) && !MADE.some((p) => k.startsWith(p)));
  assert.deepEqual(unused, [], 'keys of messages.js that nothing uses');
});

test('timeline.html has no text of its own: all of it comes from the dictionary', () => {
  const body = PAGE.slice(PAGE.indexOf('<body'))
    .replace(/<script[\s\S]*?<\/script>/g, '')
    .replace(/<noscript>[\s\S]*?<\/noscript>/g, '') // shown without JavaScript, so in both languages
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/<[^>]*>/g, '');
  assert.equal(body.trim(), '');
  assert.doesNotMatch(
    PAGE,
    /\s(?:title|aria-label|placeholder|alt)="/,
    'attributes with text: use data-label',
  );
});

test('the language is ?lang= when the page has it, else the browser one, else English', () => {
  assert.equal(T.pickLanguage('?lang=ja', ['en-US']), 'ja');
  assert.equal(T.pickLanguage('?src=x&lang=en', ['ja']), 'en');
  assert.equal(T.pickLanguage('?lang=fr', ['ja-JP', 'en']), 'ja');
  assert.equal(T.pickLanguage('', ['fr-FR', 'EN-gb']), 'en');
  assert.equal(T.pickLanguage('', ['fr']), 'en');
  assert.equal(T.pickLanguage(undefined, undefined), 'en');
});

test("a page's own texts come first, and a language it adds is one the element can be in", () => {
  const more = {
    en: { 'page.open': 'Load', 'page.titleOf': ({ name }) => `${name}!` },
    de: { 'page.open': 'Öffnen' },
  };
  assert.equal(T.text('en', 'page.open', {}, more), 'Load');
  assert.equal(T.text('en', 'page.titleOf', { name: 'a' }, more), 'a!');
  assert.equal(T.text('de', 'page.open', {}, more), 'Öffnen');
  // A key the added language lacks: the page's English, then the dictionary's English
  assert.equal(T.text('de', 'page.titleOf', { name: 'a' }, more), 'a!');
  assert.equal(T.text('de', 'page.whole', {}, more), 'Whole run');
  assert.equal(T.text('ja', 'page.open', {}, more), 'ファイルを開く');
  assert.equal(T.text('ja', 'no.such', {}, null), 'no.such');
  assert.equal(T.chooseLanguage(['de-AT', 'ja'], ['en', 'ja', 'de']), 'de');
  assert.equal(T.chooseLanguage(['pt-BR'], ['en', 'pt-BR']), 'pt-BR');
  assert.equal(T.chooseLanguage([null, '', 'fr'], ['en', 'ja']), 'en');
});

test('in Japanese, the words of a refused file, a failed write and the read-out are Japanese', () => {
  T.setLanguage('ja');
  try {
    assert.match(T.readTimeline('{', 'a.json').error, /^「a\.json」は JSON ではありません/);
    assert.match(T.readTimeline({}, null).error, /時間の軸のファイルではありません/);
    const at = { value: 3, state: 'not-applied', kept: { value: 1 } };
    assert.equal(
      T.readoutText('sent', at, { type: 'integer' }),
      '送った値 3（入らなかった。ECU の値は 1）',
    );
    const f = {
      at: 40,
      writes: [{ at: 40, applied: false, point: null, cause: true, pointDropped: false }],
    };
    const words = T.writeWords(f);
    assert.equal(words.heading, '40 ms の書き込み');
    assert.equal(words.cause(f.writes[0]), '送っていない: 失敗の原因');
    assert.equal(T.statusWord('passed'), '通った');
    assert.equal(T.statusWord('later'), 'later'); // a status the page does not know stays as the file writes it
    assert.equal(
      T.compareText([
        { op: 'ge', value: 1 },
        { op: 'le', value: 2 },
      ]),
      'ge 1 かつ le 2',
    );
  } finally {
    T.setLanguage('en');
  }
  assert.equal(T.writeWords({ at: 5, writes: [] }).heading, 'Writes for 5 ms');
});

test('the types (vehicle-sil-timeline.d.ts) name every option of the element, and nothing else', () => {
  const source = fs.readFileSync(path.join(__dirname, '..', 'viewer.js'), 'utf8');
  const block = (name) => {
    const start = source.indexOf(`const ${name} = {`);
    return source.slice(start, source.indexOf('\n  };', start));
  };
  const options = [...block('OPTIONS').matchAll(/^\s+'?[\w-]+'?: \['(\w+)'/gm)].map((m) => m[1]);
  const properties = [...block('PROPERTIES').matchAll(/^\s+(\w+): '/gm)].map((m) => m[1]);
  assert.ok(options.length >= 10 && properties.length >= 5, 'OPTIONS and PROPERTIES are read');
  const types = fs.readFileSync(path.join(__dirname, '..', 'vehicle-sil-timeline.d.ts'), 'utf8');
  const element = types.slice(
    types.indexOf('export interface VehicleSilTimelineElement'),
    types.indexOf('  addEventListener<'),
  );
  const typed = [...element.matchAll(/^ {2}(\w+)(?:\(\))?: /gm)].map((m) => m[1]);
  assert.deepEqual(typed.sort(), [...options, ...properties, 'reload'].sort());
  // The parts of hide: the type, the element's check and the CSS name the same words
  const union = /export type VehicleSilTimelinePart =([^;]+);/.exec(types)[1];
  const parts = [...union.matchAll(/'(\w+)'/g)].map((m) => m[1]);
  const css = fs.readFileSync(path.join(__dirname, '..', 'timeline.css'), 'utf8');
  assert.deepEqual(
    [...css.matchAll(/\[hide~='(\w+)'\]/g)].map((m) => m[1]).sort(),
    [...parts].sort(),
  );
  assert.match(source, new RegExp(`const PARTS = \\[${parts.map((p) => `'${p}'`).join(', ')}\\]`));
});

test('provenance helpers resolve scenario case, step file/name/call_stack, and line file in EN and JA', () => {
  const data = JSON.parse(JSON.stringify(reference('fails_beside')));
  data.scenario.case = '上限直前';
  data.steps[1].file = 'templates/init.yaml';
  data.steps[1].name = '通信確立';
  data.steps[1].call_stack = [
    {
      file: 'scenarios/load.yaml',
      line: 22,
      name: '初期化',
      params: { timeout_ms: 50, mode: 'auto' },
    },
  ];
  data.lines[1].file = 'templates/init.yaml';

  const m = model(data);
  assert.equal(m.caseName, '上限直前');
  assert.equal(m.failure.file, 'templates/init.yaml');
  assert.equal(m.failure.name, '通信確立');
  assert.equal(m.failure.callStack.length, 1);
  assert.equal(T.stepFile(m.steps[1], m.tl.scenario.file), 'templates/init.yaml');
  assert.equal(T.stepFile(m.steps[0], m.tl.scenario.file), m.tl.scenario.file);
  assert.equal(T.lineFile(m.lines[1], m.tl.scenario.file), 'templates/init.yaml');
  assert.equal(T.lineFile(m.lines[0], m.tl.scenario.file), m.tl.scenario.file);

  assert.match(
    T.stepTitle(m.steps[1], m.tl.scenario.file),
    new RegExp(`templates/init\\.yaml line ${m.steps[1].line}`),
  );
  assert.match(T.stepTitle(m.steps[0], m.tl.scenario.file), /^Step 1 \(set, line \d+\): /);
  assert.match(T.lineTitle(m.lines[1], m.tl.scenario.file), /templates\/init\.yaml line /);
  assert.doesNotMatch(T.lineTitle(m.lines[0], m.tl.scenario.file), /templates\/init\.yaml/);
  assert.deepEqual(T.callStackFrames(m.steps[1].call_stack, m.tl.scenario.file), [
    'line 22 of scenarios/load.yaml · 初期化 · timeout_ms = 50, mode = auto',
  ]);

  T.setLanguage('ja');
  try {
    assert.match(
      T.stepTitle(m.steps[1], m.tl.scenario.file),
      new RegExp(`templates/init\\.yaml ${m.steps[1].line} 行`),
    );
    assert.match(T.lineTitle(m.lines[1], m.tl.scenario.file), /templates\/init\.yaml .* 行/);
    assert.deepEqual(T.callStackFrames(m.steps[1].call_stack, m.tl.scenario.file), [
      'scenarios/load.yaml の 22 行 · 初期化 · timeout_ms = 50, mode = auto',
    ]);
  } finally {
    T.setLanguage('en');
  }
});

test('provenance helpers guard empty or absent case, file, name, and call_stack', () => {
  const data = JSON.parse(JSON.stringify(reference('fails_beside')));
  data.scenario.case = '';
  data.steps[0].file = '';
  data.steps[0].name = '';
  data.steps[0].call_stack = [];
  data.lines[0].file = '';

  const m = model(data);
  assert.equal(m.caseName, null);
  assert.equal(T.stepFile(m.steps[0], m.tl.scenario.file), m.tl.scenario.file);
  assert.equal(T.lineFile(m.lines[0], m.tl.scenario.file), m.tl.scenario.file);
  assert.deepEqual(T.callStackFrames(m.steps[0].call_stack, m.tl.scenario.file), []);
  assert.deepEqual(T.callStackFrames(undefined, m.tl.scenario.file), []);
});

test('callStackFrames formats nested multi-frame call stacks from outermost to immediate', () => {
  const stack = [
    {
      file: 'scenarios/load.yaml',
      line: 12,
      name: 'outer call',
      params: { target_ms: 500 },
    },
    {
      file: 'scenarios/templates/outer.yaml',
      line: 8,
      name: 'inner call',
      params: { mode: 'auto', flag: true },
    },
  ];
  assert.deepEqual(T.callStackFrames(stack, 'scenarios/load.yaml'), [
    'line 12 of scenarios/load.yaml · outer call · target_ms = 500',
    'line 8 of scenarios/templates/outer.yaml · inner call · mode = auto, flag = true',
  ]);
  T.setLanguage('ja');
  try {
    assert.deepEqual(T.callStackFrames(stack, 'scenarios/load.yaml'), [
      'scenarios/load.yaml の 12 行 · outer call · target_ms = 500',
      'scenarios/templates/outer.yaml の 8 行 · inner call · mode = auto, flag = true',
    ]);
  } finally {
    T.setLanguage('en');
  }
});

test('stepTitle, lineTitle, and callStackFrames handle line == null without "line null" or "line  of"', () => {
  const stepNoLine = {
    index: 1,
    file: 'templates/a.yaml',
    line: null,
    kind: 'set',
    status: 'failed',
  };
  const lineNoLine = { file: 'templates/a.yaml', line: null, step: 1, kind: 'set', text: 'x = 1' };
  const stackNoLine = [{ file: 'templates/a.yaml', line: null, name: 'Outer', params: { k: 1 } }];

  assert.equal(T.stepTitle(stepNoLine, 'main.yaml'), 'Step 1 (set, templates/a.yaml): failed');
  assert.equal(T.stepTitle({ ...stepNoLine, file: null }, 'main.yaml'), 'Step 1 (set): failed');
  assert.equal(T.lineTitle(lineNoLine, 'main.yaml'), 'x = 1 (step 1, templates/a.yaml)');
  assert.deepEqual(T.callStackFrames(stackNoLine, 'main.yaml'), [
    'templates/a.yaml · Outer · k = 1',
  ]);

  T.setLanguage('ja');
  try {
    assert.equal(T.stepTitle(stepNoLine, 'main.yaml'), '手順 1（set、templates/a.yaml）: 失敗');
    assert.equal(T.lineTitle(lineNoLine, 'main.yaml'), 'x = 1（手順 1、templates/a.yaml）');
    assert.deepEqual(T.callStackFrames(stackNoLine, 'main.yaml'), [
      'templates/a.yaml · Outer · k = 1',
    ]);
  } finally {
    T.setLanguage('en');
  }
});

const makeMockDom = () => {
  const byTag = (root, pred, out = []) => {
    for (const ch of root.children || []) {
      if (pred(ch)) out.push(ch);
      byTag(ch, pred, out);
    }
    return out;
  };
  class MockNode {
    constructor(tag, ns = null) {
      this.tagName = tag;
      this.namespaceURI = ns;
      this.children = [];
      this.attrs = {};
      this.dataset = {};
      this._text = '';
      this.hidden = false;
      this.className = '';
      this.clientWidth = 800;
      this.parentElement = null;
      this.style = { setProperty() {} };
    }
    get id() {
      return this.attrs.id || '';
    }
    set id(v) {
      this.attrs.id = v;
    }
    get textContent() {
      return this._text + this.children.map((c) => c.textContent).join('');
    }
    set textContent(v) {
      this.children = [];
      this._text = String(v);
    }
    set innerHTML(_v) {
      throw new Error('innerHTML must never be used');
    }
    setAttribute(k, v) {
      this.attrs[k] = String(v);
      if (k.startsWith('data-')) this.dataset[k.slice(5)] = String(v);
    }
    getAttribute(k) {
      return Object.prototype.hasOwnProperty.call(this.attrs, k) ? this.attrs[k] : null;
    }
    removeAttribute(k) {
      delete this.attrs[k];
    }
    append(...nodes) {
      for (const n of nodes) {
        if (typeof n === 'string') {
          const tnode = new MockNode('#text');
          tnode._text = n;
          this.children.push(tnode);
        } else if (n) {
          n.parentElement = this;
          this.children.push(n);
        }
      }
    }
    replaceChildren(...nodes) {
      this.children = [];
      this._text = '';
      this.append(...nodes);
    }
    addEventListener() {}
    getElementById(id) {
      return byTag(this, (n) => n.attrs.id === id)[0] || null;
    }
    querySelectorAll(sel) {
      if (sel.startsWith('[') && sel.endsWith(']')) {
        const inner = sel.slice(1, -1);
        const eq = inner.indexOf('=');
        if (eq >= 0) {
          const attr = inner.slice(0, eq);
          const val = inner.slice(eq + 1).replace(/^['"]|['"]$/g, '');
          return byTag(this, (n) => n.attrs[attr] === val);
        }
        return byTag(this, (n) => Object.prototype.hasOwnProperty.call(n.attrs, inner));
      }
      return byTag(this, (n) => n.tagName === sel);
    }
  }
  class MockHTMLElement extends MockNode {
    constructor() {
      super('vehicle-sil-timeline');
      this.isConnected = true;
      this._shadow = new MockNode('#shadow');
    }
    attachShadow() {
      return this._shadow;
    }
    dispatchEvent() {
      return true;
    }
  }
  const registry = new Map();
  const document = {
    createElement: (tag) => new MockNode(tag),
    createElementNS: (ns, tag) => new MockNode(tag, ns),
    createTextNode: (t) => {
      const n = new MockNode('#text');
      n._text = String(t);
      return n;
    },
  };
  const global = {
    HTMLElement: MockHTMLElement,
    document,
    navigator: { languages: ['en'], language: 'en' },
    customElements: {
      get: (name) => registry.get(name),
      define: (name, cls) => registry.set(name, cls),
    },
    CustomEvent: class {
      constructor(type, init) {
        this.type = type;
        this.detail = init?.detail;
      }
    },
    ResizeObserver: class {
      observe() {}
      disconnect() {}
    },
  };
  const code = ['messages.js', 'viewer.js']
    .map((name) => fs.readFileSync(path.join(__dirname, '..', name), 'utf8'))
    .join('\n');
  new Function(
    'globalThis',
    'HTMLElement',
    'document',
    'navigator',
    'customElements',
    'CustomEvent',
    'ResizeObserver',
    code,
  )(
    global,
    global.HTMLElement,
    global.document,
    global.navigator,
    global.customElements,
    global.CustomEvent,
    global.ResizeObserver,
  );
  return { Element: global.VehicleSilTimeline.Element };
};

test('DOM rendering handles non-string step.file, line == null, lines[].file, and escapes XSS payloads', async () => {
  const { Element } = makeMockDom();
  const el = new Element();
  const tl = JSON.parse(JSON.stringify(reference('fails_beside')));
  tl.scenario.case = '<script>window.__xss=1</script>';
  tl.steps[0].file = 123;
  tl.steps[0].line = null;
  tl.steps[0].name = '<script>window.__xss=2</script>Step1';
  tl.steps[0].status = 'failed';
  tl.steps[0].code = 'scenario-failed';
  tl.steps[0].call_stack = [
    {
      file: 'templates/<img src=x onerror="xss()">.yaml',
      line: null,
      name: 'Outer',
      params: { p: '<svg onload="xss()">' },
    },
  ];
  tl.steps[1].file = 'templates/sub.yaml';
  tl.steps[1].line = null;
  tl.steps[1].status = 'skipped';
  tl.lines[0].file = 'templates/sub.yaml';

  el.data = tl;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));

  const shadow = el._shadow;
  const summaryText = shadow.getElementById('summary').textContent;
  assert.ok(summaryText.includes(tl.scenario.file), summaryText);
  assert.ok(!summaryText.includes('line null'), summaryText);
  assert.ok(!summaryText.includes('of 123'), summaryText);
  assert.ok(!summaryText.includes('line  of'), summaryText);
  assert.ok(summaryText.includes('<script>window.__xss=2</script>Step1'));

  const stepsTable = shadow.querySelectorAll('[data-testid=steps-table]')[0];
  const stepText = stepsTable.textContent;
  assert.ok(stepText.includes('templates/sub.yaml'));
  assert.ok(!stepText.includes('templates/sub.yaml:'));
  assert.ok(!stepText.includes(':null'));

  const titles = shadow.querySelectorAll('title').map((n) => n.textContent);
  assert.ok(titles.some((txt) => txt.includes('templates/sub.yaml')));
  assert.ok(!titles.some((txt) => txt.includes('line null')));
});

test('element DOM escapes XSS in scenario.case facts, multiline step.name, and multi-frame call_stack with empty params', async () => {
  const { Element } = makeMockDom();
  const el = new Element();
  const tl = JSON.parse(JSON.stringify(reference('fails_beside')));
  tl.scenario.case = '<b>case&xss</b><script>alert(1)</script>';
  tl.steps[0].name = 'Line1\n<img src=x onerror="alert(2)">';
  tl.steps[0].file = 'templates/inner.yaml';
  tl.steps[0].line = 7;
  tl.steps[0].status = 'failed';
  tl.steps[0].code = 'scenario-failed';
  tl.steps[0].call_stack = [
    { file: 'scenarios/main.yaml', line: 4, name: 'Outer <script>x</script>', params: {} },
    { file: 'templates/mid.yaml', line: 9, name: null, params: { key: '<svg onload=1>' } },
  ];

  el.data = tl;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));

  const shadow = el._shadow;
  // No raw <script> or <img> elements were ever constructed in the shadow DOM (and innerHTML throws).
  assert.equal(shadow.querySelectorAll('script').length, 0);
  assert.equal(shadow.querySelectorAll('img').length, 0);
  // #facts renders the escaped case text verbatim.
  const factsText = shadow.getElementById('facts').textContent;
  assert.ok(factsText.includes('<b>case&xss</b><script>alert(1)</script>'), factsText);
  // Step name and multi-frame call_stack (including empty params: {}) render as safe text.
  const stepsText = shadow.querySelectorAll('[data-testid=steps-table]')[0].textContent;
  assert.ok(stepsText.includes('Line1\n<img src=x onerror="alert(2)">'), stepsText);
  assert.ok(
    stepsText.includes('line 4 of scenarios/main.yaml · Outer <script>x</script>'),
    stepsText,
  );
  assert.ok(stepsText.includes('line 9 of templates/mid.yaml · key = <svg onload=1>'), stepsText);
  assert.ok(!stepsText.includes('()'), stepsText);
});
