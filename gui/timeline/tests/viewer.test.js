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
    const sc = read.timeline.scenario;
    assert.equal(Boolean(m.failure), Boolean(sc && sc.status === 'failed'), name);
    const recTo = read.timeline.recorded_to_ms ?? sc?.recorded_to_ms ?? 0;
    assert.ok(m.domain[1] >= recTo, name);
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
    const to = tl.recorded_to_ms ?? tl.scenario?.recorded_to_ms ?? null;
    // No point after the end of the record; a failed run's record ends at the failed step's time.
    for (const s of tl.series)
      for (const p of s.points)
        assert.ok(to !== null && p[0] <= to, `${name}: ${s.variable} ${p[0]}`);
    const failed = tl.steps.find((st) => st.status === 'failed');
    if (tl.scenario?.status === 'failed') assert.equal(to, failed.at_ms, `${name}: recorded_to_ms`);
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
    [{ ...good, scenario: undefined }, /no "scenario" object/],
    [{ ...good, scenario: 'bad' }, /no "scenario" object/],
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
  assert.deepEqual(
    typed.sort(),
    [...options, ...properties, 'reload', 'resetView', 'zoomOut', 'unpinCursor'].sort(),
  );
  // Additive methods and events are declared on VehicleSilTimelineElement / VehicleSilTimelineEventMap
  assert.match(types, /^ {2}jumpToFailure\(options\?: VehicleSilTimelineFocusOptions\): boolean;/m);
  assert.match(
    types,
    /^ {2}focusStep\(index: number, options\?: VehicleSilTimelineFocusOptions\): boolean;/m,
  );
  assert.match(types, /^ {2}pan\(fraction: number\): void;/m);
  assert.match(types, /^ {2}pinCursor\(atMs: number\): boolean;/m);
  assert.match(types, /^ {2}variablehide: CustomEvent<\{ variable: string \}>;/m);
  // The parts of hide: the type, the element's check and the CSS name the same words
  const union = /export type VehicleSilTimelinePart =([^;]+);/.exec(types)[1];
  const parts = [...union.matchAll(/'(\w+)'/g)].map((m) => m[1]);
  const css = fs.readFileSync(path.join(__dirname, '..', 'timeline.css'), 'utf8');
  assert.deepEqual(
    [...css.matchAll(/\[hide~='(\w+)'\]/g)].map((m) => m[1]).sort(),
    [...parts].sort(),
  );
  const partsDecl = /const PARTS = \[([\s\S]*?)\];/.exec(source);
  assert.ok(partsDecl, 'viewer.js declares PARTS');
  assert.deepEqual(
    [...partsDecl[1].matchAll(/'(\w+)'/g)].map((m) => m[1]),
    parts,
  );
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
      this.classList = {
        toggle: (cls, force) => {
          const set = new Set((this.className || '').split(/\s+/).filter(Boolean));
          if (force === undefined) {
            if (set.has(cls)) set.delete(cls);
            else set.add(cls);
          } else if (force) set.add(cls);
          else set.delete(cls);
          this.className = [...set].join(' ');
        },
      };
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
    addEventListener(type, fn) {
      if (!this._listeners) this._listeners = new Map();
      let list = this._listeners.get(type);
      if (!list) {
        list = [];
        this._listeners.set(type, list);
      }
      list.push(fn);
    }
    dispatchEvent(ev) {
      const list = this._listeners?.get(ev.type) ?? [];
      for (const fn of list) fn.call(this, ev);
      return true;
    }
    getElementById(id) {
      return byTag(this, (n) => n.attrs.id === id)[0] || null;
    }
    querySelectorAll(sel) {
      if (sel.startsWith('.')) {
        const cls = sel.slice(1);
        return byTag(this, (n) => {
          const c = n.className || n.attrs.class || '';
          return c.split(/\s+/).includes(cls);
        });
      }
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
    dispatchEvent(ev) {
      const list = this._listeners?.get(ev.type) ?? [];
      for (const fn of list) fn.call(this, ev);
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

// --- variable-to-variable dynamic watch bands, compared failures, and 2^53 integers

/** Builds a minimal valid format-1 timeline object for dynamic-band tests. */
function makeDynamicTimeline(overrides) {
  return {
    vehicle_sil_timeline: 1,
    vehicle_sil: '0.6.0',
    stage: 'run',
    file: 'simulation.yaml',
    variant: null,
    params: [],
    time_step_ms: 10,
    scenario: {
      file: 'scenarios/tracking.yaml',
      name: 'dynamic tracking',
      status: 'passed',
      over_at_ms: 100,
      recorded_to_ms: 100,
      end_ms: null,
    },
    variables: [
      {
        name: 'Engine.speed',
        participant: 'Engine',
        type: 'real',
        causality: 'output',
        unit: 'rpm',
        min: 0,
        max: 6000,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'Dashboard.speed',
        participant: 'Dashboard',
        type: 'real',
        causality: 'output',
        unit: 'rpm',
        min: 0,
        max: 6000,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
    ],
    steps: [],
    series: [],
    lines: [],
    findings: [],
    limits: { points_per_series: 10000, points_total: 100000, dropped: [] },
    ...overrides,
  };
}

function makeSeries(variable, source, points, extra) {
  return {
    variable,
    source,
    encoding: source === 'sampled' ? 'changes' : 'all',
    samples: source === 'sampled' ? points.length : null,
    first_ms: source === 'sampled' && points.length ? points[0][0] : null,
    last_ms: source === 'sampled' && points.length ? points[points.length - 1][0] : null,
    not_applied_from: null,
    columns: ['at_ms', 'value'],
    points,
    ...extra,
  };
}

function findNodes(root, pred, out = []) {
  for (const ch of root.children || []) {
    if (pred(ch)) out.push(ch);
    findNodes(ch, pred, out);
  }
  return out;
}

test('dynamic band resolves ZOH slices from rhsVar.sampled only (never written) and skips unobserved intervals', () => {
  const tl = makeDynamicTimeline({
    variables: [
      {
        name: 'Engine.speed',
        participant: 'Engine',
        type: 'real',
        causality: 'input',
        unit: 'rpm',
        min: 0,
        max: 6000,
        items: [],
        ecu_step_ms: 10,
        written: true,
        watched: true,
      },
      {
        name: 'Dashboard.speed',
        participant: 'Dashboard',
        type: 'real',
        causality: 'output',
        unit: 'rpm',
        min: 0,
        max: 6000,
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
        kind: 'expect',
        text: 'expect Dashboard.speed near Engine.speed (tolerance 10)',
        status: 'passed',
        start_ms: 0,
        at_ms: 100,
        end_ms: 100,
        code: null,
        message: null,
        lines: [],
        watch: {
          mode: 'always',
          from_ms: 0,
          to_ms: 100,
          conditions: [
            {
              variable: 'Dashboard.speed',
              compare: [{ op: 'near', value: null, variable: 'Engine.speed', tolerance: 10 }],
            },
          ],
        },
        failed: null,
      },
    ],
    series: [
      // Written series has completely different values (9999) and starts at 0 ms: MUST be ignored!
      makeSeries('Engine.speed', 'written', [
        [0, 9999],
        [50, 8888],
      ]),
      // Sampled series starts at 20 ms (unobserved during [0, 20)), then steps at 60 ms, then NaN at 80 ms.
      makeSeries(
        'Engine.speed',
        'sampled',
        [
          [20, 1200],
          [60, 1500],
          [80, null],
          [90, 1600],
        ],
        { last_ms: 100 },
      ),
      makeSeries(
        'Dashboard.speed',
        'sampled',
        [
          [20, 1205],
          [60, 1495],
          [90, 1600],
        ],
        { last_ms: 100 },
      ),
    ],
  });

  const m = model(tl);
  const dash = m.byName.get('Dashboard.speed');
  assert.equal(dash.bands.length, 1);
  const band = dash.bands[0];
  assert.equal(band.dynamic, true);
  // [0, 20) unobserved, [80, 90) non-finite null -> both skipped; written values (9999, 8888) ignored.
  assert.deepEqual(
    band.slices.map((s) => [s.from, s.to, s.lo, s.hi]),
    [
      [20, 60, 1190, 1210],
      [60, 80, 1490, 1510],
      [90, 100, 1590, 1610],
    ],
  );
  assert.equal(T.compareText(band.compare, dash.info), 'near Engine.speed ± 10');

  // If rhsVar has ONLY a written series (no sampled series), NO dynamic slices are produced.
  const writtenOnly = JSON.parse(JSON.stringify(tl));
  writtenOnly.series = writtenOnly.series.filter(
    (s) => !(s.variable === 'Engine.speed' && s.source === 'sampled'),
  );
  const mWrittenOnly = model(writtenOnly);
  assert.deepEqual(mWrittenOnly.byName.get('Dashboard.speed').bands[0].slices, []);
});

test('dynamic band splits and skips rhsVar.gaps.sampled intervals without painting missing ranges', () => {
  const tl = makeDynamicTimeline({
    steps: [
      {
        index: 1,
        line: 4,
        kind: 'expect',
        text: 'expect Dashboard.speed near Engine.speed (tolerance 15)',
        status: 'passed',
        start_ms: 0,
        at_ms: 100,
        end_ms: 100,
        code: null,
        message: null,
        lines: [],
        watch: {
          mode: 'always',
          from_ms: 0,
          to_ms: 100,
          conditions: [
            {
              variable: 'Dashboard.speed',
              compare: [{ op: 'near', value: null, variable: 'Engine.speed', tolerance: 15 }],
            },
          ],
        },
        failed: null,
      },
    ],
    series: [
      makeSeries(
        'Engine.speed',
        'sampled',
        [
          [0, 1000],
          [30, 1100], // inside gap [20, 60): must not be held as a valid band
          [60, 1400],
          [80, 1500],
        ],
        { last_ms: 100 },
      ),
      makeSeries('Dashboard.speed', 'sampled', [[0, 1000]], { last_ms: 100 }),
    ],
    limits: {
      points_per_series: 10,
      points_total: 100,
      dropped: [
        {
          variable: 'Engine.speed',
          source: 'sampled',
          dropped: 4,
          dropped_from_ms: 20,
          dropped_before_ms: 60,
        },
      ],
    },
  });

  const m = model(tl);
  const band = m.byName.get('Dashboard.speed').bands[0];
  // [0, 20] held from 0 ms (cut at gap.from = 20), [20, 60) skipped, [60, 80] and [80, 100] resumed.
  assert.deepEqual(
    band.slices.map((s) => [s.from, s.to, s.lo, s.hi]),
    [
      [0, 20, 985, 1015],
      [60, 80, 1385, 1415],
      [80, 100, 1485, 1515],
    ],
  );
});

test('during mode uses only rhs samples with sample_ms >= from_ms, while always carries earlier samples', () => {
  const makeCase = (mode) =>
    makeDynamicTimeline({
      steps: [
        {
          index: 1,
          line: 4,
          kind: mode === 'during' ? 'ramp' : 'expect',
          text: 'check speed',
          status: 'passed',
          start_ms: 50,
          at_ms: 100,
          end_ms: 100,
          code: null,
          message: null,
          lines: [],
          watch: {
            mode,
            from_ms: 50,
            to_ms: 100,
            conditions: [
              {
                variable: 'Dashboard.speed',
                compare: [{ op: 'ge', value: null, variable: 'Engine.speed', tolerance: null }],
              },
            ],
          },
          failed: null,
        },
      ],
      series: [
        makeSeries(
          'Engine.speed',
          'sampled',
          [
            [0, 500],
            [40, 700],
            [70, 900],
          ],
          { last_ms: 100 },
        ),
        makeSeries('Dashboard.speed', 'sampled', [[50, 950]], { last_ms: 100 }),
      ],
    });

  // In "during" [50, 100]: samples at 0 ms and 40 ms are < 50 ms so [50, 70) has no valid slice!
  const mDuring = model(makeCase('during'));
  assert.deepEqual(
    mDuring.byName.get('Dashboard.speed').bands[0].slices.map((s) => [s.from, s.to, s.lo, s.hi]),
    [[70, 100, 900, Infinity]],
  );

  // Sample right at sample_ms === from_ms (50 ms) IS included in "during" mode!
  const atBoundary = makeCase('during');
  atBoundary.series[0].points = [
    [40, 700],
    [50, 800],
    [80, 900],
  ];
  assert.deepEqual(
    model(atBoundary)
      .byName.get('Dashboard.speed')
      .bands[0].slices.map((s) => [s.from, s.to, s.lo, s.hi]),
    [
      [50, 80, 800, Infinity],
      [80, 100, 900, Infinity],
    ],
  );

  // In "always" [50, 100]: sample at 40 ms carries forward across [50, 70)!
  const mAlways = model(makeCase('always'));
  assert.deepEqual(
    mAlways.byName.get('Dashboard.speed').bands[0].slices.map((s) => [s.from, s.to, s.lo, s.hi]),
    [
      [50, 70, 700, Infinity],
      [70, 100, 900, Infinity],
    ],
  );
});

test('multi-comparison intersection combines constants and multiple variables per slice and omits empty intervals', () => {
  const tl = makeDynamicTimeline({
    variables: [
      {
        name: 'A.val',
        participant: 'A',
        type: 'integer',
        causality: 'output',
        unit: '',
        min: 0,
        max: 500,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'B.val',
        participant: 'B',
        type: 'integer',
        causality: 'output',
        unit: '',
        min: 0,
        max: 500,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'Target.val',
        participant: 'Target',
        type: 'integer',
        causality: 'output',
        unit: '',
        min: 0,
        max: 500,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
    ],
    steps: [
      {
        index: 1,
        line: 3,
        kind: 'expect',
        text: 'expect Target.val ge A.val and le B.val and le 200',
        status: 'passed',
        start_ms: 0,
        at_ms: 100,
        end_ms: 100,
        code: null,
        message: null,
        lines: [],
        watch: {
          mode: 'always',
          from_ms: 0,
          to_ms: 100,
          conditions: [
            {
              variable: 'Target.val',
              compare: [
                { op: 'ge', value: null, variable: 'A.val', tolerance: null },
                { op: 'le', value: null, variable: 'B.val', tolerance: null },
                { op: 'le', value: 200, tolerance: null },
              ],
            },
          ],
        },
        failed: null,
      },
    ],
    series: [
      // [0, 30): A=100, B=250 -> [100, min(250, 200)] = [100, 200]
      // [30, 50): A=220, B=250 -> [220, 200] EMPTY -> omitted!
      // [50, 70): A=120, B=110 -> [120, 110] EMPTY -> omitted!
      // [70, 100]: A=120, B=180 -> [120, 180]
      makeSeries(
        'A.val',
        'sampled',
        [
          [0, 100],
          [30, 220],
          [50, 120],
        ],
        { last_ms: 100 },
      ),
      makeSeries(
        'B.val',
        'sampled',
        [
          [0, 250],
          [50, 110],
          [70, 180],
        ],
        { last_ms: 100 },
      ),
      makeSeries('Target.val', 'sampled', [[0, 150]], { last_ms: 100 }),
    ],
  });

  const m = model(tl);
  const band = m.byName.get('Target.val').bands[0];
  assert.deepEqual(
    band.slices.map((s) => [s.from, s.to, s.lo, s.hi]),
    [
      [0, 30, 100, 200],
      [70, 100, 120, 180],
    ],
  );

  // Integer strict emptiness: gt 5 and lt 6 has no integer solution -> omitted.
  const strictTl = JSON.parse(JSON.stringify(tl));
  strictTl.steps[0].watch.conditions[0].compare = [
    { op: 'gt', value: 5, tolerance: null },
    { op: 'lt', value: 6, tolerance: null },
  ];
  assert.deepEqual(model(strictTl).byName.get('Target.val').bands[0].slices, []);
});

test('laneRange keeps 2-argument compatibility, ignores null ± tolerance coercion, and fits dynamic slices', () => {
  assert.equal(T.laneRange.length, 2);
  const tl = makeDynamicTimeline({
    variables: [
      {
        name: 'Engine.speed',
        participant: 'Engine',
        type: 'real',
        causality: 'output',
        unit: 'rpm',
        min: 0,
        max: 6000,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'Dashboard.speed',
        participant: 'Dashboard',
        type: 'real',
        causality: 'output',
        unit: 'rpm',
        min: 0,
        max: 6000,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
    ],
    steps: [
      {
        index: 1,
        line: 3,
        kind: 'expect',
        text: 'expect Dashboard.speed near Engine.speed (tolerance 20)',
        status: 'passed',
        start_ms: 0,
        at_ms: 40,
        end_ms: 40,
        code: null,
        message: null,
        lines: [],
        watch: {
          mode: 'always',
          from_ms: 0,
          to_ms: 40,
          conditions: [
            {
              variable: 'Dashboard.speed',
              compare: [{ op: 'near', value: null, variable: 'Engine.speed', tolerance: 20 }],
            },
          ],
        },
        failed: null,
      },
    ],
    series: [
      makeSeries(
        'Engine.speed',
        'sampled',
        [
          [0, 1500],
          [20, 1800],
        ],
        { last_ms: 40 },
      ),
      makeSeries('Dashboard.speed', 'sampled', [[0, 1510]], { last_ms: 40 }),
    ],
  });

  const m = model(tl);
  const dash = m.byName.get('Dashboard.speed');
  assert.deepEqual(T.laneRange(dash, false), [0, 6000]);
  const [lo, hi] = T.laneRange(dash, true);
  // Must include dynamic band bounds [1480, 1820] and MUST NOT coerce null ± 20 into [-20, 20]!
  assert.ok(lo > 1400 && lo <= 1480, `unexpected lo=${lo}`);
  assert.ok(hi >= 1820 && hi < 1900, `unexpected hi=${hi}`);
});

test('once timeout dashed line resolves dynamic limitValue when compare[0].value is null', async () => {
  const tl = makeDynamicTimeline({
    steps: [
      {
        index: 1,
        line: 3,
        kind: 'wait_until',
        text: 'wait_until Dashboard.speed near Engine.speed (tolerance 10)',
        status: 'passed',
        start_ms: 0,
        at_ms: 30,
        end_ms: 30,
        code: null,
        message: null,
        lines: [],
        watch: {
          mode: 'once',
          from_ms: 0,
          to_ms: 200,
          conditions: [
            {
              variable: 'Dashboard.speed',
              compare: [{ op: 'near', value: null, variable: 'Engine.speed', tolerance: 10 }],
            },
          ],
        },
        failed: null,
      },
    ],
    series: [
      makeSeries(
        'Engine.speed',
        'sampled',
        [
          [0, 1000],
          [20, 1400],
        ],
        { last_ms: 30 },
      ),
      makeSeries(
        'Dashboard.speed',
        'sampled',
        [
          [0, 500],
          [30, 1400],
        ],
        { last_ms: 30 },
      ),
    ],
  });
  tl.scenario.recorded_to_ms = 30;
  tl.scenario.over_at_ms = 30;

  const m = model(tl);
  const band = m.byName.get('Dashboard.speed').bands[0];
  assert.equal(band.mode, 'once');
  assert.equal(band.to, 30);
  assert.equal(band.limit, 200);
  assert.equal(band.limitValue, 1400);

  // Verify DOM rendering places .threshold.limit at the resolved y (not at area.y0 = 4).
  const { Element } = makeMockDom();
  const el = new Element();
  el.data = tl;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  const limitLines = findNodes(
    el._shadow,
    (n) => n.tagName === 'line' && n.attrs.class === 'threshold limit',
  );
  assert.equal(limitLines.length, 1);
  const y1 = Number(limitLines[0].attrs.y1);
  assert.ok(y1 > 4, `limit line y1=${y1} should not fall back to top edge area.y0=4`);
});

test('discrete bool and enum lanes resolve and render dynamic watch bands with intersection', async () => {
  const gearItems = [
    { name: 'P', value: 0 },
    { name: 'R', value: 1 },
    { name: 'D', value: 2 },
  ];
  const tl = makeDynamicTimeline({
    variables: [
      {
        name: 'Trans.gear',
        participant: 'Trans',
        type: 'enum',
        causality: 'output',
        unit: '',
        min: null,
        max: null,
        items: gearItems,
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'Dash.gear',
        participant: 'Dash',
        type: 'enum',
        causality: 'output',
        unit: '',
        min: null,
        max: null,
        items: gearItems,
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'Body.lamp',
        participant: 'Body',
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
      {
        name: 'Dash.lamp',
        participant: 'Dash',
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
        line: 3,
        kind: 'expect',
        text: 'expect Dash.gear eq Trans.gear and ne P, Dash.lamp eq Body.lamp',
        status: 'passed',
        start_ms: 0,
        at_ms: 100,
        end_ms: 100,
        code: null,
        message: null,
        lines: [],
        watch: {
          mode: 'always',
          from_ms: 0,
          to_ms: 100,
          conditions: [
            {
              variable: 'Dash.gear',
              compare: [
                { op: 'eq', value: null, variable: 'Trans.gear', tolerance: null },
                { op: 'ne', value: 0, tolerance: null },
              ],
            },
            {
              variable: 'Dash.lamp',
              compare: [{ op: 'eq', value: null, variable: 'Body.lamp', tolerance: null }],
            },
          ],
        },
        failed: null,
      },
    ],
    series: [
      // [0, 30): Trans.gear = 0 (P) -> contradicts ne 0 -> EMPTY slice!
      // [30, 100]: Trans.gear = 2 (D) -> valid discrete slice value = 2!
      makeSeries(
        'Trans.gear',
        'sampled',
        [
          [0, 0],
          [30, 2],
        ],
        { last_ms: 100 },
      ),
      makeSeries(
        'Dash.gear',
        'sampled',
        [
          [0, 2],
          [30, 2],
        ],
        { last_ms: 100 },
      ),
      makeSeries(
        'Body.lamp',
        'sampled',
        [
          [0, false],
          [50, true],
        ],
        { last_ms: 100 },
      ),
      makeSeries(
        'Dash.lamp',
        'sampled',
        [
          [0, false],
          [50, true],
        ],
        { last_ms: 100 },
      ),
    ],
  });

  const m = model(tl);
  const gearBand = m.byName.get('Dash.gear').bands[0];
  assert.equal(gearBand.dynamic, true);
  assert.deepEqual(
    gearBand.slices.map((s) => [s.from, s.to, s.value, s.excluded]),
    [[30, 100, 2, [0]]],
  );
  const lampBand = m.byName.get('Dash.lamp').bands[0];
  assert.equal(lampBand.dynamic, true);
  assert.deepEqual(
    lampBand.slices.map((s) => [s.from, s.to, s.value]),
    [
      [0, 50, false],
      [50, 100, true],
    ],
  );

  const { Element } = makeMockDom();
  const el = new Element();
  el.data = tl;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  const dynBoxes = findNodes(
    el._shadow,
    (n) => n.tagName === 'rect' && n.attrs.class === 'expect-box dynamic',
  );
  // 1 slice on Dash.gear + 2 slices on Dash.lamp = 3 dynamic discrete boxes.
  assert.equal(dynBoxes.length, 3);
  const titles = findNodes(el._shadow, (n) => n.tagName === 'title').map((n) => n.textContent);
  assert.ok(titles.some((txt) => txt.includes('eq D (2) and ne P (0) (30 ms to 100 ms)')));
  assert.ok(titles.some((txt) => txt.includes('eq true (50 ms to 100 ms)')));
});

test('failed variable comparison renders both-side values, sample times, both-lane markers, null compared, and escapes XSS', async () => {
  const tl = makeDynamicTimeline({
    scenario: {
      file: 'scenarios/tracking.yaml',
      name: 'speed mismatch',
      status: 'failed',
      over_at_ms: 40,
      recorded_to_ms: 40,
      end_ms: null,
    },
    steps: [
      {
        index: 1,
        line: 8,
        kind: 'expect',
        text: 'expect Dashboard.speed near Engine.speed (tolerance 10)',
        status: 'failed',
        start_ms: 0,
        at_ms: 40,
        end_ms: null,
        code: 'scenario-failed',
        message:
          'at 40 ms Dashboard.speed near Engine.speed (tolerance 10), but it was 1650 (its sample of 40 ms) while Engine.speed was 1800 (its sample of 30 ms)',
        lines: [],
        watch: {
          mode: 'always',
          from_ms: 0,
          to_ms: 100,
          conditions: [
            {
              variable: 'Dashboard.speed',
              compare: [{ op: 'near', value: null, variable: 'Engine.speed', tolerance: 10 }],
            },
          ],
        },
        failed: {
          at_ms: 40,
          code: 'scenario-failed',
          conditions: [
            {
              condition: 0,
              value: 1650,
              sample_ms: 40,
              compared: [{ variable: 'Engine.speed', value: 1800, sample_ms: 30 }],
            },
          ],
          writes: [],
          write: null,
        },
      },
    ],
    series: [
      makeSeries(
        'Engine.speed',
        'sampled',
        [
          [0, 1650],
          [30, 1800],
        ],
        { last_ms: 30 },
      ),
      makeSeries('Dashboard.speed', 'sampled', [[0, 1650]], { last_ms: 40 }),
    ],
  });

  const m = model(tl);
  assert.equal(m.byName.get('Dashboard.speed').failing, true);
  assert.equal(m.byName.get('Engine.speed').failing, true);
  assert.deepEqual(m.byName.get('Dashboard.speed').marks, [
    { kind: 'condition', at: 40, value: 1650, text: '1650' },
  ]);
  assert.deepEqual(m.byName.get('Engine.speed').marks, [
    { kind: 'compared', at: 30, value: 1800, text: '1800', against: 'Dashboard.speed' },
  ]);
  assert.deepEqual(m.failure.conditions[0].compared, [
    { variable: 'Engine.speed', value: 1800, valueText: '1800', sampleMs: 30 },
  ]);

  const { Element } = makeMockDom();
  const el = new Element();
  el.data = tl;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  const condText = el._shadow.querySelectorAll('[data-testid=failed-conditions]')[0].textContent;
  assert.equal(
    condText,
    'Dashboard.speed near Engine.speed ± 10: it was 1650 (sample of 40 ms); while Engine.speed was 1800 (sample of 30 ms)',
  );
  const lhsMarks = findNodes(el._shadow, (n) => n.tagName === 'g' && n.attrs.class === 'mark bad');
  const rhsMarks = findNodes(
    el._shadow,
    (n) => n.tagName === 'g' && n.attrs.class === 'mark bad compared',
  );
  assert.equal(lhsMarks.length, 1);
  assert.equal(rhsMarks.length, 1);
  assert.match(
    rhsMarks[0].textContent,
    /Compared value was 1800 \(sample of 30 ms\) for Dashboard\.speed/,
  );

  // Japanese rendering
  el.setAttribute('lang', 'ja');
  el.attributeChangedCallback('lang', null, 'ja');
  await new Promise((r) => Promise.resolve().then(r));
  const condTextJa = el._shadow.querySelectorAll('[data-testid=failed-conditions]')[0].textContent;
  assert.equal(
    condTextJa,
    'Dashboard.speed near Engine.speed ± 10: 値は 1650（40 ms のサンプル）。比較対象の Engine.speed は 1800（30 ms のサンプル）',
  );

  // Null compared (during unseen RHS) + XSS variable name
  const nullXss = JSON.parse(JSON.stringify(tl));
  const xssVar = 'Rhs.<img src=x onerror="xss()">';
  nullXss.variables[0].name = xssVar;
  nullXss.series[0].variable = xssVar;
  nullXss.steps[0].watch.mode = 'during';
  nullXss.steps[0].watch.conditions[0].compare[0].variable = xssVar;
  nullXss.steps[0].failed.conditions[0].compared = [
    { variable: xssVar, value: null, sample_ms: null },
  ];
  const el2 = new Element();
  el2.data = nullXss;
  el2.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(el2._shadow.querySelectorAll('img').length, 0);
  const condNull = el2._shadow.querySelectorAll('[data-testid=failed-conditions]')[0].textContent;
  assert.ok(
    condNull.includes(`while ${xssVar} had no sample in the interval`),
    `unexpected text: ${condNull}`,
  );
  const rhsNullMark = findNodes(
    el2._shadow,
    (n) => n.tagName === 'g' && n.attrs.class === 'mark bad compared',
  );
  assert.equal(rhsNullMark.length, 1);
  assert.ok(rhsNullMark[0].textContent.includes('no value'));
  assert.ok(rhsNullMark[0].textContent.includes('Compared variable had no value at 40 ms'));
});

test('over-2^53 64-bit integer comparison failure: browser Number rounds 9007199254740993 to 9007199254740992 while failure message and rounding notes preserve distinction', async () => {
  // Raw JSON text as C++ JsonWriter emits for exact int64_t 9007199254740993 vs 9007199254740992.
  const rawJson = `{
    "vehicle_sil_timeline": 1,
    "vehicle_sil": "0.6.0",
    "stage": "run",
    "file": "simulation.yaml",
    "variant": null,
    "params": [],
    "time_step_ms": 10,
    "scenario": {
      "file": "scenarios/int64_compare.yaml",
      "name": "int64 above 2^53",
      "status": "failed",
      "over_at_ms": 10,
      "recorded_to_ms": 10,
      "end_ms": null
    },
    "variables": [
      {
        "name": "Engine.counter",
        "participant": "Engine",
        "type": "integer",
        "causality": "output",
        "unit": "",
        "min": null,
        "max": null,
        "items": [],
        "ecu_step_ms": 10,
        "written": false,
        "watched": true
      },
      {
        "name": "Dashboard.counter",
        "participant": "Dashboard",
        "type": "integer",
        "causality": "output",
        "unit": "",
        "min": null,
        "max": null,
        "items": [],
        "ecu_step_ms": 10,
        "written": false,
        "watched": true
      }
    ],
    "steps": [
      {
        "index": 1,
        "line": 4,
        "kind": "expect",
        "text": "expect Dashboard.counter eq Engine.counter",
        "status": "failed",
        "start_ms": 10,
        "at_ms": 10,
        "end_ms": null,
        "code": "scenario-failed",
        "message": "at 10 ms Dashboard.counter eq Engine.counter, but it was 9007199254740993 (its sample of 10 ms) while Engine.counter was 9007199254740992 (its sample of 10 ms)",
        "lines": [],
        "watch": {
          "mode": "at",
          "from_ms": 10,
          "to_ms": null,
          "conditions": [
            {
              "variable": "Dashboard.counter",
              "compare": [{ "op": "eq", "value": null, "variable": "Engine.counter", "tolerance": null }]
            }
          ]
        },
        "failed": {
          "at_ms": 10,
          "code": "scenario-failed",
          "conditions": [
            {
              "condition": 0,
              "value": 9007199254740993,
              "sample_ms": 10,
              "compared": [{ "variable": "Engine.counter", "value": 9007199254740992, "sample_ms": 10 }]
            }
          ],
          "writes": [],
          "write": null
        }
      }
    ],
    "series": [
      {
        "variable": "Engine.counter",
        "source": "sampled",
        "encoding": "changes",
        "samples": 1,
        "first_ms": 10,
        "last_ms": 10,
        "not_applied_from": null,
        "columns": ["at_ms", "value"],
        "points": [[10, 9007199254740992]]
      },
      {
        "variable": "Dashboard.counter",
        "source": "sampled",
        "encoding": "changes",
        "samples": 1,
        "first_ms": 10,
        "last_ms": 10,
        "not_applied_from": null,
        "columns": ["at_ms", "value"],
        "points": [[10, 9007199254740993]]
      }
    ],
    "lines": [],
    "findings": [],
    "limits": { "points_per_series": 10000, "points_total": 100000, "dropped": [] }
  }`;

  const read = T.readTimeline(rawJson, 'int64.timeline.json');
  assert.equal(read.error, undefined);
  const m = T.buildModel(read.timeline);
  // JS JSON.parse rounds 9007199254740993 to IEEE-754 double 9007199254740992 (same as RHS!).
  assert.equal(m.failure.conditions[0].value, 9007199254740992);
  assert.equal(m.failure.conditions[0].compared[0].value, 9007199254740992);
  assert.equal(m.failure.conditions[0].valueText, '9007199254740992');
  assert.equal(m.failure.conditions[0].compared[0].valueText, '9007199254740992');
  // Both lanes warn about precision loss beyond ±9007199254740991.
  assert.match(m.byName.get('Dashboard.counter').notes.join(), /beyond ±9007199254740991/);
  assert.match(m.byName.get('Engine.counter').notes.join(), /beyond ±9007199254740991/);

  const { Element } = makeMockDom();
  const el = new Element();
  el.data = rawJson;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  // Failure message string preserves the exact C++ 64-bit integer difference.
  const msg = el._shadow.querySelectorAll('[data-testid=failure-message]')[0].textContent;
  assert.ok(msg.includes('9007199254740993'), msg);
  assert.ok(msg.includes('9007199254740992'), msg);
  // Conditions block warns about browser rounding.
  const note = el._shadow.querySelectorAll('[data-testid=conditions-rounded-note]')[0].textContent;
  assert.match(note, /beyond ±9007199254740991.*exact 64-bit values/);
});

test('ne-only dynamic band draws threshold.ne without allowed rect, and unobserved once falls back to lane center', async () => {
  const tl = makeDynamicTimeline({
    steps: [
      {
        index: 1,
        line: 3,
        kind: 'expect',
        text: 'expect Dashboard.speed ne Engine.speed',
        status: 'passed',
        start_ms: 0,
        at_ms: 40,
        end_ms: 40,
        code: null,
        message: null,
        lines: [],
        watch: {
          mode: 'always',
          from_ms: 0,
          to_ms: 40,
          conditions: [
            {
              variable: 'Dashboard.speed',
              compare: [{ op: 'ne', value: null, variable: 'Engine.speed', tolerance: null }],
            },
          ],
        },
        failed: null,
      },
      {
        index: 2,
        line: 6,
        kind: 'wait_until',
        text: 'wait_until Dashboard.speed eq Engine.speed',
        status: 'failed',
        start_ms: 50,
        at_ms: 60,
        end_ms: null,
        code: 'scenario-timeout',
        message: 'timed out',
        lines: [],
        watch: {
          mode: 'once',
          from_ms: 50,
          to_ms: 100,
          conditions: [
            {
              variable: 'Dashboard.speed',
              compare: [{ op: 'eq', value: null, variable: 'Missing.speed', tolerance: null }],
            },
          ],
        },
        failed: {
          at_ms: 60,
          code: 'scenario-timeout',
          conditions: [
            {
              condition: 0,
              value: 1200,
              sample_ms: 60,
              compared: [{ variable: 'Missing.speed', value: null, sample_ms: null }],
            },
          ],
          writes: [],
          write: null,
        },
      },
    ],
    series: [
      makeSeries('Engine.speed', 'sampled', [[0, 1000]], { last_ms: 60 }),
      makeSeries('Dashboard.speed', 'sampled', [[0, 1200]], { last_ms: 60 }),
    ],
  });
  tl.scenario.status = 'failed';
  tl.scenario.recorded_to_ms = 60;
  tl.scenario.over_at_ms = 60;

  const m = model(tl);
  const bands = m.byName.get('Dashboard.speed').bands;
  assert.equal(bands[0].slices.length, 1);
  assert.equal(bands[0].slices[0].hasBound, false);
  assert.deepEqual(bands[0].slices[0].excluded, [1000]);
  assert.equal(bands[1].slices.length, 0);
  assert.equal(bands[1].limitValue, null);

  const { Element } = makeMockDom();
  const el = new Element();
  el.from = 0;
  el.to = 110;
  el.data = tl;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(
    findNodes(el._shadow, (n) => n.tagName === 'rect' && n.attrs.class === 'allowed').length,
    0,
  );
  assert.equal(
    findNodes(el._shadow, (n) => n.tagName === 'line' && n.attrs.class === 'threshold ne').length,
    1,
  );
  const limitLines = findNodes(
    el._shadow,
    (n) => n.tagName === 'line' && n.attrs.class === 'threshold limit',
  );
  assert.equal(limitLines.length, 1);
  // Unobserved once limit falls back to vertical center of the numeric lane (40 px), not top edge (4 px).
  assert.equal(Number(limitLines[0].attrs.y1), 38);
});

test('terminal rhs sample at exact watch endpoint resolves slice, limitValue, and laneRange across once, during, and always', async () => {
  // 1. Passed once [0, 200] where rhs jumps at exact at_ms = 40: terminal slice [40, 40] and limitValue = 50.
  const onceJump = makeDynamicTimeline({
    steps: [
      {
        index: 1,
        line: 3,
        kind: 'wait_until',
        text: 'wait_until Dashboard.speed near Engine.speed (tolerance 5)',
        status: 'passed',
        start_ms: 0,
        at_ms: 40,
        end_ms: 40,
        code: null,
        message: null,
        lines: [],
        watch: {
          mode: 'once',
          from_ms: 0,
          to_ms: 200,
          conditions: [
            {
              variable: 'Dashboard.speed',
              compare: [{ op: 'near', value: null, variable: 'Engine.speed', tolerance: 5 }],
            },
          ],
        },
        failed: null,
      },
    ],
    series: [
      makeSeries(
        'Engine.speed',
        'sampled',
        [
          [0, 100],
          [40, 50],
        ],
        { last_ms: 40 },
      ),
      makeSeries('Dashboard.speed', 'sampled', [[0, 50]], { last_ms: 40 }),
    ],
  });
  onceJump.scenario.recorded_to_ms = 40;
  onceJump.scenario.over_at_ms = 40;
  const mOnceJump = model(onceJump);
  const bOnceJump = mOnceJump.byName.get('Dashboard.speed').bands[0];
  assert.deepEqual(
    bOnceJump.slices.map((s) => [s.from, s.to, s.lo, s.hi]),
    [
      [0, 40, 95, 105],
      [40, 40, 45, 55],
    ],
  );
  assert.equal(bOnceJump.limitValue, 50);

  // Verify DOM rendering draws both slices and aligns .threshold.limit with the matched 50 slice.
  const { Element } = makeMockDom();
  const el = new Element();
  el.data = onceJump;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  const sliceTitles = findNodes(
    el._shadow,
    (n) => n.tagName === 'g' && n.attrs.class === 'band-slice',
  ).map((g) => g.textContent);
  assert.deepEqual(sliceTitles, ['near 100 ± 5 (0 ms to 40 ms)', 'near 50 ± 5 (40 ms to 40 ms)']);
  const limitLine = findNodes(
    el._shadow,
    (n) => n.tagName === 'line' && n.attrs.class === 'threshold limit',
  )[0];
  const thLines = findNodes(
    el._shadow,
    (n) => n.tagName === 'line' && n.attrs.class === 'threshold',
  );
  assert.equal(thLines.length, 2);
  assert.equal(limitLine.attrs.y1, thLines[1].attrs.y1);

  // 2. Passed once [0, 200] where rhs first arrives at exact at_ms = 50: terminal slice [50, 50], limitValue = 80, laneRange includes 80.
  const onceFirst = JSON.parse(JSON.stringify(onceJump));
  onceFirst.steps[0].at_ms = 50;
  onceFirst.steps[0].end_ms = 50;
  onceFirst.scenario.recorded_to_ms = 50;
  onceFirst.scenario.over_at_ms = 50;
  onceFirst.series[0] = makeSeries('Engine.speed', 'sampled', [[50, 80]], { last_ms: 50 });
  onceFirst.series[1] = makeSeries('Dashboard.speed', 'sampled', [[0, 10]], { last_ms: 50 });
  const mOnceFirst = model(onceFirst);
  const dashFirst = mOnceFirst.byName.get('Dashboard.speed');
  assert.deepEqual(
    dashFirst.bands[0].slices.map((s) => [s.from, s.to, s.lo, s.hi]),
    [[50, 50, 75, 85]],
  );
  assert.equal(dashFirst.bands[0].limitValue, 80);
  const [, hiFirst] = T.laneRange(dashFirst, true);
  assert.ok(hiFirst >= 85, `expected laneRange hi >= 85, got ${hiFirst}`);

  // 3. Passed once with intersection (ge rhs and le 100) empty on [0, 50) and non-empty at endpoint 50.
  const onceIntersect = JSON.parse(JSON.stringify(onceFirst));
  onceIntersect.steps[0].watch.conditions[0].compare = [
    { op: 'ge', value: null, variable: 'Engine.speed', tolerance: null },
    { op: 'le', value: 100, tolerance: null },
  ];
  onceIntersect.series[0] = makeSeries(
    'Engine.speed',
    'sampled',
    [
      [0, 120],
      [50, 80],
    ],
    { last_ms: 50 },
  );
  const bOnceInt = model(onceIntersect).byName.get('Dashboard.speed').bands[0];
  assert.deepEqual(
    bOnceInt.slices.map((s) => [s.from, s.to, s.lo, s.hi]),
    [[50, 50, 80, 100]],
  );
  assert.equal(bOnceInt.limitValue, 80);

  // 4. Failed during [10, 40] where rhs's only in-window sample arrives at exact endpoint 40.
  const duringEnd = makeDynamicTimeline({
    scenario: {
      file: 'scenarios/tracking.yaml',
      name: 'during endpoint',
      status: 'failed',
      over_at_ms: 40,
      recorded_to_ms: 40,
      end_ms: null,
    },
    steps: [
      {
        index: 1,
        line: 4,
        kind: 'ramp',
        text: 'ramp with expect Dashboard.speed le Engine.speed',
        status: 'failed',
        start_ms: 10,
        at_ms: 40,
        end_ms: null,
        code: 'scenario-failed',
        message: 'failed at 40 ms',
        lines: [],
        watch: {
          mode: 'during',
          from_ms: 10,
          to_ms: 40,
          conditions: [
            {
              variable: 'Dashboard.speed',
              compare: [{ op: 'le', value: null, variable: 'Engine.speed', tolerance: null }],
            },
          ],
        },
        failed: {
          at_ms: 40,
          code: 'scenario-failed',
          conditions: [
            {
              condition: 0,
              value: 50,
              sample_ms: 40,
              compared: [{ variable: 'Engine.speed', value: 20, sample_ms: 40 }],
            },
          ],
          writes: [],
          write: null,
        },
      },
    ],
    series: [
      makeSeries(
        'Engine.speed',
        'sampled',
        [
          [0, 100],
          [40, 20],
        ],
        { last_ms: 40 },
      ),
      makeSeries('Dashboard.speed', 'sampled', [[10, 50]], { last_ms: 40 }),
    ],
  });
  assert.deepEqual(
    model(duringEnd)
      .byName.get('Dashboard.speed')
      .bands[0].slices.map((s) => [s.from, s.to, s.lo, s.hi]),
    [[40, 40, -Infinity, 20]],
  );

  // 5. Failed always [0, 100] where rhs jumps at exact failed.at_ms = 40.
  const alwaysEnd = JSON.parse(JSON.stringify(duringEnd));
  alwaysEnd.steps[0].kind = 'expect';
  alwaysEnd.steps[0].watch.mode = 'always';
  alwaysEnd.steps[0].watch.from_ms = 0;
  alwaysEnd.steps[0].watch.to_ms = 100;
  assert.deepEqual(
    model(alwaysEnd)
      .byName.get('Dashboard.speed')
      .bands[0].slices.map((s) => [s.from, s.to, s.lo, s.hi]),
    [
      [0, 40, -Infinity, 100],
      [40, 40, -Infinity, 20],
    ],
  );
});

test('constant comparison bound above 2^53 triggers conditions-rounded-note while fractional tolerances do not', async () => {
  const rawConstOver = `{
    "vehicle_sil_timeline": 1,
    "vehicle_sil": "0.6.0",
    "stage": "run",
    "file": "simulation.yaml",
    "variant": null,
    "params": [],
    "time_step_ms": 10,
    "scenario": {
      "file": "scenarios/const_int64.yaml",
      "name": "constant int64 bound",
      "status": "failed",
      "over_at_ms": 50,
      "recorded_to_ms": 50,
      "end_ms": null
    },
    "variables": [
      { "name": "ECU.a", "participant": "ECU", "type": "integer", "causality": "output", "unit": "", "min": null, "max": null, "items": [], "ecu_step_ms": 10, "written": false, "watched": true }
    ],
    "steps": [
      {
        "index": 1,
        "line": 4,
        "kind": "expect",
        "text": "expect ECU.a eq 9007199254740993",
        "status": "failed",
        "start_ms": 50,
        "at_ms": 50,
        "end_ms": null,
        "code": "scenario-failed",
        "message": "at 50 ms ECU.a eq 9007199254740993, but it was 100 (its sample of 50 ms)",
        "lines": [],
        "watch": {
          "mode": "at",
          "from_ms": 50,
          "to_ms": null,
          "conditions": [
            { "variable": "ECU.a", "compare": [{ "op": "eq", "value": 9007199254740993, "tolerance": null }] }
          ]
        },
        "failed": {
          "at_ms": 50,
          "code": "scenario-failed",
          "conditions": [{ "condition": 0, "value": 100, "sample_ms": 50 }],
          "writes": [],
          "write": null
        }
      }
    ],
    "series": [
      { "variable": "ECU.a", "source": "sampled", "encoding": "changes", "samples": 1, "first_ms": 0, "last_ms": 50, "not_applied_from": null, "columns": ["at_ms", "value"], "points": [[0, 100]] }
    ],
    "lines": [],
    "findings": [],
    "limits": { "points_per_series": 10000, "points_total": 100000, "dropped": [] }
  }`;

  const { Element } = makeMockDom();
  const el = new Element();
  el.data = rawConstOver;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  const notes = el._shadow.querySelectorAll('[data-testid=conditions-rounded-note]');
  assert.equal(notes.length, 1);
  assert.match(notes[0].textContent, /beyond ±9007199254740991/);

  // Fractional tolerance on a safe integer variable must NOT falsely trigger the rounding note.
  const safeFrac = JSON.parse(rawConstOver);
  safeFrac.steps[0].watch.conditions[0].compare = [{ op: 'near', value: 200, tolerance: 0.5 }];
  const el2 = new Element();
  el2.data = safeFrac;
  el2.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(el2._shadow.querySelectorAll('[data-testid=conditions-rounded-note]').length, 0);
});

test('multi-ne integer intersection omits slices when every integer in [intLo, intHi] is excluded', () => {
  const tl = makeDynamicTimeline({
    variables: [
      {
        name: 'ECU.count',
        participant: 'ECU',
        type: 'integer',
        causality: 'output',
        unit: '',
        min: 0,
        max: 100,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
    ],
    steps: [
      {
        index: 1,
        line: 3,
        kind: 'expect',
        text: 'expect ECU.count ge 10 and le 11 and ne 10 and ne 11',
        status: 'failed',
        start_ms: 0,
        at_ms: 20,
        end_ms: null,
        code: 'scenario-failed',
        message: 'failed',
        lines: [],
        watch: {
          mode: 'always',
          from_ms: 0,
          to_ms: 20,
          conditions: [
            {
              variable: 'ECU.count',
              compare: [
                { op: 'ge', value: 10, tolerance: null },
                { op: 'le', value: 11, tolerance: null },
                { op: 'ne', value: 10, tolerance: null },
                { op: 'ne', value: 11, tolerance: null },
              ],
            },
          ],
        },
        failed: {
          at_ms: 20,
          code: 'scenario-failed',
          conditions: [{ condition: 0, value: 10, sample_ms: 20 }],
          writes: [],
          write: null,
        },
      },
    ],
    series: [makeSeries('ECU.count', 'sampled', [[0, 10]], { last_ms: 20 })],
  });
  tl.scenario.status = 'failed';
  tl.scenario.recorded_to_ms = 20;
  tl.scenario.over_at_ms = 20;

  assert.deepEqual(model(tl).byName.get('ECU.count').bands[0].slices, []);

  // When one integer (11) remains allowed, the slice is preserved.
  const nonEmpty = JSON.parse(JSON.stringify(tl));
  nonEmpty.steps[0].watch.conditions[0].compare.pop();
  assert.deepEqual(
    model(nonEmpty)
      .byName.get('ECU.count')
      .bands[0].slices.map((s) => [s.from, s.to, s.lo, s.hi, s.excluded]),
    [[0, 20, 10, 11, [10]]],
  );
});

test('logic analyzer: event lane sits below variable lanes and is omitted cleanly when steps is empty', async () => {
  const { Element } = makeMockDom();
  const el = new Element();
  el.data = reference('fails_beside');
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));

  const chart = el._shadow.getElementById('chart');
  const laneClasses = chart.children
    .filter((c) => typeof c.className === 'string' && c.className.split(' ').includes('lane'))
    .map((c) => c.className);
  assert.ok(laneClasses[0].includes('axis'), 'axis lane is first');
  assert.ok(laneClasses.at(-1).includes('steps'), 'steps lane is at the bottom below waveforms');

  // Eventless timeline omits the event lane completely and uses clean summary text.
  const eventless = JSON.parse(JSON.stringify(reference('motions')));
  eventless.steps = [];
  eventless.lines = [];
  const el2 = new Element();
  el2.data = eventless;
  el2.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  const chart2 = el2._shadow.getElementById('chart');
  const stepLanes2 = chart2.children.filter(
    (c) => typeof c.className === 'string' && c.className.split(' ').includes('steps'),
  );
  assert.equal(stepLanes2.length, 0, 'eventless recording omits empty steps lane');
  assert.equal(el2._shadow.getElementById('step-details').hidden, true);
  const summaryText = el2._shadow.getElementById('summary').textContent;
  assert.match(summaryText, /Passed \(no scenario steps\)/);
  assert.doesNotMatch(summaryText, /0 steps/);
});

test('logic analyzer: pixel-width row packing prevents label overlap and labels narrow bars concisely', () => {
  const scale = T.makeScale(0, 940, 1000);
  const steps = [
    {
      index: 3,
      kind: 'ramp',
      name: 'prefill brake pressure',
      status: 'passed',
      start_ms: 100,
      at_ms: 120,
      end_ms: 120,
    },
    {
      index: 4,
      kind: 'set',
      name: 'engage AEB and regen cut',
      status: 'passed',
      start_ms: 200,
      at_ms: 200,
      end_ms: 200,
    },
    {
      index: 5,
      kind: 'ramp',
      name: 'short ramp',
      status: 'passed',
      start_ms: 200,
      at_ms: 220,
      end_ms: 220,
    },
    {
      index: 6,
      kind: 'ramp',
      name: 'ramp deceleration to -7.5 m/s^2',
      status: 'passed',
      start_ms: 220,
      at_ms: 520,
      end_ms: 520,
    },
  ];
  const { items, rows } = T.layoutStepItems(steps, scale, 940);
  // Every pair of items on the same row must have non-overlapping pixel boxes.
  for (let i = 0; i < items.length; i++) {
    for (let j = i + 1; j < items.length; j++) {
      if (rows[i] !== rows[j]) continue;
      assert.ok(
        items[i].x1 <= items[j].x0 || items[j].x1 <= items[i].x0,
        `items #${items[i].st.index} and #${items[j].st.index} overlap on row ${rows[i]}`,
      );
    }
  }
  // Point event #4 and adjacent bar #6 cannot share the same row because #4's label extends past 220 ms.
  const idx4 = items.findIndex((it) => it.st.index === 4);
  const idx6 = items.findIndex((it) => it.st.index === 6);
  assert.notEqual(rows[idx4], rows[idx6], '#4 label and #6 bar are placed on separate rows');
  // Narrow bars (#3, #5 ~21px wide) get concise #N labels instead of being blank.
  const it3 = items.find((it) => it.st.index === 3);
  const it5 = items.find((it) => it.st.index === 5);
  assert.equal(it3.displayLabel, '#3');
  assert.equal(it5.displayLabel, '#5');
});

test('logic analyzer: simultaneous events render cluster badge and list all siblings with unordered wording', async () => {
  const tl = JSON.parse(JSON.stringify(reference('fails_beside')));
  tl.steps = [
    {
      index: 1,
      line: 5,
      kind: 'set',
      name: 'override pulse',
      text: 'set Body.level = 30',
      status: 'passed',
      start_ms: 50,
      at_ms: 50,
      end_ms: 50,
      code: null,
      message: null,
      lines: [],
      watch: null,
      failed: null,
    },
    {
      ...tl.steps[1],
      index: 2,
      start_ms: 50,
      name: 'braking check',
    },
  ];
  const groups = T.sameStartGroups(tl.steps);
  assert.equal(groups.size, 1);
  assert.deepEqual(
    groups.get(50).map((s) => s.index),
    [1, 2],
  );

  const { Element } = makeMockDom();
  const el = new Element();
  el.data = tl;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));

  const clusters = el._shadow.querySelectorAll('.sim-cluster');
  assert.equal(clusters.length, 1);
  assert.equal(clusters[0].getAttribute('tabindex'), '0');
  assert.equal(clusters[0].getAttribute('role'), 'button');
  assert.match(clusters[0].textContent, /×2/);

  const banner = el._shadow.querySelectorAll('[data-testid=simultaneous-banner]');
  assert.equal(banner.length, 1);
  assert.match(banner[0].textContent, /display order does not imply causal order/i);
  const items = el._shadow.querySelectorAll('[data-sim-step]');
  assert.equal(items.length, 2);

  // Japanese wording explicitly states non-causal order.
  el.setAttribute('lang', 'ja');
  el.attributeChangedCallback('lang', null, 'ja');
  await new Promise((r) => Promise.resolve().then(r));
  const bannerJa = el._shadow.querySelectorAll('[data-testid=simultaneous-banner]');
  assert.equal(bannerJa.length, 1);
  assert.match(bannerJa[0].textContent, /表示順は因果関係を示しません/);
});

test('logic analyzer: pre-failure framing, zoom-preserving jump/focus, pan clamping, and end-edge state marker', async () => {
  const tl = makeDynamicTimeline({
    variables: [
      {
        name: 'Brake.pressure',
        participant: 'Brake',
        type: 'real',
        causality: 'output',
        unit: 'bar',
        min: 0,
        max: 200,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'Brake.fault_latch',
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
      {
        name: 'ADAS.aeb_state',
        participant: 'ADAS',
        type: 'enum',
        causality: 'output',
        unit: '',
        min: null,
        max: null,
        items: [
          { name: 'braking', value: 1 },
          { name: 'degraded', value: 2 },
        ],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
    ],
    steps: [
      {
        index: 1,
        line: 4,
        kind: 'set',
        text: 'set initial',
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
        line: 8,
        kind: 'ramp',
        name: 'long braking ramp',
        text: 'ramp Brake.pressure to 120 over 400 ms',
        status: 'failed',
        start_ms: 540,
        at_ms: 640,
        end_ms: 940,
        code: 'scenario-failed',
        message: 'pressure dropped',
        lines: [],
        watch: {
          mode: 'during',
          from_ms: 540,
          to_ms: 940,
          conditions: [
            { variable: 'Brake.pressure', compare: [{ op: 'ge', value: 100, tolerance: null }] },
          ],
        },
        failed: {
          at_ms: 640,
          code: 'scenario-failed',
          conditions: [{ condition: 0, value: 88, sample_ms: 640 }],
          writes: [],
          write: null,
        },
      },
    ],
    series: [
      makeSeries(
        'Brake.pressure',
        'sampled',
        [
          [0, 120],
          [600, 110],
          [640, 88],
        ],
        { last_ms: 640 },
      ),
      makeSeries(
        'Brake.fault_latch',
        'sampled',
        [
          [0, false],
          [640, true],
        ],
        { last_ms: 640 },
      ),
      makeSeries(
        'ADAS.aeb_state',
        'sampled',
        [
          [0, 1],
          [620, 2],
        ],
        { last_ms: 640 },
      ),
    ],
  });
  tl.scenario.status = 'failed';
  tl.scenario.recorded_to_ms = 640;
  tl.scenario.over_at_ms = 650;

  const m = model(tl);
  // Pre-failure framing respects recorded_to_ms = 640 instead of wasting 60% on 640..940 unrecorded tail.
  const [i0, i1] = T.initialView(m);
  assert.ok((640 - i0) / (i1 - i0) >= 0.75, `expected >=75% pre-failure, got [${i0}, ${i1}]`);
  const [s0, s1] = T.stepViewFor(m, m.steps[1]);
  assert.ok((640 - s0) / (s1 - s0) >= 0.75, `failed stepView respects recordedTo: [${s0}, ${s1}]`);

  // Zoom-preserving jump keeps exact 150 ms zoom span and positions 640 ms at 80%.
  const [j0, j1] = T.failureJumpView(m, [150, 300], true);
  assert.ok(Math.abs(j1 - j0 - 150) < 1e-6);
  assert.ok(Math.abs(j0 - 520) < 1e-6 && Math.abs(j1 - 670) < 1e-6);

  // panView clamps cleanly without changing span.
  assert.deepEqual(T.panView([0, 1000], [100, 250], -200), [0, 150]);
  assert.deepEqual(T.panView([0, 1000], [800, 950], 200), [850, 1000]);

  // modelMetadata computes relatedToFailure and changedNearFailure accurately.
  const meta = T.modelMetadata(m, 'brake.timeline.json');
  assert.equal(meta.recordedTo, 640);
  assert.equal(meta.stepCount, 2);
  assert.equal(meta.failure.step, 2);
  assert.equal(meta.failure.at, 640);
  const byVar = new Map(meta.variables.map((v) => [v.name, v]));
  assert.equal(byVar.get('Brake.pressure').relatedToFailure, true);
  assert.equal(byVar.get('ADAS.aeb_state').relatedToFailure, false);
  assert.equal(byVar.get('ADAS.aeb_state').changedNearFailure, true);

  // Element API: jumpToFailure preserves zoom, pins cursor, selects failed step, and survives variables filter.
  const { Element } = makeMockDom();
  const el = new Element();
  let loaded = null;
  el.addEventListener('load', (e) => {
    loaded = e.detail;
  });
  el.data = tl;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(loaded.failure.at, 640);

  // End-edge bool transition at recorded_to_ms (640 ms) draws crisp → true badge.
  const edgeLabels = el._shadow.querySelectorAll('.state-edge-label');
  assert.ok(
    edgeLabels.some((n) => n.textContent.includes('→ true')),
    'end-edge transition marker renders → true',
  );

  el.from = 150;
  el.to = 300;
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(el.jumpToFailure({ preserveZoom: true }), true);
  assert.ok(Math.abs(el.to - el.from - 150) < 1e-6);
  assert.equal(el.cursor, 640);
  assert.equal(el.step, 2);

  // Filtering signals (including empty array) never resets zoom, cursor, or step.
  el.variables = ['Brake.pressure'];
  await new Promise((r) => Promise.resolve().then(r));
  assert.ok(Math.abs(el.to - el.from - 150) < 1e-6);
  assert.equal(el.cursor, 640);
  assert.equal(el.step, 2);

  el.variables = [];
  await new Promise((r) => Promise.resolve().then(r));
  const varNames = el._shadow.querySelectorAll('.var-name');
  assert.equal(varNames.length, 0, 'explicit empty variables array hides all variable lanes');
  assert.equal(el.cursor, 640);
  assert.equal(el.step, 2);
});

test('logic analyzer: niceTicks terminates at extreme zoom and >= 2^53 timestamps', () => {
  // Extreme zoom where step falls below float64 ULP around t0 must never infinite-loop.
  const huge = 2 ** 53;
  const ticksHuge = T.niceTicks(huge, huge + 1, 5);
  assert.ok(ticksHuge.length >= 1 && ticksHuge.length <= 200);
  const tinySpan = T.niceTicks(100, 100 + 1e-15, 5);
  assert.ok(tinySpan.length >= 1 && tinySpan.length <= 200);
});

test('logic analyzer: short-run leadMs, non-sticky navigation, quiet restoration, participants hiding, and single edge label', async () => {
  const shortTl = makeDynamicTimeline({
    variables: [
      {
        name: 'Brake.pressure',
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
        name: 'Brake.fault_latch',
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
      {
        name: 'ADAS.override',
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
    ],
    steps: [
      {
        index: 1,
        line: 5,
        kind: 'set',
        text: 'set ADAS.override = true',
        status: 'passed',
        start_ms: 200,
        at_ms: 200,
        end_ms: 200,
        code: null,
        message: null,
        lines: [],
        watch: null,
        failed: null,
      },
      {
        index: 2,
        line: 8,
        kind: 'expect',
        text: 'expect Brake.fault_latch eq false',
        status: 'failed',
        start_ms: 200,
        at_ms: 250,
        end_ms: 300,
        code: 'scenario-failed',
        message: 'fault latched',
        lines: [],
        watch: {
          mode: 'during',
          from_ms: 200,
          to_ms: 300,
          conditions: [
            {
              variable: 'Brake.fault_latch',
              compare: [{ op: 'eq', value: false, tolerance: null }],
            },
          ],
        },
        failed: {
          at_ms: 250,
          code: 'scenario-failed',
          conditions: [{ condition: 0, value: true, sample_ms: 250 }],
          writes: [],
          write: null,
        },
      },
    ],
    series: [
      makeSeries('Brake.pressure', 'sampled', [[0, 100]], { last_ms: 250 }),
      makeSeries(
        'Brake.fault_latch',
        'sampled',
        [
          [0, false],
          [250, true],
        ],
        { last_ms: 250 },
      ),
      makeSeries(
        'ADAS.override',
        'written',
        [
          [0, false],
          [200, true],
        ],
        { encoding: 'all', last_ms: 250 },
      ),
    ],
  });
  shortTl.scenario.status = 'failed';
  shortTl.scenario.recorded_to_ms = 250;
  shortTl.scenario.over_at_ms = 260;

  const m = model(shortTl);
  // Short run: -300 ms covers the full domain and is NEVER narrower than -150 ms.
  const [f150, t150] = T.failureJumpView(m, m.domain, { leadMs: 150 });
  const [f300, t300] = T.failureJumpView(m, m.domain, { leadMs: 300 });
  assert.ok(
    t300 - f300 >= t150 - f150,
    `-300 ms (${f300}..${t300}) must not be narrower than -150 ms (${f150}..${t150})`,
  );
  assert.equal(f300, m.domain[0]);
  assert.equal(t300, m.domain[1]);

  // Simultaneous-start written signal (ADAS.override written at 200 ms == failed step start_ms) is relatedToFailure.
  const meta = T.modelMetadata(m, 'short.timeline.json');
  const adasVar = meta.variables.find((v) => v.name === 'ADAS.override');
  assert.equal(adasVar.relatedToFailure, true);

  // stepFocusView preserves whole-run view when step is already visible inside it.
  assert.deepEqual(T.stepFocusView(m, m.steps[0], m.domain, true), m.domain);

  // Calling jumpToFailure inside a `data` load listener still emits cursorchange/stepselect (c.quiet cleared/restored).
  const { Element } = makeMockDom();
  const el = new Element();
  const emitted = [];
  el.addEventListener('cursorchange', (e) => emitted.push(['cursorchange', e.detail]));
  el.addEventListener('stepselect', (e) => emitted.push(['stepselect', e.detail]));
  el.addEventListener('viewchange', (e) => emitted.push(['viewchange', e.detail]));
  el.addEventListener('load', () => {
    el.jumpToFailure({ leadMs: 150 });
  });
  el.data = shortTl;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));

  assert.ok(emitted.some(([type]) => type === 'cursorchange'));
  assert.ok(emitted.some(([type]) => type === 'stepselect'));
  assert.equal(el.cursor, 250);
  assert.equal(el.step, 2);

  // Failed end-edge bool transition has ONLY ONE → true label (no duplicate colliding black & red labels).
  const edgeLabels = el._shadow
    .querySelectorAll('.state-edge-label')
    .filter((n) => n.textContent.includes('→ true'));
  assert.equal(edgeLabels.length, 1, 'exactly one non-colliding → true label on failing bool lane');

  // Non-sticky resetView / pan / zoomOut / unpinCursor work without leaking sticky options across reload.
  el.resetView();
  assert.equal(el.from, m.domain[0]);
  assert.equal(el.to, m.domain[1]);
  el.jumpToFailure({ leadMs: 150 });
  const beforePan = el.from;
  el.pan(-0.25);
  assert.ok(el.from < beforePan);
  el.zoomOut();
  el.unpinCursor();

  // Built-in #participants hides automatically when `variables` is host-managed.
  assert.equal(el._shadow.getElementById('participants').hidden, false);
  el.variables = ['Brake.fault_latch'];
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(el._shadow.getElementById('participants').hidden, true);
  el.variables = null;
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(el._shadow.getElementById('participants').hidden, false);
});

test('stepFocusView preserves whole-run view when selecting steps at domain edges d0 and d1', () => {
  const tl = makeDynamicTimeline({
    variables: [
      {
        name: 'Brake.pressure',
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
    steps: [
      {
        index: 1,
        line: 1,
        kind: 'set',
        name: 'init',
        text: 'x = 0',
        start_ms: 0,
        at_ms: 0,
        end_ms: 0,
        status: 'passed',
        code: null,
        message: null,
        lines: [],
        watch: null,
        failed: null,
      },
      {
        index: 2,
        line: 2,
        kind: 'wait',
        name: 'mid',
        text: 'wait 940 ms',
        start_ms: 0,
        at_ms: 940,
        end_ms: 940,
        status: 'passed',
        code: null,
        message: null,
        lines: [],
        watch: null,
        failed: null,
      },
      {
        index: 3,
        line: 3,
        kind: 'check',
        name: 'end',
        text: 'x == 1',
        start_ms: 940,
        at_ms: 940,
        end_ms: 940,
        status: 'passed',
        code: null,
        message: null,
        lines: [],
        watch: null,
        failed: null,
      },
    ],
    series: [
      makeSeries(
        'Brake.pressure',
        'sampled',
        [
          [0, 0],
          [940, 1],
        ],
        { last_ms: 940 },
      ),
    ],
  });
  tl.scenario.recorded_to_ms = 940;
  tl.scenario.over_at_ms = 940;
  const m = model(tl);
  assert.equal(m.domain[0], 0);
  assert.ok(m.domain[1] > 940);

  // Step at start_ms = d0 (0 ms) keeps the whole-run view instead of collapsing to stepViewFor().
  assert.deepEqual(T.stepFocusView(m, m.steps[0], m.domain, true), m.domain);
  // Step at start_ms = d1 also keeps the whole-run view.
  const endEdgeStep = { ...m.steps[2], start_ms: m.domain[1], end_ms: m.domain[1] };
  assert.deepEqual(T.stepFocusView(m, endEdgeStep, m.domain, true), m.domain);
  // Step at start_ms = 0 when zoomed to [0, 200] (flush with d0) also keeps [0, 200].
  assert.deepEqual(T.stepFocusView(m, m.steps[0], [0, 200], true), [0, 200]);
  // Step at 199 when zoomed to [0, 200] (NOT flush with d1) still pans to keep margin from the cropped right edge.
  const nearCroppedRight = { ...m.steps[2], start_ms: 199, end_ms: 199 };
  const panned = T.stepFocusView(m, nearCroppedRight, [0, 200], true);
  assert.ok(panned[1] > 200 && panned[1] - panned[0] === 200);
});

test('modelMetadata keeps watched strictly from info.watched and exposes recorded for extra recorded signals', () => {
  const tl = makeDynamicTimeline({
    variables: [
      {
        name: 'Engine.engine.throttle',
        participant: 'Engine',
        type: 'real',
        causality: 'input',
        unit: '%',
        min: 0,
        max: 100,
        items: [],
        ecu_step_ms: 10,
        written: true,
        watched: false,
      },
      {
        name: 'Engine.engine.speed',
        participant: 'Engine',
        type: 'integer',
        causality: 'output',
        unit: 'rpm',
        min: null,
        max: null,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'Dashboard.dashboard.engine_speed',
        participant: 'Dashboard',
        type: 'integer',
        causality: 'output',
        unit: 'rpm',
        min: null,
        max: null,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: false,
        recorded: true,
      },
    ],
    steps: [
      {
        index: 1,
        line: 5,
        kind: 'set',
        text: 'set Engine.engine.throttle = 50',
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
    ],
    series: [
      makeSeries('Engine.engine.throttle', 'written', [[0, 50]], { encoding: 'all' }),
      makeSeries(
        'Engine.engine.speed',
        'sampled',
        [
          [0, 0],
          [100, 1200],
        ],
        { last_ms: 100 },
      ),
      makeSeries(
        'Dashboard.dashboard.engine_speed',
        'sampled',
        [
          [0, 0],
          [100, 1180],
        ],
        { last_ms: 100 },
      ),
    ],
  });
  tl.scenario.recorded_to_ms = 100;
  tl.scenario.over_at_ms = 100;
  const m = model(tl);
  const meta = T.modelMetadata(m, 'extra.timeline.json');
  const byName = new Map(meta.variables.map((v) => [v.name, v]));

  assert.equal(byName.get('Engine.engine.throttle').written, true);
  assert.equal(byName.get('Engine.engine.throttle').watched, false);
  assert.equal(byName.get('Engine.engine.throttle').recorded, false);

  assert.equal(byName.get('Engine.engine.speed').written, false);
  assert.equal(byName.get('Engine.engine.speed').watched, true);
  assert.equal(byName.get('Engine.engine.speed').recorded, false);

  // Extra recorded signal has sampled points but watched MUST remain false and recorded true.
  const extra = byName.get('Dashboard.dashboard.engine_speed');
  assert.equal(extra.written, false);
  assert.equal(extra.watched, false);
  assert.equal(extra.recorded, true);
  assert.equal(m.byName.get('Dashboard.dashboard.engine_speed').sampled.v[1], 1180);
});

test('post-failure tail: failureJumpView honors trailMs, variableChangedNear scans post-failure window, header shows tail span, and legacy v1 timelines stay unchanged', async () => {
  const tl = makeDynamicTimeline({
    scenario: {
      file: 'scenarios/tail.yaml',
      name: 'post-failure tail',
      status: 'failed',
      over_at_ms: 210,
      recorded_to_ms: 500,
      end_ms: null,
    },
    variables: [
      {
        name: 'Engine.engine.speed',
        participant: 'Engine',
        type: 'integer',
        causality: 'output',
        unit: 'rpm',
        min: null,
        max: null,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: true,
      },
      {
        name: 'Dashboard.dashboard.engine_speed',
        participant: 'Dashboard',
        type: 'integer',
        causality: 'output',
        unit: 'rpm',
        min: null,
        max: null,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: false,
        recorded: true,
      },
      {
        name: 'Static.flag',
        participant: 'Static',
        type: 'bool',
        causality: 'output',
        unit: '',
        min: null,
        max: null,
        items: [],
        ecu_step_ms: 10,
        written: false,
        watched: false,
        recorded: true,
      },
    ],
    steps: [
      {
        index: 1,
        line: 5,
        kind: 'wait_until',
        text: 'wait_until Engine.engine.speed ge 1700',
        status: 'failed',
        start_ms: 0,
        at_ms: 200,
        end_ms: null,
        code: 'scenario-timeout',
        message: 'timed out at 200 ms',
        lines: [],
        watch: {
          mode: 'once',
          from_ms: 0,
          to_ms: 200,
          conditions: [
            {
              variable: 'Engine.engine.speed',
              compare: [{ op: 'ge', value: 1700, tolerance: null }],
            },
          ],
        },
        failed: {
          at_ms: 200,
          code: 'scenario-timeout',
          conditions: [{ condition: 0, value: 1200, sample_ms: 200 }],
          writes: [],
          write: null,
        },
      },
    ],
    series: [
      makeSeries(
        'Engine.engine.speed',
        'sampled',
        [
          [0, 800],
          [200, 1200],
          [350, 1600],
          [500, 1850],
        ],
        { last_ms: 500 },
      ),
      // Changes ONLY after failure (at 280 ms, within +150 ms of failure at 200 ms)
      makeSeries(
        'Dashboard.dashboard.engine_speed',
        'sampled',
        [
          [0, 800],
          [280, 1450],
          [500, 1820],
        ],
        { last_ms: 500 },
      ),
      // Never changes after t=0 -> must remain changedNearFailure: false
      makeSeries('Static.flag', 'sampled', [[0, false]], { last_ms: 500 }),
    ],
  });

  const m = model(tl);
  assert.equal(m.recordedTo, 500);
  assert.equal(m.failure.at, 200);

  // 1. failureJumpView with explicit leadMs + trailMs frames [200 - 150, 200 + 300] = [50, 500].
  assert.deepEqual(T.failureJumpView(m, m.domain, { leadMs: 150, trailMs: 300 }), [50, 500]);
  // Explicit trailMs = 0 clamps right edge to failure timestamp.
  assert.deepEqual(T.failureJumpView(m, m.domain, { leadMs: 150, trailMs: 0 }), [50, 200]);
  // Without trailMs, existing leadMs * 0.25 behavior is preserved.
  assert.deepEqual(T.failureJumpView(m, m.domain, { leadMs: 150 }), [50, 237.5]);

  // 2. variableChangedNear / modelMetadata catches signals changing right after failure in (200, 350].
  const meta = T.modelMetadata(m, 'tail.timeline.json');
  const byName = new Map(meta.variables.map((v) => [v.name, v]));
  assert.equal(byName.get('Engine.engine.speed').changedNearFailure, true);
  assert.equal(byName.get('Dashboard.dashboard.engine_speed').changedNearFailure, true);
  assert.equal(byName.get('Static.flag').changedNearFailure, false);

  // 3. Header fact displays post-failure recorded span in EN and JA.
  const { Element } = makeMockDom();
  const el = new Element();
  el.data = tl;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  const factsEn = el._shadow.getElementById('facts').textContent;
  assert.ok(factsEn.includes('recorded to 500 ms (+300 ms after failure)'), factsEn);

  el.setAttribute('lang', 'ja');
  el.attributeChangedCallback('lang', null, 'ja');
  await new Promise((r) => Promise.resolve().then(r));
  const factsJa = el._shadow.getElementById('facts').textContent;
  assert.ok(factsJa.includes('記録の終わり 500 ms（失敗後 +300 ms）'), factsJa);

  // 4. Legacy v1 failed timeline (recorded_to_ms === failed.at_ms, no `recorded` key) shows plain recordedTo without post-failure suffix.
  const legacy = JSON.parse(JSON.stringify(tl));
  legacy.scenario.recorded_to_ms = 200;
  legacy.variables = [legacy.variables[0]];
  legacy.series = [
    makeSeries(
      'Engine.engine.speed',
      'sampled',
      [
        [0, 800],
        [200, 1200],
      ],
      { last_ms: 200 },
    ),
  ];
  const elLegacy = new Element();
  elLegacy.data = legacy;
  elLegacy.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  const legacyFacts = elLegacy._shadow.getElementById('facts').textContent;
  assert.ok(legacyFacts.includes('recorded to 200 ms'), legacyFacts);
  assert.ok(!legacyFacts.includes('after failure'), legacyFacts);
});

test('manual run timeline (scenario: null): neutral header & summary, no pass/fail badge or warning, omits empty scenario tables', async () => {
  const tl = fixture('manual_run');
  const m = model(tl);
  assert.equal(m.isManual, true);
  assert.equal(m.status, null);
  assert.equal(m.recordedTo, 500);
  assert.equal(m.steps.length, 0);
  assert.equal(m.lines.length, 0);
  assert.equal(m.failure, null);

  const meta = T.modelMetadata(m, 'simulation.timeline.json');
  assert.equal(meta.manual, true);
  assert.equal(meta.scenario, null);
  assert.equal(meta.status, 'manual');
  assert.equal(meta.recordedTo, 500);
  assert.equal(meta.stepCount, 0);
  assert.equal(meta.failure, null);
  assert.deepEqual(
    meta.variables.map((v) => [v.name, v.written, v.watched, v.recorded]),
    [
      ['Engine.engine.speed', false, false, true],
      ['Dashboard.dashboard.engine_speed', false, false, true],
    ],
  );

  // Legacy scenario timeline reports manual: false
  const scenarioMeta = T.modelMetadata(model(reference('waits')), 'waits.timeline.json');
  assert.equal(scenarioMeta.manual, false);

  const { Element } = makeMockDom();
  const el = new Element();
  el.data = tl;
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));

  assert.equal(el._shadow.getElementById('title').textContent, 'Manual run (simulation.yaml)');
  const facts = el._shadow.getElementById('facts');
  assert.equal(
    findNodes(facts, (n) => (n.attrs?.class ?? '').includes('badge')).length,
    0,
    'manual run must not render a pass/fail badge',
  );
  assert.ok(facts.textContent.includes('recorded to 500 ms'), facts.textContent);

  const summary = el._shadow.getElementById('summary');
  assert.equal(el._shadow.querySelectorAll('[data-testid=not-decided]').length, 0);
  assert.equal(findNodes(summary, (n) => (n.attrs?.class ?? '').includes('warn')).length, 0);
  const manualBanner = el._shadow.querySelectorAll('[data-testid=manual-summary]')[0];
  assert.ok(manualBanner, 'neutral manual summary banner should render');
  assert.ok(
    manualBanner.textContent.includes('Recorded 2 signals up to 500 ms'),
    manualBanner.textContent,
  );

  // Empty Steps and Input lines tables are omitted, and empty Findings is omitted too
  assert.equal(el._shadow.querySelectorAll('[data-testid=steps-table]').length, 0);
  assert.equal(el._shadow.querySelectorAll('[data-testid=lines-table]').length, 0);
  assert.equal(el._shadow.querySelectorAll('[data-testid=findings-table]').length, 0);
  const enChartText = el._shadow.getElementById('chart').textContent;
  assert.ok(enChartText.includes('Recorded variables'), enChartText);
  assert.ok(!enChartText.toLowerCase().includes('scenario'), enChartText);

  // Japanese locale renders neutral JA header, summary, and chart group label
  el.setAttribute('lang', 'ja');
  el.attributeChangedCallback('lang', null, 'ja');
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(el._shadow.getElementById('title').textContent, '手動実行（simulation.yaml）');
  assert.ok(
    el._shadow
      .getElementById('summary')
      .textContent.includes('2 個の信号を 500 ms まで記録しました'),
  );
  const jaChartText = el._shadow.getElementById('chart').textContent;
  assert.ok(jaChartText.includes('記録した変数'), jaChartText);
  assert.ok(!jaChartText.includes('シナリオ'), jaChartText);

  // Manual run with findings + dropped + late values keeps findings table and notices
  const withNotices = JSON.parse(JSON.stringify(tl));
  withNotices.findings = [
    {
      severity: 'warning',
      code: 'control-link-late-values',
      file: null,
      line: null,
      message: 'late values',
    },
  ];
  withNotices.limits.dropped = [
    {
      variable: 'Engine.engine.speed',
      source: 'sampled',
      dropped: 3,
      dropped_from_ms: 0,
      dropped_before_ms: 30,
    },
  ];
  const elNotices = new Element();
  elNotices.data = withNotices;
  elNotices.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  assert.ok(
    elNotices._shadow.getElementById('summary').textContent.includes('Points are missing.'),
  );
  assert.ok(
    elNotices._shadow.getElementById('summary').textContent.includes('ECU values arrived late'),
  );
  assert.equal(elNotices._shadow.querySelectorAll('[data-testid=findings-table]').length, 1);
  assert.equal(elNotices._shadow.querySelectorAll('[data-testid=steps-table]').length, 0);

  // Manual run with recorded_to_ms: null (zero samples recorded)
  const emptyManual = JSON.parse(JSON.stringify(tl));
  emptyManual.recorded_to_ms = null;
  emptyManual.series = [];
  const elEmpty = new Element();
  elEmpty.data = emptyManual;
  elEmpty.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  assert.ok(
    elEmpty._shadow
      .getElementById('summary')
      .textContent.includes('Manual run: no samples recorded'),
  );
  assert.ok(elEmpty._shadow.getElementById('facts').textContent.includes('nothing recorded'));
});

test('virtual-time ECU events: parsing, grouping, filtering, ticks, drawer, pinCursor(), and events_dropped banner', async () => {
  const tl = fixture('ecu_events');
  const m = model(tl);
  assert.equal(m.events.length, 5);
  assert.equal(m.eventsDropped.length, 0);
  assert.equal(m.eventGroups.length, 3);
  // Simultaneous timestamps are grouped (0 ms: 2 info -> info; 300 ms: 1 warning -> warning; 520 ms: error+info -> error).
  assert.deepEqual(
    m.eventGroups.map((g) => [g.atMs, g.severity, g.events.length]),
    [
      [0, 'info', 2],
      [300, 'warning', 1],
      [520, 'error', 2],
    ],
  );

  // filterEcuEvents supports participant, level, text query, and visible zoom range
  assert.equal(T.filterEcuEvents(m.events, { participant: 'Engine' }).length, 3);
  assert.equal(T.filterEcuEvents(m.events, { level: 'warning' }).length, 1);
  assert.equal(T.filterEcuEvents(m.events, { query: 'heartbeat' }).length, 1);
  assert.equal(
    T.filterEcuEvents(m.events, { visibleOnly: true, view: [200, 400] })
      .map((e) => e.atMs)
      .join(','),
    '300',
  );

  const meta = T.modelMetadata(m, 'ecu_events.timeline.json');
  assert.equal(meta.eventCount, 5);

  // Legacy v1 timeline without events has 0 events, no events lane, and hidden #ecu-events
  const legacyTl = reference('waits');
  assert.equal(T.modelMetadata(model(legacyTl), 'waits.timeline.json').eventCount, 0);
  const { Element } = makeMockDom();
  const elLegacy = new Element();
  elLegacy.data = legacyTl;
  elLegacy.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(elLegacy._shadow.getElementById('ecu-events').hidden, true);
  assert.equal(elLegacy._shadow.querySelectorAll('[data-testid=ecu-event-tick]').length, 0);

  // Render ecu_events fixture even with hide="header summary help tables toolbar" (embedded mode)
  const el = new Element();
  el.hide = 'header summary help tables toolbar';
  el.data = tl;
  const cursorEvents = [];
  el.addEventListener('cursorchange', (e) => cursorEvents.push(e.detail.cursor));
  el.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));

  assert.equal(el._shadow.getElementById('ecu-events').hidden, false);
  const ticks = el._shadow.querySelectorAll('[data-testid=ecu-event-tick]');
  assert.equal(ticks.length, 3);
  assert.ok(
    findNodes(
      el._shadow.getElementById('chart'),
      (n) => n.tagName === 'text' && n.textContent === '×2',
    ).length >= 2,
    'simultaneous timestamps should show ×2 badges',
  );
  const rows = el._shadow.querySelectorAll('[data-testid=ecu-event-row]');
  assert.equal(rows.length, 5);
  assert.equal(el._shadow.querySelectorAll('[data-testid=events-dropped-banner]').length, 0);

  // Clicking a row pins the waveform cursor at that exact virtual time and updates Δt
  rows[2].dispatchEvent({ type: 'click', stopPropagation() {}, preventDefault() {} });
  assert.equal(el.cursor, 300);
  assert.equal(cursorEvents[cursorEvents.length - 1], 300);
  assert.equal(rows[2].attrs['aria-selected'], 'true');
  const deltas = el._shadow.querySelectorAll('.ev-delta').map((n) => n.textContent);
  assert.deepEqual(deltas, ['-300 ms', '-300 ms', '0 ms', '+220 ms', '+220 ms']);

  // Public pinCursor() clamps to domain, emits cursorchange, and is not clobbered by pending from/to/cursor microtasks
  assert.equal(el.pinCursor(520), true);
  assert.equal(el.cursor, 520);
  assert.equal(cursorEvents[cursorEvents.length - 1], 520);
  assert.equal(el.pinCursor(-50), true);
  assert.equal(el.cursor, 0);
  assert.equal(el.pinCursor('not-a-number'), false);

  // Synchronous el.from / el.to + el.pinCursor() must pan into view and keep the panned view after microtask flush
  el.from = 0;
  el.to = 100;
  assert.equal(el.pinCursor(520), true);
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(el.cursor, 520);
  assert.ok(el.from <= 520 && el.to >= 520, `expected 520 inside [${el.from}, ${el.to}]`);

  // Synchronous el.from / el.to + in-view el.pinCursor(50) must still redraw #range, emit viewchange, and refresh visibleOnly rows
  el.resetView();
  const visibleToggle = el._shadow.getElementById('ecu-events-visible-only');
  visibleToggle.checked = true;
  visibleToggle.dispatchEvent({ type: 'change', target: visibleToggle });
  assert.equal(el._shadow.querySelectorAll('[data-testid=ecu-event-row]').length, 5);
  const viewEvents = [];
  el.addEventListener('viewchange', (e) => viewEvents.push(e.detail));
  el.from = 0;
  el.to = 100;
  assert.equal(el.pinCursor(50), true);
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(el.cursor, 50);
  assert.equal(el.from, 0);
  assert.equal(el.to, 100);
  assert.deepEqual(viewEvents[viewEvents.length - 1], { from: 0, to: 100 });
  assert.ok(
    el._shadow.getElementById('range').textContent.includes('Showing 0 ms to 100 ms'),
    el._shadow.getElementById('range').textContent,
  );
  assert.deepEqual(
    el._shadow.querySelectorAll('[data-testid=ecu-event-row]').map((n) => n.attrs['data-event-at']),
    ['0', '0'],
  );
  visibleToggle.checked = false;
  visibleToggle.dispatchEvent({ type: 'change', target: visibleToggle });

  // Synchronous el.cursor = null / el.cursor = 100 + el.pinCursor() must remain pinned after microtask flush
  el.cursor = null;
  assert.equal(el.pinCursor(300), true);
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(el.cursor, 300);

  el.cursor = 100;
  assert.equal(el.pinCursor(300), true);
  await new Promise((r) => Promise.resolve().then(r));
  assert.equal(el.cursor, 300);
  el.resetView();

  // Empty manual --record-events timeline (0 signals, 0 events, recorded_to_ms != null) shows neutral EN & JA summary
  const emptyEventsManual = JSON.parse(JSON.stringify(fixture('manual_run')));
  emptyEventsManual.variables = [];
  emptyEventsManual.series = [];
  emptyEventsManual.events = [];
  const elEmptyEv = new Element();
  elEmptyEv.data = emptyEventsManual;
  elEmptyEv.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  assert.ok(
    elEmptyEv._shadow
      .getElementById('summary')
      .textContent.includes('No signals or ECU events recorded up to 500 ms'),
    elEmptyEv._shadow.getElementById('summary').textContent,
  );
  elEmptyEv.setAttribute('lang', 'ja');
  elEmptyEv.attributeChangedCallback('lang', null, 'ja');
  await new Promise((r) => Promise.resolve().then(r));
  assert.ok(
    elEmptyEv._shadow
      .getElementById('summary')
      .textContent.includes('500 ms までに記録された信号や ECU イベントはありません'),
    elEmptyEv._shadow.getElementById('summary').textContent,
  );

  // Filtering by participant and text + visible-only toggle synced to zoom
  const partSelect = el._shadow.getElementById('ecu-events-participant');
  partSelect.value = 'Dashboard';
  partSelect.dispatchEvent({ type: 'change', target: partSelect });
  assert.equal(el._shadow.querySelectorAll('[data-testid=ecu-event-row]').length, 2);

  partSelect.value = '';
  partSelect.dispatchEvent({ type: 'change', target: partSelect });
  const visibleCb = el._shadow.getElementById('ecu-events-visible-only');
  visibleCb.checked = true;
  visibleCb.dispatchEvent({ type: 'change', target: visibleCb });
  el.from = 250;
  el.to = 350;
  await new Promise((r) => Promise.resolve().then(r));
  const zoomedRows = el._shadow.querySelectorAll('[data-testid=ecu-event-row]');
  assert.equal(zoomedRows.length, 1);
  assert.equal(zoomedRows[0].attrs['data-event-at'], '300');

  // events_dropped honesty banner renders for both dropped and agent_dropped entries (EN & JA)
  const droppedTl = JSON.parse(JSON.stringify(tl));
  droppedTl.limits.events_dropped = [
    {
      participant: 'Engine',
      dropped: 12,
      dropped_from_ms: 0,
      dropped_before_ms: 200,
      agent_dropped: 3,
    },
  ];
  const elDrop = new Element();
  elDrop.data = droppedTl;
  elDrop.connectedCallback();
  await new Promise((r) => Promise.resolve().then(r));
  const bannerEn = elDrop._shadow.querySelectorAll('[data-testid=events-dropped-banner]')[0];
  assert.ok(bannerEn, 'events_dropped honesty banner must render');
  assert.ok(
    bannerEn.textContent.includes('Engine: 12 dropped (0 ms to 200 ms)'),
    bannerEn.textContent,
  );
  assert.ok(bannerEn.textContent.includes('3 dropped in ECU queue'), bannerEn.textContent);

  elDrop.setAttribute('lang', 'ja');
  elDrop.attributeChangedCallback('lang', null, 'ja');
  await new Promise((r) => Promise.resolve().then(r));
  const bannerJa = elDrop._shadow.querySelectorAll('[data-testid=events-dropped-banner]')[0];
  assert.ok(
    bannerJa.textContent.includes('一部の ECU イベントが欠けています。'),
    bannerJa.textContent,
  );
  assert.ok(bannerJa.textContent.includes('ECU 側キューで 3 件欠落'), bannerJa.textContent);
});
