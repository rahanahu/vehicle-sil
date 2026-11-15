// The timeline page of vehicle-sil: draws a timeline file, the JSON that
// `vehicle_sil_launch run <configuration> --scenario <scenario> --timeline <file>` writes.
// The format is schema/timeline.schema.json and "Recording a scenario run on a timeline" in docs/simulation-run.md.
//
// One plain script: no framework, no build, no fonts or scripts from elsewhere. All text goes in with textContent
// (never innerHTML), because names, units and messages come from users and ECUs.
// The data comes in one of 3 ways, in this order:
//   1. embedded in the page: the file's JSON as the text of the element with id "timeline-data" (a script
//      element of type application/json, which timeline.html has empty). A tool that makes one HTML file puts
//      it there, with '<', '>' and '&' written as \u003c, \u003e and \u0026. This script must never contain
//      the closing tag of a script element, so that it can be put inline too;
//   2. ?src=<url> on the page's own origin (a server that serves the page and the file);
//   3. a file dropped on the page or chosen with "Open a file" (works when the page is opened as a file).
// Everything above "The page" needs no DOM: tests/viewer.test.js runs it in Node.
(function (global) {
  'use strict';

  const FORMAT = 1;
  const HOW_TO_WRITE =
    'Open the file that "vehicle_sil_launch run <configuration> --scenario <scenario> --timeline <file>" wrote.';

  /** The values this page knows. Anything else is shown by name and text, and not drawn (the format only adds). */
  const KNOWN = {
    stepKind: ['set', 'ramp', 'wave', 'dither', 'expect', 'wait_until'],
    stepStatus: ['passed', 'failed', 'unfinished', 'not_run', 'skipped'],
    mode: ['at', 'always', 'once', 'during'],
    op: ['eq', 'ne', 'ge', 'le', 'gt', 'lt', 'near'],
    lineKind: ['set', 'ramp', 'wave', 'dither'],
    type: ['real', 'integer', 'bool', 'enum'],
    /** The encoding that goes with each source of a series. */
    encoding: { written: 'all', sampled: 'changes' },
  };

  const isObject = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);
  const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
  const knows = (list, v) => list.includes(v);

  // ---------------------------------------------------------------------------------------------------------------
  // Reading a file

  /**
   * Reads the text (or the parsed JSON) of a timeline file.
   * Returns { timeline } or { error } (a sentence that says what is wrong and what to do).
   */
  function readTimeline(input, name) {
    const label = name ? `"${name}"` : 'The file';
    let data = input;
    if (typeof input === 'string') {
      try {
        data = JSON.parse(input);
      } catch (e) {
        return { error: `${label} is not JSON (${e.message}). ${HOW_TO_WRITE}` };
      }
    }
    if (!isObject(data) || !('vehicle_sil_timeline' in data)) {
      return {
        error: `${label} is not a timeline file: it has no "vehicle_sil_timeline" key. ${HOW_TO_WRITE}`,
      };
    }
    if (data.vehicle_sil_timeline !== FORMAT) {
      const by =
        typeof data.vehicle_sil === 'string'
          ? ` (it was written by vehicle-sil ${data.vehicle_sil})`
          : '';
      return {
        error:
          `${label} is timeline format ${JSON.stringify(data.vehicle_sil_timeline)}${by}, and this page reads ` +
          `format ${FORMAT} only. Open it with the timeline page of the vehicle-sil version that wrote it.`,
      };
    }
    if (data.stage !== 'run') {
      return {
        error:
          `${label} has stage ${JSON.stringify(data.stage)}, and this page draws only "run" (the record of a run). ` +
          'Open it with the timeline page of the vehicle-sil version that wrote it.',
      };
    }
    if (!isObject(data.scenario)) {
      return {
        error: `${label} has no "scenario" object, so it is not a whole timeline file. ${HOW_TO_WRITE}`,
      };
    }
    for (const key of ['variables', 'steps', 'series']) {
      if (!Array.isArray(data[key])) {
        return {
          error: `${label} has no "${key}" array, so it is not a whole timeline file. ${HOW_TO_WRITE}`,
        };
      }
      const bad = data[key].findIndex((item) => !isObject(item));
      if (bad >= 0) {
        return {
          error: `${label}: ${key}[${bad}] is not an object, so the file cannot be read. ${HOW_TO_WRITE}`,
        };
      }
    }
    if (data.variables.some((v) => typeof v.name !== 'string')) {
      return {
        error: `${label}: a variable has no "name", so the file cannot be read. ${HOW_TO_WRITE}`,
      };
    }
    return { timeline: data };
  }

  // ---------------------------------------------------------------------------------------------------------------
  // Formatting

  function fmtNum(v, exact) {
    if (!isNum(v)) return String(v);
    if (exact || Number.isInteger(v)) return String(v);
    const s = String(v);
    return s.length <= 9 ? s : String(Number(v.toPrecision(6)));
  }

  function fmtMs(t) {
    return t === null || t === undefined ? '-' : `${fmtNum(t)} ms`;
  }

  /** A value as the launcher writes it: an enum with its item's name, a bool as true or false. */
  function fmtValue(v, info, exact) {
    if (v === null || v === undefined) return 'null';
    if (typeof v === 'boolean') return String(v);
    if (info && info.type === 'enum' && Array.isArray(info.items)) {
      const item = info.items.find((i) => i && i.value === v);
      if (item) return `${item.name} (${v})`;
    }
    return fmtNum(v, exact);
  }

  /** One condition's comparisons, the way the scenario writes them ("le 200", "near 200 ± 0.5"). */
  function compareText(compare, info) {
    if (!Array.isArray(compare)) return '';
    return compare
      .map((c) => {
        if (!isObject(c)) return '?';
        const tol =
          c.op === 'near' && c.tolerance !== undefined ? ` ± ${fmtNum(c.tolerance, true)}` : '';
        return `${c.op} ${fmtValue(c.value, info, true)}${tol}`;
      })
      .join(' and ');
  }

  // ---------------------------------------------------------------------------------------------------------------
  // The model: what the page draws, worked out once per file

  /** What a value of each type is, for the note on values that are not of their variable's type. */
  const TYPE_WORDS = {
    real: 'a number',
    integer: 'an integer',
    bool: 'true or false',
    enum: 'the value of an item (an integer)',
  };

  /** Whether a value fits its variable's type. null fits all (a real that is no number); a type not known takes all. */
  function fitsType(value, type) {
    if (value === null || !Object.prototype.hasOwnProperty.call(TYPE_WORDS, type)) return true;
    if (type === 'bool') return typeof value === 'boolean';
    if (type === 'real') return isNum(value);
    return Number.isInteger(value);
  }

  /**
   * Reads one series of a variable of type `type` into columns. Returns { series, notes } or { note } when the
   * page cannot draw it. A value that is not of the type becomes undefined (not drawn) and is named in the notes.
   */
  function readSeries(s, type) {
    const what = `the ${s.source} series`;
    if (!Object.prototype.hasOwnProperty.call(KNOWN.encoding, s.source)) {
      return { note: `${what} is not drawn: this page does not know the source "${s.source}".` };
    }
    if (s.encoding !== KNOWN.encoding[s.source]) {
      return {
        note: `${what} is not drawn: this page does not know the encoding "${s.encoding}".`,
      };
    }
    if (!Array.isArray(s.points))
      return { note: `${what} is not drawn: it has no "points" array.` };
    let ti = 0;
    let vi = 1;
    let bi = -1;
    if (Array.isArray(s.columns)) {
      ti = s.columns.indexOf('t_ms');
      vi = s.columns.indexOf('value');
      bi = s.columns.indexOf('base');
      if (ti < 0 || vi < 0) {
        return { note: `${what} is not drawn: its "columns" have no "t_ms" or "value".` };
      }
    }
    const t = [];
    const v = [];
    const base = bi >= 0 ? [] : null;
    const wrong = [];
    let rounded = 0;
    for (const p of s.points) {
      if (!Array.isArray(p) || !isNum(p[ti])) {
        return { note: `${what} is not drawn: a point is not [time, value].` };
      }
      t.push(p[ti]);
      const value = p[vi] === undefined ? null : p[vi];
      if (!fitsType(value, type)) {
        wrong.push([p[ti], value]);
        v.push(undefined);
      } else {
        if (type === 'integer' && value !== null && !Number.isSafeInteger(value)) rounded++;
        v.push(value);
      }
      if (base) base.push(p[bi] === undefined ? null : p[bi]);
    }
    const notes = [];
    if (wrong.length) {
      const [at, value] = wrong[0];
      notes.push(
        `${wrong.length} value${wrong.length === 1 ? '' : 's'} of ${what} ${wrong.length === 1 ? 'is' : 'are'} not ` +
          `${TYPE_WORDS[type]}, as its type ${type} says, so ${wrong.length === 1 ? 'it is' : 'they are'} not drawn ` +
          `(the first: ${JSON.stringify(value)} at ${fmtMs(at)}).`,
      );
    }
    if (rounded) {
      notes.push(
        `${rounded} value${rounded === 1 ? '' : 's'} of ${what} ${rounded === 1 ? 'is' : 'are'} beyond ` +
          `±${Number.MAX_SAFE_INTEGER}, which this page cannot hold exactly: shown rounded.`,
      );
    }
    return {
      notes,
      series: {
        source: s.source,
        t,
        v,
        base,
        /** For points of the failed step's writes: point index -> applied (true, false, or null: not known). */
        applied: null,
        notAppliedFrom: Number.isInteger(s.not_applied_from) ? s.not_applied_from : null,
        firstMs: isNum(s.first_ms) ? s.first_ms : null,
        lastMs: isNum(s.last_ms) ? s.last_ms : null,
        samples: Number.isInteger(s.samples) ? s.samples : null,
      },
    };
  }

  function participantOf(name) {
    const dot = name.indexOf('.');
    return dot > 0 ? name.slice(0, dot) : name;
  }

  /** The intervals in which a series lost points at the limits: { from, before, dropped } or null. */
  function gapOf(limits, variable, source) {
    if (!isObject(limits) || !Array.isArray(limits.dropped)) return null;
    const d = limits.dropped.find(
      (x) => isObject(x) && x.variable === variable && x.source === source,
    );
    if (!d || !isNum(d.dropped_from_ms) || !isNum(d.dropped_before_ms)) return null;
    return { from: d.dropped_from_ms, before: d.dropped_before_ms, dropped: d.dropped };
  }

  /**
   * The bands that conditions put on a variable's lane. A band covers the samples the condition looks at:
   * at: the instant from_ms; always and during: from_ms to until_ms, but not past the failure of the step or the
   * end of the record (`recordedTo`), since no sample was looked at after them; once: from_ms to the time it held
   * (passed) or to the limit, with a thin line on to the limit. `until` is the end the scenario gave.
   */
  function watchBands(step, variable, recordedTo) {
    const w = step.watch;
    if (!isObject(w) || !Array.isArray(w.conditions) || !isNum(w.from_ms))
      return { bands: [], notes: [] };
    const bands = [];
    const notes = [];
    const failedConditions =
      isObject(step.failed) && Array.isArray(step.failed.conditions)
        ? step.failed.conditions.map((c) => c && c.condition)
        : [];
    w.conditions.forEach((c, index) => {
      if (!isObject(c) || c.variable !== variable) return;
      if (!knows(KNOWN.mode, w.mode)) {
        notes.push(
          `the condition of step ${step.index} is not drawn: this page does not know the mode "${w.mode}".`,
        );
        return;
      }
      const compare = Array.isArray(c.compare) ? c.compare.filter(isObject) : [];
      const unknownOp = compare.find((x) => !knows(KNOWN.op, x.op));
      if (unknownOp) {
        notes.push(
          `the condition of step ${step.index} is not drawn: this page does not know "${unknownOp.op}".`,
        );
        return;
      }
      const from = w.from_ms;
      let to = isNum(w.until_ms) ? w.until_ms : from;
      let limit = null;
      const until = to;
      if (w.mode === 'once') {
        limit = isNum(w.until_ms) ? w.until_ms : null;
        if (step.status === 'passed' && isNum(step.at_ms)) to = step.at_ms;
        else if (step.status === 'failed' && isObject(step.failed) && isNum(step.failed.at_ms))
          to = step.failed.at_ms;
      } else if (w.mode === 'during' || w.mode === 'always') {
        if (step.status === 'failed' && isObject(step.failed) && isNum(step.failed.at_ms))
          to = Math.min(to, step.failed.at_ms);
        if (isNum(recordedTo)) to = Math.min(to, recordedTo);
      }
      bands.push({
        step: step.index,
        condition: index,
        mode: w.mode,
        from,
        to: Math.max(to, from),
        until: Math.max(until, from),
        limit,
        compare,
        status: knows(KNOWN.stepStatus, step.status) ? step.status : 'unknown',
        failed: failedConditions.includes(index),
      });
    });
    return { bands, notes };
  }

  /** The failure to lead the eye to: the failed step and what failed, in numbers and names. */
  function failureOf(tl, byName) {
    const step = tl.steps.find((s) => s.status === 'failed');
    if (!step) return null;
    const f = isObject(step.failed) ? step.failed : null;
    const conditions = (f && Array.isArray(f.conditions) ? f.conditions : [])
      .filter(isObject)
      .map((c) => {
        const wc =
          isObject(step.watch) && Array.isArray(step.watch.conditions)
            ? step.watch.conditions[c.condition]
            : null;
        const variable = isObject(wc) ? wc.variable : null;
        const info = byName.get(variable);
        return {
          variable,
          compare: isObject(wc) ? compareText(wc.compare, info && info.info) : null,
          value: c.value,
          valueText:
            c.value === null || c.value === undefined
              ? null
              : fmtValue(c.value, info && info.info, true),
          sampleMs: isNum(c.sample_ms) ? c.sample_ms : null,
          mode: isObject(step.watch) ? step.watch.mode : null,
        };
      });
    const writes = (f && Array.isArray(f.writes) ? f.writes : []).map((w, i) => {
      if (!isObject(w)) return null;
      const info = byName.get(w.variable);
      return {
        variable: w.variable,
        at: isNum(w.at_ms) ? w.at_ms : null,
        value: w.value,
        valueText:
          w.value === null || w.value === undefined
            ? 'not computed'
            : fmtValue(w.value, info && info.info, true),
        point: Number.isInteger(w.point) ? w.point : null,
        applied: w.applied === true ? true : w.applied === false ? false : null,
        cause: f.write === i,
      };
    });
    const known = writes.filter(Boolean);
    const at = f && isNum(f.at_ms) ? f.at_ms : isNum(step.at_ms) ? step.at_ms : null;
    return {
      step,
      at,
      code: (f && f.code) || step.code || null,
      message: typeof step.message === 'string' ? step.message : null,
      conditions,
      writes: known,
    };
  }

  /** Builds everything the page draws from a file that readTimeline() accepted. */
  function buildModel(tl) {
    const sc = tl.scenario;
    const recordedTo = isNum(sc.recorded_to_ms) ? sc.recorded_to_ms : null;
    const tick = isNum(tl.tick_ms) && tl.tick_ms > 0 ? tl.tick_ms : null;
    const vars = [];
    const byName = new Map();
    const addVar = (info) => {
      const v = {
        name: info.name,
        participant:
          typeof info.participant === 'string' ? info.participant : participantOf(info.name),
        info,
        written: null,
        sampled: null,
        notes: [],
        bands: [],
        lines: [],
        marks: [],
        gaps: {},
        failing: false,
      };
      vars.push(v);
      byName.set(v.name, v);
      return v;
    };
    tl.variables.forEach(addVar);
    const varOf = (name) =>
      byName.get(name) || addVar({ name, type: 'unknown', written: false, watched: false });

    for (const s of tl.series) {
      if (typeof s.variable !== 'string') continue;
      const v = varOf(s.variable);
      const read = readSeries(s, v.info.type);
      if (read.note) {
        v.notes.push(read.note);
        continue;
      }
      if (read.series.source === 'written' && !v.written) v.written = read.series;
      else if (read.series.source === 'sampled' && !v.sampled) v.sampled = read.series;
      else continue;
      v.notes.push(...read.notes);
      const gap = gapOf(tl.limits, s.variable, s.source);
      if (gap) v.gaps[s.source] = gap;
    }
    for (const v of vars) {
      if (!knows(KNOWN.type, v.info.type) && (v.written || v.sampled)) {
        v.notes.push(
          `its type "${v.info.type}" is not known to this page; the values are drawn as numbers.`,
        );
      }
    }

    const lines = (Array.isArray(tl.lines) ? tl.lines : [])
      .map((line, index) => (isObject(line) ? { ...line, index } : null))
      .filter(Boolean);
    for (const line of lines)
      if (typeof line.variable === 'string') varOf(line.variable).lines.push(line);
    // A dither lasts until the next dither line of the same variable.
    for (const v of vars) {
      const dithers = v.lines.filter((l) => l.kind === 'dither');
      dithers.forEach((l, i) => {
        l.until = i + 1 < dithers.length ? dithers[i + 1].from_ms : null;
      });
    }

    for (const step of tl.steps) {
      if (!isObject(step.watch) || !Array.isArray(step.watch.conditions)) continue;
      const names = new Set(step.watch.conditions.filter(isObject).map((c) => c.variable));
      for (const name of names) {
        if (typeof name !== 'string') continue;
        const v = varOf(name);
        const { bands, notes } = watchBands(step, name, recordedTo);
        v.bands.push(...bands);
        v.notes.push(...notes);
      }
    }

    const failure = failureOf(tl, byName);
    if (failure) {
      for (const c of failure.conditions) {
        if (typeof c.variable !== 'string') continue;
        const v = varOf(c.variable);
        v.failing = true;
        v.marks.push({
          kind: 'condition',
          at: c.sampleMs ?? failure.at,
          value: c.value,
          text: c.valueText,
        });
      }
      for (const w of failure.writes) {
        if (typeof w.variable !== 'string') continue;
        const v = varOf(w.variable);
        if (w.cause || w.applied !== true) v.failing = true;
        if (v.written && w.point !== null) {
          if (!v.written.applied) v.written.applied = new Map();
          v.written.applied.set(w.point, w.applied);
        }
        v.marks.push({
          kind: 'write',
          at: w.at ?? failure.at,
          value: w.value,
          applied: w.applied,
          sent: w.point !== null,
          cause: w.cause,
        });
      }
    }

    // The time range of the whole run: the record, and the steps and lines that the run decided.
    let end = recordedTo ?? 0;
    for (const step of tl.steps) {
      if (step.status === 'not_run') continue;
      for (const t of [step.start_ms, step.at_ms, step.end_ms])
        if (isNum(t)) end = Math.max(end, t);
    }
    for (const l of lines)
      for (const t of [l.from_ms, l.to_ms]) if (isNum(t)) end = Math.max(end, t);
    for (const v of vars) {
      for (const s of [v.written, v.sampled])
        if (s && s.t.length) end = Math.max(end, s.t[s.t.length - 1]);
    }
    const margin = Math.max(tick ?? 0, end * 0.04, end > 0 ? 0 : 10);
    const domain = [0, end + margin];

    const inputs = vars.filter((v) => v.info.written === true || (v.written && !v.info.watched));
    const others = vars.filter((v) => !inputs.includes(v));
    const findings = Array.isArray(tl.findings) ? tl.findings.filter(isObject) : [];
    const dropped =
      isObject(tl.limits) && Array.isArray(tl.limits.dropped)
        ? tl.limits.dropped.filter(isObject)
        : [];
    return {
      tl,
      name: typeof sc.name === 'string' && sc.name !== '' ? sc.name : null,
      status: sc.status,
      recordedTo,
      overAt: isNum(sc.over_at_ms) ? sc.over_at_ms : null,
      tick,
      steps: tl.steps,
      lines,
      vars,
      byName,
      inputs,
      others,
      failure,
      findings,
      dropped,
      late: findings.some((f) => f.code === 'control-link-late-values'),
      domain,
    };
  }

  // ---------------------------------------------------------------------------------------------------------------
  // Layout: time to position, rows of the step lane, the segments of a held series, thinning out

  /** Maps virtual time [t0, t1] to x in [0, width]. */
  function makeScale(t0, t1, width) {
    const k = width / (t1 - t0 || 1);
    return { t0, t1, width, x: (t) => (t - t0) * k, t: (x) => t0 + x / k };
  }

  /** The view when the page opens: the failure (a little before the failed step to the end of the record), or all. */
  function initialView(model) {
    const [d0, d1] = model.domain;
    const f = model.failure;
    if (!f || !isNum(f.at)) return [d0, d1];
    const start = isNum(f.step.start_ms) ? Math.min(f.step.start_ms, f.at) : f.at;
    const right = Math.max(f.at, model.recordedTo ?? f.at);
    const span = Math.max(right - start, (d1 - d0) * 0.2, (model.tick ?? 1) * 4);
    const t0 = Math.max(d0, start - span * 0.3);
    const t1 = Math.min(d1, right + span * 0.15);
    return [t0, Math.max(t1, t0 + 1e-9)];
  }

  /** Round tick values for an axis, about `count` of them. */
  function niceTicks(t0, t1, count) {
    const span = t1 - t0;
    if (!(span > 0)) return [t0];
    const raw = span / Math.max(1, count);
    const mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const step = [1, 2, 5, 10].map((m) => m * mag).find((s) => s >= raw) ?? 10 * mag;
    const out = [];
    for (let t = Math.ceil(t0 / step) * step; t <= t1 + step * 1e-9; t += step)
      out.push(Number(t.toPrecision(12)));
    return out;
  }

  /**
   * Puts intervals (in pixels: { x0, x1 }) on rows so that none overlap: the first row with room, in order.
   * Returns { rows: [row index per item], count }.
   */
  function packRows(items, gap) {
    const ends = [];
    const rows = items.map((it) => {
      let r = ends.findIndex((e) => e + gap <= it.x0);
      if (r < 0) {
        r = ends.length;
        ends.push(-Infinity);
      }
      ends[r] = it.x1;
      return r;
    });
    return { rows, count: Math.max(1, ends.length) };
  }

  /** Whether a state is of a point that was sent and is not known to be in the ECU. */
  const unapplied = (state) => state === 'not-applied' || state === 'not-known';

  /**
   * What is known of point i of a series. sampled: 'seen'. written: 'sent' before not_applied_from; from it on
   * 'not-applied' where the failed step's writes say the ECU did not apply it (applied false), and 'not-known'
   * otherwise (applied null, or a point sent after them: the schema says these are not known to have been applied).
   */
  function pointState(series, i) {
    if (series.source === 'sampled') return 'seen';
    const naf = series.notAppliedFrom;
    if (naf === null || i < naf) return 'sent';
    const applied = series.applied ? series.applied.get(i) : undefined;
    return applied === false ? 'not-applied' : applied === true ? 'sent' : 'not-known';
  }

  /**
   * The value the ECU kept from the first point of not_applied_from on, while it is known: up to the first point
   * that is not 'not-applied'. Returns { from, to, v } or null (no such point, or not known from the start).
   */
  function keptValue(series, end) {
    const naf = series.notAppliedFrom;
    const n = series.t.length;
    if (naf === null || naf < 1 || naf >= n || pointState(series, naf) !== 'not-applied')
      return null;
    let to = Math.max(end ?? series.t[n - 1], series.t[n - 1]);
    for (let i = naf + 1; i < n; i++) {
      if (pointState(series, i) !== 'not-applied') {
        to = series.t[i];
        break;
      }
    }
    return { from: series.t[naf], to, v: series.v[naf - 1] };
  }

  /**
   * The segments of a series held from each point to the next: { from, to, v, base, state }.
   * state: 'sent' (or 'seen'); 'not-applied' and 'not-known' (from not_applied_from on, see pointState); 'held'
   * (the value that stayed in the ECU after a write it did not apply, while that is known); 'alone' (a point
   * inside an interval whose points were dropped: not held). `end` is where the last point stops (the end of the
   * record, or last_ms of a sampled series). v is undefined for a value that is not of the variable's type.
   */
  function holdSegments(series, end, gap) {
    const segs = [];
    const n = series.t.length;
    const naf = series.notAppliedFrom;
    for (let i = 0; i < n; i++) {
      const from = series.t[i];
      let to = i + 1 < n ? series.t[i + 1] : Math.max(end ?? from, from);
      let state = pointState(series, i);
      if (gap) {
        if (from >= gap.from && from < gap.before) {
          to = from;
          if (!unapplied(state)) state = 'alone';
        } else if (from < gap.from && to > gap.from) {
          to = gap.from;
        }
      }
      if (naf !== null && i === naf - 1) to = Math.min(to, series.t[naf]);
      segs.push({
        from,
        to,
        v: series.v[i],
        base: series.base ? series.base[i] : undefined,
        state,
      });
    }
    const kept = series.source === 'sampled' ? null : keptValue(series, end);
    if (kept) segs.push({ ...kept, base: undefined, state: 'held' });
    return segs;
  }

  /**
   * Where each segment of a bool or enum band goes, in the order drawn: { seg, y, height }. The value kept in the
   * ECU ('held') is a strip under the row, drawn first, so that it never covers a write that was not applied or
   * whose result is not known (the row above it is shorter by the strip).
   */
  function stateBandLayout(segs, y0, height) {
    const strip = 5;
    const held = segs.filter((seg) => seg.state === 'held');
    const rowHeight = held.length ? height - strip - 1 : height;
    return [
      ...held.map((seg) => ({ seg, y: y0 + height - strip, height: strip })),
      ...segs
        .filter((seg) => seg.state !== 'held')
        .map((seg) => ({ seg, y: y0, height: rowHeight })),
    ];
  }

  /**
   * Thins out the segments in view [t0, t1] to about 2 per pixel column: segments that start in the same column
   * become one with the column's min and max (drawn as a vertical stroke) and its last value. Segments of
   * another state or with a value that is not a number are kept as they are.
   */
  function decimate(segs, scale) {
    const visible = segs.filter((s) => s.to >= scale.t0 && s.from <= scale.t1);
    if (visible.length <= 2 * Math.max(1, scale.width)) {
      return visible.map((s) => ({ ...s, min: s.v, max: s.v }));
    }
    const out = [];
    let cur = null;
    let col = null;
    for (const s of visible) {
      const c = Math.floor(scale.x(s.from));
      const numeric = isNum(s.v);
      if (cur && numeric && c === col && s.state === cur.state && isNum(cur.v)) {
        cur.to = s.to;
        cur.min = Math.min(cur.min, s.v);
        cur.max = Math.max(cur.max, s.v);
        cur.v = s.v;
        cur.base = s.base;
        continue;
      }
      cur = { ...s, min: s.v, max: s.v };
      col = c;
      out.push(cur);
    }
    return out;
  }

  /**
   * The value of a series at time t, or null where it has none:
   *   { value }                                a value sent or seen (undefined: not of the variable's type);
   *   { missing: true }                        inside the interval `gap` whose points were dropped at the limits;
   *   { value, state: 'not-applied', kept }    sent, and the ECU did not apply it. kept: { value } that the ECU
   *                                            has (value undefined: from before the record), or null: not known;
   *   { value, state: 'not-known' }            sent, and whether the ECU applied it is not known.
   */
  function valueAt(series, t, end, gap) {
    if (!series || !series.t.length || t < series.t[0]) return null;
    let lo = 0;
    let hi = series.t.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (series.t[mid] <= t) lo = mid;
      else hi = mid - 1;
    }
    // Where the drawing stops: a sampled value at last_ms; a sent one at the end, or at its last point after it.
    const last = series.t[series.t.length - 1];
    const limit =
      series.source === 'sampled' ? (series.lastMs ?? end) : isNum(end) ? Math.max(end, last) : end;
    if (isNum(limit) && t > limit) return null;
    if (gap && t >= gap.from && t < gap.before && series.t[lo] !== t) return { missing: true };
    const state = pointState(series, lo);
    if (state === 'not-known') return { value: series.v[lo], state };
    if (state === 'not-applied') {
      // The ECU has the value before not_applied_from only while every point since was not applied.
      const naf = series.notAppliedFrom;
      let known = true;
      for (let i = naf; i <= lo && known; i++) known = pointState(series, i) === 'not-applied';
      return {
        value: series.v[lo],
        state,
        kept: !known ? null : { value: naf > 0 ? series.v[naf - 1] : undefined },
      };
    }
    return { value: series.v[lo] };
  }

  /** The words of the cursor's read-out for one series ("sent" or "seen") at a time: see valueAt(). */
  function readoutText(word, at, info) {
    if (at.missing) return `${word}: values dropped at the file's limits`;
    if (at.value === undefined) return `${word}: a value that is not of the type (see the note)`;
    const value = `${word} ${fmtValue(at.value, info)}`;
    if (at.state === 'not-known') return `${value} (not known whether the ECU applied it)`;
    if (at.state !== 'not-applied') return value;
    const has =
      at.kept === null
        ? 'a value that is not known'
        : at.kept.value === undefined
          ? 'its earlier value'
          : fmtValue(at.kept.value, info);
    return `${value} (not applied; the ECU has ${has})`;
  }

  /**
   * The words of the table of a failed step's writes, from what the file says of each write: applied, point (null:
   * not sent) and whether it is the cause. Returns { heading, explain, cause(write) }.
   */
  function writeWords(f) {
    const at = f.writes.length && f.writes[0].at !== null ? f.writes[0].at : f.at;
    const has = (applied) => f.writes.some((w) => w.applied === applied);
    const explain = [
      has(true) ? 'Applied: in the ECU, although the step failed.' : null,
      has(false)
        ? 'Not applied: the ECU refused it, or it came after the refused write in the same command, or it was not sent.'
        : null,
      has(null) ? 'Not known: no result says whether the ECU applied it.' : null,
    ]
      .filter(Boolean)
      .join(' ');
    const cause = (w) => {
      if (w.point === null) return w.cause ? 'not sent: the cause of the failure' : 'not sent';
      return w.cause ? 'the cause of the failure' : '';
    };
    return { heading: `Writes for ${fmtMs(at)}`, explain, cause };
  }

  /** The value range of a numeric lane: the declared min and max, or (fit) the values with some room. */
  function laneRange(v, fit) {
    const info = v.info;
    if (!fit && isNum(info.min) && isNum(info.max) && info.max > info.min)
      return [info.min, info.max];
    let lo = Infinity;
    let hi = -Infinity;
    const take = (x) => {
      if (isNum(x)) {
        lo = Math.min(lo, x);
        hi = Math.max(hi, x);
      }
    };
    for (const s of [v.written, v.sampled]) {
      if (!s) continue;
      s.v.forEach(take);
      if (s.base) s.base.forEach(take);
    }
    for (const b of v.bands) {
      for (const c of b.compare) {
        take(c.value);
        if (c.op === 'near' && isNum(c.tolerance)) {
          take(c.value - c.tolerance);
          take(c.value + c.tolerance);
        }
      }
    }
    for (const m of v.marks) take(m.value);
    if (lo === Infinity) return [0, 1];
    if (lo === hi) {
      const d = Math.abs(lo) * 0.1 || 1;
      return [lo - d, hi + d];
    }
    const pad = (hi - lo) * 0.08;
    return [lo - pad, hi + pad];
  }

  /**
   * Whether a numeric lane starts scaled to its values: when the values use less than a fifth of the declared
   * range (min to max), a lane in the declared range would show a flat line. The lane's button switches.
   */
  function defaultFit(v) {
    const info = v.info;
    if (!(isNum(info.min) && isNum(info.max) && info.max > info.min)) return true;
    const [lo, hi] = laneRange(v, true);
    return hi - lo < (info.max - info.min) / 5;
  }

  /** Joins neighbouring segments with the same value and state (a band shows each state once). */
  function mergeEqual(segs) {
    const out = [];
    for (const seg of segs) {
      const prev = out[out.length - 1];
      if (
        prev &&
        prev.state === seg.state &&
        prev.v === seg.v &&
        prev.to === seg.from &&
        seg.state !== 'alone'
      ) {
        prev.to = seg.to;
      } else {
        out.push({ ...seg });
      }
    }
    return out;
  }

  /** How a lane draws its variable. */
  function laneKind(v) {
    if (v.info.type === 'bool') return 'bool';
    if (v.info.type === 'enum') return 'enum';
    return 'number';
  }

  const api = {
    FORMAT,
    KNOWN,
    readTimeline,
    buildModel,
    makeScale,
    initialView,
    niceTicks,
    packRows,
    pointState,
    holdSegments,
    stateBandLayout,
    decimate,
    valueAt,
    readoutText,
    writeWords,
    laneRange,
    defaultFit,
    mergeEqual,
    laneKind,
    fmtValue,
    fmtMs,
    compareText,
  };

  if (typeof module === 'object' && module.exports) {
    module.exports = api;
    return;
  }
  global.VehicleSilTimeline = api;
  if (typeof document === 'undefined') return;

  // ---------------------------------------------------------------------------------------------------------------
  // The page

  const SVG = 'http://www.w3.org/2000/svg';
  const LANE_NUMBER_HEIGHT = 76;
  const BAND_HEIGHT = 24;
  const STRIP_HEIGHT = 16;
  const STEP_ROW = 22;

  /** An HTML element; children are elements or strings (set as text, never parsed). */
  function h(tag, attrs, ...children) {
    const e = document.createElement(tag);
    for (const [k, val] of Object.entries(attrs || {})) {
      if (val === null || val === undefined || val === false) continue;
      if (k === 'class') e.className = val;
      else if (k === 'text') e.textContent = val;
      else if (k.startsWith('on')) e.addEventListener(k.slice(2), val);
      else e.setAttribute(k, val === true ? '' : String(val));
    }
    for (const c of children.flat()) {
      if (c === null || c === undefined || c === false) continue;
      e.append(typeof c === 'string' ? document.createTextNode(c) : c);
    }
    return e;
  }

  /** An SVG element. */
  function s(tag, attrs, parent) {
    const e = document.createElementNS(SVG, tag);
    for (const [k, val] of Object.entries(attrs || {})) {
      if (val === null || val === undefined) continue;
      if (k === 'text') e.textContent = val;
      else e.setAttribute(k, String(val));
    }
    if (parent) parent.append(e);
    return e;
  }

  function tooltip(el, text) {
    s('title', { text }, el);
    return el;
  }

  const state = {
    model: null,
    view: null,
    fit: new Set(),
    hidden: new Set(),
    cursor: null,
    lanes: [],
    width: 0,
    /** The name of the file shown (null for data embedded in the page or none). */
    file: null,
  };

  const $ = (id) => document.getElementById(id);

  /** Says why a file cannot be shown. The page then shows no file, so that nothing on it is taken for this one. */
  function showError(message) {
    const box = $('error');
    box.replaceChildren(h('strong', { text: 'This file cannot be shown. ' }), message);
    box.hidden = false;
    state.model = null;
    state.file = null;
    state.lanes = [];
    $('view').hidden = true;
    $('load').hidden = false;
    $('title').textContent = 'Scenario timeline';
    $('facts').replaceChildren();
    document.title = 'Scenario timeline';
  }

  function clearError() {
    $('error').hidden = true;
  }

  function load(text, name) {
    const read = readTimeline(text, name);
    if (read.error) {
      showError(read.error);
      return;
    }
    clearError();
    const model = buildModel(read.timeline);
    state.model = model;
    state.view = initialView(model);
    state.fit.clear();
    state.hidden.clear();
    state.cursor = null;
    state.file = name || null;
    document.title = `${model.name ?? 'Scenario'} - timeline`;
    $('load').hidden = true;
    $('view').hidden = false;
    renderAll();
  }

  // --- the parts that do not change with the view

  function renderHeader(m) {
    const tl = m.tl;
    $('title').textContent = m.name ?? '(a scenario without a name)';
    const status = h('span', {
      class: `badge st-${statusClass(m.status)}`,
      text: String(m.status),
    });
    const facts = [
      ['timeline file', state.file],
      ['scenario', tl.scenario.file],
      ['configuration', tl.file],
      ['variant', tl.variant],
      ['controller time step', m.tick !== null ? fmtMs(m.tick) : null],
      ['recorded to', m.recordedTo !== null ? fmtMs(m.recordedTo) : 'nothing recorded'],
      ['vehicle-sil', tl.vehicle_sil],
    ].filter(([, v]) => v !== null && v !== undefined);
    const params = Array.isArray(tl.params) ? tl.params.filter(isObject) : [];
    $('facts').replaceChildren(
      status,
      ...facts.map(([k, v]) =>
        h('span', { class: 'fact' }, h('span', { class: 'muted', text: `${k} ` }), String(v)),
      ),
      ...params.map((p) =>
        h(
          'span',
          { class: 'fact' },
          h('span', { class: 'muted', text: 'param ' }),
          `${p.name} = ${fmtValue(p.value)}`,
          h('span', { class: 'muted', text: ` (${p.from})` }),
        ),
      ),
    );
  }

  function statusClass(st) {
    return knows(KNOWN.stepStatus, st) || st === 'incomplete' ? st : 'unknown';
  }

  function renderSummary(m) {
    const box = $('summary');
    const parts = [];
    const f = m.failure;
    if (f) {
      const step = f.step;
      const where = [step.kind, `line ${step.line} of ${m.tl.scenario.file}`, f.code]
        .filter(Boolean)
        .join(' · ');
      parts.push(
        h(
          'div',
          { class: 'banner bad', 'data-testid': 'failure' },
          h('h2', { text: `Step ${step.index} failed at ${fmtMs(f.at)}` }),
          h('p', { class: 'muted small', text: `${where} · ${step.text}` }),
          f.message
            ? h('p', { class: 'message', 'data-testid': 'failure-message', text: f.message })
            : null,
          f.conditions.length ? renderConditions(f) : null,
          f.writes.length ? renderWrites(f) : null,
        ),
      );
    } else if (m.status === 'passed') {
      const n = m.steps.length;
      const skipped = m.steps.filter((x) => x.status === 'skipped').length;
      parts.push(
        h(
          'div',
          { class: 'banner ok', 'data-testid': 'passed' },
          h('h2', { text: `Passed: ${n} step${n === 1 ? '' : 's'}` }),
          skipped ? h('p', { text: `${skipped} skipped (see the table of steps).` }) : null,
        ),
      );
    } else {
      const text =
        m.status === 'not_run'
          ? 'The scenario did not run, so there is nothing to draw. The findings below say why.'
          : m.status === 'incomplete'
            ? 'The scenario ended partway (incomplete): the run stopped before the scenario was decided. The findings below say why.'
            : `The scenario ended with status "${m.status}", which this page does not know. The steps and findings below show what the file says.`;
      parts.push(
        h(
          'div',
          { class: 'banner warn', 'data-testid': 'not-decided' },
          h('h2', { text: `Scenario ${m.status}` }),
          h('p', { text }),
          ...m.findings
            .filter((x) => x.severity === 'error' && typeof x.message === 'string')
            .map((x) => h('p', { class: 'message', text: `${x.code}: ${x.message}` })),
        ),
      );
    }
    if (m.dropped.length) {
      parts.push(
        h(
          'div',
          { class: 'banner warn small' },
          h('strong', { text: 'Points are missing. ' }),
          'The launcher dropped older points at the limits of the file, so these values are missing (shown as a shaded box with a dashed border): ',
          m.dropped
            .map(
              (d) =>
                `${d.variable} (${d.source}): ${d.dropped} points from ${fmtMs(d.dropped_from_ms)} to ${fmtMs(d.dropped_before_ms)}`,
            )
            .join('; '),
          '.',
        ),
      );
    }
    if (m.late) {
      parts.push(
        h(
          'div',
          { class: 'banner warn small' },
          h('strong', { text: 'ECU values arrived late in this run ' }),
          '(control-link-late-values). The value series of another run of the same scenario can differ.',
        ),
      );
    }
    box.replaceChildren(...parts);
  }

  function renderConditions(f) {
    return h(
      'div',
      {},
      h('h3', { text: 'Not met' }),
      h(
        'ul',
        { 'data-testid': 'failed-conditions' },
        ...f.conditions.map((c) => {
          const what = `${c.variable ?? '?'} ${c.compare ?? ''}`.trim();
          let got;
          if (c.valueText !== null)
            got = `it was ${c.valueText}${c.sampleMs !== null ? ` (sample of ${fmtMs(c.sampleMs)})` : ''}`;
          else if (c.mode === 'during') got = 'no sample came in the interval';
          else got = 'it had no value yet';
          return h('li', {}, h('code', { text: what }), `: ${got}`);
        }),
      ),
    );
  }

  function renderWrites(f) {
    const words = writeWords(f);
    const label = (w) =>
      w.applied === true ? 'applied' : w.applied === false ? 'not applied' : 'not known';
    return h(
      'div',
      {},
      h('h3', { text: words.heading }),
      h('p', { class: 'muted small', 'data-testid': 'failed-writes-explained' }, words.explain),
      h(
        'table',
        { class: 'writes', 'data-testid': 'failed-writes' },
        h(
          'thead',
          {},
          h(
            'tr',
            {},
            ...['Variable', 'Value', 'In the ECU', 'Cause'].map((x) =>
              h('th', { scope: 'col', text: x }),
            ),
          ),
        ),
        h(
          'tbody',
          {},
          ...f.writes.map((w) =>
            h(
              'tr',
              { class: w.cause ? 'cause' : null },
              h('td', {}, h('code', { text: w.variable })),
              h('td', { text: w.valueText }),
              h(
                'td',
                { class: `applied-${String(w.applied)}` },
                appliedIcon(w.applied),
                ` ${label(w)}`,
              ),
              h('td', { text: words.cause(w) }),
            ),
          ),
        ),
      ),
    );
  }

  function appliedIcon(applied) {
    return h('span', {
      class: 'icon',
      'aria-hidden': 'true',
      text: applied === true ? '✓' : applied === false ? '✗' : '?',
    });
  }

  function renderParticipants(m) {
    const names = [...new Set(m.vars.map((v) => v.participant))];
    const box = $('participants');
    if (names.length < 2) {
      box.replaceChildren();
      return;
    }
    box.replaceChildren(
      h('span', { class: 'muted', text: 'Participants ' }),
      ...names.map((p) =>
        h(
          'label',
          { class: 'chip' },
          h('input', {
            type: 'checkbox',
            checked: !state.hidden.has(p),
            onchange: (e) => {
              if (e.target.checked) state.hidden.delete(p);
              else state.hidden.add(p);
              renderChart();
            },
          }),
          ` ${p}`,
        ),
      ),
    );
  }

  // --- the tables (the same contents as the chart)

  function renderTables(m) {
    const stepRows = m.steps.map((st) => {
      const result =
        st.message ??
        (typeof st.expect_skipped === 'string' ? `expect not checked: ${st.expect_skipped}` : '');
      const canShow = isNum(st.start_ms);
      return h(
        'tr',
        { class: st.status === 'failed' ? 'failed-row' : null },
        h(
          'td',
          {},
          canShow
            ? h('button', {
                type: 'button',
                class: 'link',
                title: 'Show this step on the chart',
                onclick: () => showStep(st),
                text: `#${st.index}`,
              })
            : `#${st.index}`,
        ),
        h('td', { text: String(st.line ?? '') }),
        h('td', { text: String(st.kind) }),
        h(
          'td',
          {},
          h('span', { class: `badge st-${statusClass(st.status)}`, text: String(st.status) }),
        ),
        h('td', { class: 'num', text: fmtMs(st.start_ms) }),
        h('td', { class: 'num', text: fmtMs(st.at_ms) }),
        h('td', { class: 'num', text: fmtMs(st.end_ms) }),
        h('td', { text: String(st.text ?? '') }),
        h('td', { text: result }),
      );
    });
    const lineRows = m.lines.map((l) =>
      h(
        'tr',
        {},
        h('td', { text: String(l.index) }),
        h('td', {}, h('code', { text: String(l.variable) })),
        h('td', { text: String(l.kind) }),
        h('td', { text: l.step === null || l.step === undefined ? 'inputs' : `#${l.step}` }),
        h('td', { class: 'num', text: fmtMs(l.from_ms) }),
        h('td', { class: 'num', text: fmtMs(l.to_ms) }),
        h('td', {
          text:
            l.base === null || l.base === undefined
              ? ''
              : `${fmtValue(l.base, m.byName.get(l.variable)?.info, true)} (${l.base_source})`,
        }),
        h('td', { text: String(l.text ?? '') }),
      ),
    );
    const findingRows = m.findings.map((x) =>
      h(
        'tr',
        {},
        h('td', { text: String(x.severity ?? '') }),
        h('td', {}, h('code', { text: String(x.code) })),
        h('td', { text: x.line === null || x.line === undefined ? '' : String(x.line) }),
        h('td', { text: String(x.message ?? '(code only)') }),
      ),
    );
    const table = (caption, heads, rows, testid) =>
      h(
        'details',
        { open: true },
        h('summary', { text: `${caption} (${rows.length})` }),
        rows.length
          ? h(
              'div',
              { class: 'table-wrap' },
              h(
                'table',
                { 'data-testid': testid },
                h(
                  'thead',
                  {},
                  h('tr', {}, ...heads.map((x) => h('th', { scope: 'col', text: x }))),
                ),
                h('tbody', {}, ...rows),
              ),
            )
          : h('p', { class: 'muted', text: 'None.' }),
      );
    $('tables').replaceChildren(
      table(
        'Steps',
        ['#', 'Line', 'Kind', 'Status', 'Start', 'Decided', 'End', 'Step', 'Result'],
        stepRows,
        'steps-table',
      ),
      table(
        'Input lines',
        ['#', 'Variable', 'Kind', 'Step', 'From', 'To', 'Base', 'Text'],
        lineRows,
        'lines-table',
      ),
      table('Findings', ['Severity', 'Code', 'Line', 'Message'], findingRows, 'findings-table'),
    );
  }

  function showStep(st) {
    const from = st.start_ms;
    const ends = [st.end_ms, st.at_ms, isObject(st.watch) ? st.watch.until_ms : null].filter(isNum);
    const to = Math.max(from, ...ends);
    const pad = Math.max((to - from) * 0.25, (state.model.tick ?? 1) * 3);
    setView([
      Math.max(state.model.domain[0], from - pad),
      Math.min(state.model.domain[1], to + pad),
    ]);
    $('chart').scrollIntoView({ block: 'start' });
  }

  // --- the chart

  function setView(view) {
    const [d0, d1] = state.model.domain;
    let [t0, t1] = view;
    t0 = Math.max(d0, t0);
    t1 = Math.min(d1, t1);
    if (!(t1 > t0)) return;
    state.view = [t0, t1];
    renderChart();
  }

  function renderAll() {
    const m = state.model;
    renderHeader(m);
    renderSummary(m);
    renderParticipants(m);
    renderTables(m);
    renderChart();
  }

  function plotWidth() {
    const chart = $('chart');
    const head = chart.clientWidth >= 720 ? 200 : 0;
    return Math.max(160, chart.clientWidth - head - 2);
  }

  function renderChart() {
    const m = state.model;
    if (!m) return;
    const width = plotWidth();
    state.width = width;
    const scale = makeScale(state.view[0], state.view[1], width);
    state.lanes = [];
    const chart = $('chart');
    const children = [axisLane(scale), stepLane(m, scale)];
    const visible = (v) => !state.hidden.has(v.participant);
    const inputs = m.inputs.filter(visible);
    const others = m.others.filter(visible);
    if (inputs.length) {
      children.push(h('div', { class: 'group', text: 'Inputs: what the scenario sent (and saw)' }));
      children.push(...inputs.map((v) => variableLane(m, v, scale)));
    }
    if (others.length) {
      children.push(
        h('div', { class: 'group', text: 'Outputs and other variables: what the scenario saw' }),
      );
      children.push(...others.map((v) => variableLane(m, v, scale)));
    }
    chart.replaceChildren(...children);
    $('range').textContent =
      `Showing ${fmtMs(scale.t0)} to ${fmtMs(scale.t1)} of ${fmtMs(m.domain[0])} to ${fmtMs(m.domain[1])}`;
    $('show-failure').hidden = !m.failure;
    updateCursor();
  }

  function laneShell(head, height, scale, cls) {
    const svg = s('svg', {
      class: 'plot',
      width: scale.width,
      height,
      viewBox: `0 0 ${scale.width} ${height}`,
      role: 'img',
    });
    const lane = h(
      'div',
      { class: `lane ${cls || ''}` },
      h('div', { class: 'lane-head' }, head),
      h('div', { class: 'lane-plot' }, svg),
    );
    const cursor = s('line', { class: 'cursor', x1: -10, x2: -10, y1: 0, y2: height }, null);
    state.lanes.push({ svg, cursor, height, readout: null });
    attachPointer(svg, scale, height);
    return { lane, svg, cursor };
  }

  /** The marks every lane has: the part after the record, the failure time. Drawn before the data. */
  function commonBack(svg, m, scale, height) {
    if (m.recordedTo !== null && m.recordedTo < scale.t1) {
      const x = Math.max(0, scale.x(m.recordedTo));
      s('rect', { class: 'unrecorded', x, y: 0, width: scale.width - x, height }, svg);
    }
    if (m.recordedTo === null)
      s('rect', { class: 'unrecorded', x: 0, y: 0, width: scale.width, height }, svg);
  }

  function commonFront(svg, m, scale, height, cursor) {
    if (m.failure && isNum(m.failure.at) && m.failure.at >= scale.t0 && m.failure.at <= scale.t1) {
      const x = scale.x(m.failure.at);
      s('line', { class: 'fail-line', x1: x, x2: x, y1: 0, y2: height }, svg);
    }
    svg.append(cursor);
  }

  function axisLane(scale) {
    const height = 22;
    const { lane, svg, cursor } = laneShell(
      h('span', {
        class: 'muted small',
        id: 'cursor-time',
        'aria-live': 'polite',
        text: 'Virtual time',
      }),
      height,
      scale,
      'axis',
    );
    commonBack(svg, state.model, scale, height);
    const ticks = niceTicks(scale.t0, scale.t1, Math.max(2, Math.floor(scale.width / 90)));
    for (const t of ticks) {
      const x = scale.x(t);
      s('line', { class: 'tick', x1: x, x2: x, y1: height - 6, y2: height }, svg);
      s('text', { class: 'tick-label', x: x + 3, y: height - 8, text: fmtMs(t) }, svg);
    }
    svg.append(cursor);
    return lane;
  }

  function stepLane(m, scale) {
    const items = [];
    for (const st of m.steps) {
      if (!isNum(st.start_ms)) continue;
      const start = st.start_ms;
      let end = [st.end_ms, st.at_ms].filter(isNum).reduce((a, b) => Math.max(a, b), start);
      const open = st.status === 'unfinished';
      if (open) end = m.domain[1];
      const limit =
        isObject(st.watch) &&
        st.watch.mode === 'once' &&
        isNum(st.watch.until_ms) &&
        st.watch.until_ms > end
          ? st.watch.until_ms
          : null;
      if ((limit ?? end) < scale.t0 || start > scale.t1) continue;
      const label = `#${st.index} ${st.kind}`;
      const labelWidth = label.length * 6.6 + 12;
      const xs = Math.max(0, scale.x(start));
      const xe = Math.min(scale.width, scale.x(limit ?? end));
      // A label that would run off the right edge goes to the left of its step.
      const left = xe - xs < labelWidth && xs + labelWidth > scale.width;
      const x0 = left ? xs - labelWidth : xs;
      const x1 = left ? Math.max(xe, xs + 7) : Math.max(xe, xs + labelWidth);
      items.push({ st, start, end, limit, open, label, left, x0, x1 });
    }
    const { rows, count } = packRows(items, 6);
    const height = count * STEP_ROW + 6;
    const head = h(
      'div',
      {},
      h('strong', { text: 'Steps' }),
      h('div', { class: 'readout small', id: 'cursor-steps' }),
    );
    const { lane, svg, cursor } = laneShell(head, height, scale, 'steps');
    commonBack(svg, m, scale, height);
    items.forEach((it, i) => {
      const y = rows[i] * STEP_ROW + 4;
      const bh = STEP_ROW - 6;
      const g = s('g', { class: `step st-${statusClass(it.st.status)}` }, svg);
      const x0 = scale.x(it.start);
      const x1 = scale.x(it.end);
      const point = x1 - x0 < 2;
      if (it.st.status === 'skipped') {
        s('rect', { class: 'skipped', x: x0 - 5, y, width: 10, height: bh }, g);
      } else if (point) {
        s(
          'path',
          { class: 'marker', d: `M${x0} ${y} l6 ${bh / 2} l-6 ${bh / 2} l-6 ${-bh / 2} z` },
          g,
        );
      } else {
        s('rect', { class: 'bar', x: x0, y, width: x1 - x0, height: bh, rx: 3 }, g);
        if (it.open)
          s(
            'path',
            { class: 'open', d: `M${x1 - 7} ${y - 1} l7 ${bh / 2 + 1} l-7 ${bh / 2 + 1}` },
            g,
          );
      }
      if (isNum(it.st.at_ms) && it.st.at_ms !== it.start && it.st.status !== 'unfinished') {
        const xa = scale.x(it.st.at_ms);
        s('line', { class: 'decided', x1: xa, x2: xa, y1: y - 2, y2: y + bh + 2 }, g);
      }
      if (it.limit !== null) {
        const xl = scale.x(it.limit);
        s('line', { class: 'limit', x1, x2: xl, y1: y + bh / 2, y2: y + bh / 2 }, g);
        s('line', { class: 'limit', x1: xl, x2: xl, y1: y + 2, y2: y + bh - 2 }, g);
      }
      if (it.left) {
        s(
          'text',
          { class: 'step-label', x: x0 - 8, y: y + bh - 4, 'text-anchor': 'end', text: it.label },
          g,
        );
      } else {
        s(
          'text',
          {
            class: 'step-label',
            x: Math.max(x0, 0) + (point ? 8 : 5),
            y: y + bh - 4,
            text: it.label,
          },
          g,
        );
      }
      const lines = [
        `Step ${it.st.index} (${it.st.kind}, line ${it.st.line}): ${it.st.status}`,
        String(it.st.text ?? ''),
        `start ${fmtMs(it.st.start_ms)}, decided ${fmtMs(it.st.at_ms)}, end ${fmtMs(it.st.end_ms)}`,
      ];
      if (it.st.message) lines.push(it.st.message);
      if (typeof it.st.expect_skipped === 'string')
        lines.push(`expect not checked: ${it.st.expect_skipped}`);
      tooltip(g, lines.join('\n'));
    });
    commonFront(svg, m, scale, height, cursor);
    return lane;
  }

  function variableLane(m, v, scale) {
    const kind = laneKind(v);
    const both = v.written && v.sampled;
    const strip = v.lines.length ? STRIP_HEIGHT : 0;
    const bandCount = both ? 2 : 1;
    const plotH = kind === 'number' ? LANE_NUMBER_HEIGHT : BAND_HEIGHT * bandCount + 8;
    const height = strip + plotH;
    const info = v.info;
    const range =
      isNum(info.min) && isNum(info.max) ? `${fmtNum(info.min)} to ${fmtNum(info.max)}` : null;
    const meta = [
      info.causality ?? null,
      info.type,
      info.unit || null,
      range,
      isNum(info.ecu_step_ms) ? `step ${fmtMs(info.ecu_step_ms)}` : null,
    ]
      .filter(Boolean)
      .join(' · ');
    const fit = defaultFit(v) !== state.fit.has(v.name);
    const readout = h('div', { class: 'readout small' });
    const writtenEnd =
      m.recordedTo ??
      (v.written && v.written.t.length ? v.written.t[v.written.t.length - 1] : null);
    // The legend names the states of the sent values that the lane has.
    const writtenStates = new Set();
    if (v.written && v.written.notAppliedFrom !== null) {
      for (let i = v.written.notAppliedFrom; i < v.written.t.length; i++)
        writtenStates.add(pointState(v.written, i));
      if (keptValue(v.written, writtenEnd)) writtenStates.add('held');
    }
    const head = h(
      'div',
      {},
      h('code', { class: `var-name${v.failing ? ' failing' : ''}`, text: v.name }),
      h('div', { class: 'muted small', text: meta }),
      h(
        'div',
        { class: 'legend small' },
        v.written
          ? h('span', { class: 'key sent' }, both && kind !== 'number' ? 'sent (upper)' : 'sent')
          : null,
        v.sampled
          ? h('span', { class: 'key seen' }, both && kind !== 'number' ? 'seen (lower)' : 'seen')
          : null,
        writtenStates.has('not-applied')
          ? h('span', { class: 'key not-applied' }, 'not applied')
          : null,
        writtenStates.has('not-known') ? h('span', { class: 'key not-known' }, 'not known') : null,
        writtenStates.has('held') ? h('span', { class: 'key held' }, 'kept in the ECU') : null,
        v.written && v.written.base ? h('span', { class: 'key base' }, 'base') : null,
        kind === 'number' && isNum(info.min) && isNum(info.max)
          ? h('button', {
              type: 'button',
              class: 'link',
              'aria-pressed': fit ? 'true' : 'false',
              title: fit ? 'Scale to the declared range (min to max)' : 'Scale to the values',
              onclick: () => {
                if (state.fit.has(v.name)) state.fit.delete(v.name);
                else state.fit.add(v.name);
                renderChart();
              },
              text: fit ? 'declared range' : 'fit values',
            })
          : null,
      ),
      readout,
      ...v.notes.map((n) => h('div', { class: 'note small', text: `Note: ${n}` })),
    );
    const { lane, svg, cursor } = laneShell(head, height, scale, v.failing ? 'failing' : '');
    state.lanes[state.lanes.length - 1].readout = { el: readout, v };
    svg.setAttribute('aria-label', `${v.name}: values over virtual time`);
    commonBack(svg, m, scale, height);
    if (strip) drawLines(svg, m, v, scale);
    const area = { y0: strip + 4, y1: height - 4 };
    for (const [source, gap] of Object.entries(v.gaps)) {
      if (gap.before < scale.t0 || gap.from > scale.t1) continue;
      const x0 = Math.max(0, scale.x(gap.from));
      const x1 = Math.min(scale.width, scale.x(gap.before));
      tooltip(
        s(
          'rect',
          {
            class: 'dropped',
            x: x0,
            y: area.y0,
            width: Math.max(1, x1 - x0),
            height: area.y1 - area.y0,
          },
          svg,
        ),
        `${gap.dropped} points of the ${source} series were dropped at the file's limits: values are missing from ${fmtMs(gap.from)} to ${fmtMs(gap.before)}`,
      );
    }
    const sampledEnd = v.sampled ? (v.sampled.lastMs ?? m.recordedTo) : null;
    if (kind === 'number') {
      const [lo, hi] = laneRange(v, fit);
      const y = (val) => area.y1 - ((val - lo) / (hi - lo || 1)) * (area.y1 - area.y0);
      s('text', { class: 'range-label', x: 2, y: area.y0 + 9, text: fmtNum(hi) }, svg);
      s('text', { class: 'range-label', x: 2, y: area.y1 - 2, text: fmtNum(lo) }, svg);
      drawBandsNumber(svg, v, scale, y, area, lo, hi);
      if (v.sampled)
        drawNumberSeries(
          svg,
          holdSegments(v.sampled, sampledEnd, v.gaps.sampled),
          scale,
          y,
          'seen',
        );
      if (v.written) {
        const segs = holdSegments(v.written, writtenEnd, v.gaps.written);
        if (v.written.base) drawBase(svg, segs, scale, y);
        drawNumberSeries(svg, segs, scale, y, 'sent');
      }
      drawMarks(
        svg,
        v,
        scale,
        (val) => (isNum(val) ? y(Math.min(hi, Math.max(lo, val))) : (area.y0 + area.y1) / 2),
        area,
      );
    } else {
      const bands = [];
      if (v.written) bands.push(['sent', v.written, writtenEnd, v.gaps.written]);
      if (v.sampled) bands.push(['seen', v.sampled, sampledEnd, v.gaps.sampled]);
      // A write of the failed step has its mark with its words; its band does not say them again.
      const marked = new Set(v.marks.filter((mk) => mk.kind === 'write').map((mk) => mk.at));
      bands.forEach(([cls, series, end, gap], i) => {
        const y0 = area.y0 + i * BAND_HEIGHT;
        drawStateBand(
          svg,
          mergeEqual(holdSegments(series, end, gap)),
          scale,
          y0,
          BAND_HEIGHT - 4,
          cls,
          kind,
          info,
          cls === 'sent' ? marked : new Set(),
        );
      });
      drawBandsState(svg, v, scale, area, info);
      drawMarks(svg, v, scale, () => area.y0 + (BAND_HEIGHT - 4) / 2, area);
    }
    if (!v.written && !v.sampled && !v.notes.length) {
      s('text', { class: 'empty-label', x: 6, y: height / 2 + 4, text: 'no values recorded' }, svg);
    }
    commonFront(svg, m, scale, height, cursor);
    return lane;
  }

  function drawLines(svg, m, v, scale) {
    for (const l of v.lines) {
      if (!isNum(l.from_ms)) continue;
      const known = knows(KNOWN.lineKind, l.kind);
      const x0 = scale.x(l.from_ms);
      const text = `${l.text ?? l.kind} (${l.step === null || l.step === undefined ? 'inputs' : `step ${l.step}`}, line ${l.line})`;
      const g = tooltip(
        s('g', { class: `line-mark line-${known ? l.kind : 'unknown'}` }, svg),
        text,
      );
      if (!known) {
        s('line', { x1: x0, x2: x0, y1: 1, y2: STRIP_HEIGHT - 2 }, g);
        if (isNum(l.to_ms))
          s('line', { x1: scale.x(l.to_ms), x2: scale.x(l.to_ms), y1: 1, y2: STRIP_HEIGHT - 2 }, g);
        continue;
      }
      if (l.kind === 'set') {
        s('path', { d: `M${x0} 2 l4 4 l-4 4 l-4 -4 z` }, g);
        continue;
      }
      let end =
        l.kind === 'dither' ? (l.until ?? m.domain[1]) : isNum(l.to_ms) ? l.to_ms : l.from_ms;
      if (isNum(l.stopped_at_ms)) end = Math.min(end, l.stopped_at_ms);
      if (l.kind === 'dither' && !l.shape) {
        s('path', { d: `M${x0 - 3} 3 l6 6 m0 -6 l-6 6` }, g);
        continue;
      }
      const x1 = scale.x(end);
      s('rect', { x: x0, y: 2, width: Math.max(2, x1 - x0), height: STRIP_HEIGHT - 5, rx: 2 }, g);
      const label =
        l.shape && typeof l.shape.type === 'string'
          ? `${l.kind === 'dither' ? 'dither ' : ''}${l.shape.type}`
          : l.kind;
      if (x1 - x0 > label.length * 6 + 6)
        s('text', { x: Math.max(x0, 0) + 3, y: STRIP_HEIGHT - 4, text: label }, g);
    }
  }

  /** The title of a segment of a series, by its state (the same words as the read-out and the writes table). */
  const STATE_TITLES = {
    'not-applied': 'Sent, and the ECU did not apply it',
    'not-known': 'Sent; not known whether the ECU applied it',
    held: 'The value that stayed in the ECU: the writes after it were not applied',
  };

  function drawNumberSeries(svg, segs, scale, y, cls) {
    const shown = decimate(segs, scale);
    const groups = { main: [], 'not-applied': [], 'not-known': [], held: [] };
    let prev = null;
    for (const seg of shown) {
      const key = Object.prototype.hasOwnProperty.call(groups, seg.state) ? seg.state : 'main';
      if (!isNum(seg.v)) {
        prev = null;
        continue;
      }
      const x0 = scale.x(seg.from);
      const x1 = scale.x(seg.to);
      let d = '';
      if (prev && prev.key === key && Math.abs(prev.x - x0) < 0.5)
        d += `M${x0} ${prev.y}L${x0} ${y(seg.v)}`;
      if (seg.min !== seg.max) d += `M${x0} ${y(seg.min)}L${x0} ${y(seg.max)}`;
      d += `M${x0} ${y(seg.v)}L${Math.max(x1, x0 + 0.5)} ${y(seg.v)}`;
      groups[key].push(d);
      prev = seg.state === 'alone' ? null : { key, x: x1, y: y(seg.v) };
      if (seg.state === 'alone' || unapplied(seg.state)) {
        const pt = s(
          'circle',
          { class: `pt ${cls} ${seg.state}`, cx: x0, cy: y(seg.v), r: 2.5 },
          svg,
        );
        if (unapplied(seg.state))
          tooltip(pt, `${STATE_TITLES[seg.state]}: ${fmtValue(seg.v)} at ${fmtMs(seg.from)}`);
      }
    }
    if (groups.main.length) s('path', { class: `series ${cls}`, d: groups.main.join('') }, svg);
    for (const key of ['held', 'not-applied', 'not-known']) {
      if (groups[key].length)
        tooltip(
          s('path', { class: `series ${cls} ${key}`, d: groups[key].join('') }, svg),
          STATE_TITLES[key],
        );
    }
  }

  function drawBase(svg, segs, scale, y) {
    const d = decimate(segs, scale)
      .filter((seg) => isNum(seg.base) && seg.state !== 'held')
      .map(
        (seg) =>
          `M${scale.x(seg.from)} ${y(seg.base)}L${Math.max(scale.x(seg.to), scale.x(seg.from) + 0.5)} ${y(seg.base)}`,
      )
      .join('');
    if (d) s('path', { class: 'series base', d }, svg);
  }

  function drawStateBand(svg, segs, scale, y0, height, cls, kind, info, marked) {
    for (const { seg, y, height: rh } of stateBandLayout(decimate(segs, scale), y0, height)) {
      // A value that is not of the variable's type is not drawn (the lane's note names it).
      if (seg.v === undefined) continue;
      const x0 = Math.max(-1, scale.x(seg.from));
      const x1 = Math.min(scale.width + 1, scale.x(seg.to));
      // A point at the end of the record (or alone in a gap) still gets a visible width.
      const w = Math.max(seg.to === seg.from || unapplied(seg.state) ? 3 : 0.5, x1 - x0);
      let fill;
      if (seg.v === null) fill = 'null';
      else if (seg.min !== seg.max) fill = 'mixed';
      else if (kind === 'bool') fill = seg.v ? 'true' : 'false';
      else fill = `e${Math.abs(Number(seg.v)) % 3}`;
      const r = s(
        'rect',
        { class: `state ${cls} ${seg.state} v-${fill}`, x: x0, y, width: w, height: rh },
        svg,
      );
      const value = fmtValue(seg.v, info);
      tooltip(
        r,
        seg.state === 'held'
          ? `${STATE_TITLES.held}: ${value} from ${fmtMs(seg.from)} to ${fmtMs(seg.to)}`
          : unapplied(seg.state)
            ? `${STATE_TITLES[seg.state]}: ${value} at ${fmtMs(seg.from)}`
            : `${value} from ${fmtMs(seg.from)}`,
      );
      if (seg.state === 'held' || (unapplied(seg.state) && marked.has(seg.from))) continue;
      const text = fill === 'mixed' ? '…' : value;
      const name =
        kind === 'enum' && info && Array.isArray(info.items)
          ? ((info.items.find((i) => i && i.value === seg.v) || {}).name ?? text)
          : text;
      const said = { 'not-applied': ' (not applied)', 'not-known': ' (not known)' }[seg.state];
      const label = said && w > (name + said).length * 6.5 + 6 ? name + said : name;
      if (w > label.length * 6.5 + 6) {
        s(
          'text',
          {
            class: `state-label ${seg.state}`,
            x: Math.max(x0, 0) + 3,
            y: y + rh - 6,
            text: label,
          },
          svg,
        );
      }
    }
  }

  /** Green for a condition that held, red for the one that failed; grey when its step was not decided by it. */
  function bandClass(b) {
    return b.failed ? 'bad' : b.status === 'passed' ? 'ok' : 'neutral';
  }

  function bandTitle(b, info) {
    const range = b.mode === 'at' ? `at ${fmtMs(b.from)}` : `${fmtMs(b.from)} to ${fmtMs(b.until)}`;
    const limit = b.limit !== null && b.limit !== b.to ? `, limit ${fmtMs(b.limit)}` : '';
    const result = b.failed ? 'did not hold' : b.status === 'passed' ? 'held' : `step ${b.status}`;
    return `Step ${b.step} (${b.mode}): ${compareText(b.compare, info)}, ${range}${limit}: ${result}`;
  }

  function drawBandsNumber(svg, v, scale, y, area, lo, hi) {
    const clampY = (val) => y(Math.min(hi, Math.max(lo, val)));
    for (const b of v.bands) {
      if (b.to < scale.t0 || b.from > scale.t1) continue;
      const x0 = scale.x(b.from);
      const x1 = Math.max(scale.x(b.to), x0 + 3);
      const g = tooltip(s('g', { class: `band ${bandClass(b)}` }, svg), bandTitle(b, v.info));
      for (const c of b.compare) {
        if (!isNum(c.value)) continue;
        let top;
        let bottom;
        if (c.op === 'ge' || c.op === 'gt') [top, bottom] = [area.y0, clampY(c.value)];
        else if (c.op === 'le' || c.op === 'lt') [top, bottom] = [clampY(c.value), area.y1];
        else if (c.op === 'near' && isNum(c.tolerance))
          [top, bottom] = [clampY(c.value + c.tolerance), clampY(c.value - c.tolerance)];
        else if (c.op === 'eq') [top, bottom] = [clampY(c.value) - 2, clampY(c.value) + 2];
        if (top !== undefined)
          s(
            'rect',
            { class: 'allowed', x: x0, y: top, width: x1 - x0, height: Math.max(2, bottom - top) },
            g,
          );
        const ty = clampY(c.value);
        s(
          'line',
          { class: `threshold${c.op === 'ne' ? ' ne' : ''}`, x1: x0, x2: x1, y1: ty, y2: ty },
          g,
        );
      }
      if (b.limit !== null && b.limit > b.to) {
        const xl = scale.x(b.limit);
        const ty = isNum(b.compare[0] && b.compare[0].value) ? clampY(b.compare[0].value) : area.y0;
        s('line', { class: 'threshold limit', x1: x1, x2: xl, y1: ty, y2: ty }, g);
      }
    }
  }

  function drawBandsState(svg, v, scale, area, info) {
    for (const b of v.bands) {
      if (b.to < scale.t0 || b.from > scale.t1) continue;
      const x0 = scale.x(b.from);
      const x1 = Math.max(scale.x(b.to), x0 + 3);
      const g = tooltip(s('g', { class: `band ${bandClass(b)}` }, svg), bandTitle(b, info));
      s(
        'rect',
        {
          class: 'expect-box',
          x: x0,
          y: area.y0 - 2,
          width: x1 - x0,
          height: area.y1 - area.y0 + 4,
        },
        g,
      );
      const text = `expect ${compareText(b.compare, info)}`;
      if (x1 - x0 > text.length * 6 + 4)
        s('text', { class: 'expect-label', x: x0 + 3, y: area.y1 - 1, text }, g);
    }
  }

  function drawMarks(svg, v, scale, yOf, area) {
    for (const mk of v.marks) {
      if (!isNum(mk.at) || mk.at < scale.t0 || mk.at > scale.t1) continue;
      const x = scale.x(mk.at);
      const cy = yOf(mk.value);
      if (mk.kind === 'condition') {
        const g = tooltip(
          s('g', { class: 'mark bad' }, svg),
          mk.text === null
            ? `No value at ${fmtMs(mk.at)}`
            : `It was ${mk.text} (sample of ${fmtMs(mk.at)}): the condition did not hold`,
        );
        s('circle', { cx: x, cy, r: 5 }, g);
        const label = mk.text === null ? 'no value' : mk.text;
        const left = x + label.length * 6.5 + 10 > scale.width;
        s(
          'text',
          {
            x: left ? x - 8 : x + 8,
            y: Math.max(area.y0 + 10, cy - 4),
            'text-anchor': left ? 'end' : 'start',
            text: label,
          },
          g,
        );
      } else {
        const cls = mk.applied === true ? 'ok' : mk.applied === false ? 'bad' : 'warn';
        const word =
          mk.applied === true
            ? 'applied'
            : mk.applied === false
              ? mk.sent === false
                ? 'not sent'
                : 'not applied'
              : 'not known whether applied';
        const g = tooltip(
          s('g', { class: `mark write ${cls}${mk.cause ? ' cause' : ''}` }, svg),
          `Write of ${fmtValue(mk.value, v.info, true)} for ${fmtMs(mk.at)}: ${word}${mk.cause ? ' (the cause of the failure)' : ''}`,
        );
        s('rect', { x: x - 5, y: cy - 5, width: 10, height: 10, rx: 2 }, g);
        s(
          'text',
          {
            class: 'glyph',
            x,
            y: cy + 4,
            'text-anchor': 'middle',
            text: mk.applied === true ? '✓' : mk.applied === false ? '✗' : '?',
          },
          g,
        );
        const label = `${fmtValue(mk.value, v.info)} ${mk.applied === null ? 'not known' : word}`;
        const left = x + label.length * 6.5 + 10 > scale.width;
        s(
          'text',
          {
            x: left ? x - 9 : x + 9,
            y: Math.max(area.y0 + 10, cy - 6),
            'text-anchor': left ? 'end' : 'start',
            text: label,
          },
          g,
        );
      }
    }
  }

  // --- the cursor and zooming

  function attachPointer(svg, scale, height) {
    let down = null;
    let sel = null;
    const xOf = (e) => {
      const r = svg.getBoundingClientRect();
      return Math.min(scale.width, Math.max(0, e.clientX - r.left));
    };
    svg.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return;
      down = xOf(e);
      svg.setPointerCapture(e.pointerId);
    });
    svg.addEventListener('pointermove', (e) => {
      const x = xOf(e);
      state.cursor = scale.t(x);
      updateCursor();
      if (down !== null && Math.abs(x - down) > 4) {
        if (!sel) sel = s('rect', { class: 'selection', y: 0, height }, svg);
        sel.setAttribute('x', Math.min(x, down));
        sel.setAttribute('width', Math.abs(x - down));
      }
    });
    svg.addEventListener('pointerup', (e) => {
      const x = xOf(e);
      if (down !== null && Math.abs(x - down) > 4)
        setView([scale.t(Math.min(x, down)), scale.t(Math.max(x, down))]);
      down = null;
      if (sel) sel.remove();
      sel = null;
    });
    svg.addEventListener('dblclick', () => setView(state.model.domain));
  }

  function updateCursor() {
    const m = state.model;
    const t = state.cursor;
    if (!m || t === null || t < state.view[0] || t > state.view[1]) {
      for (const lane of state.lanes) {
        lane.cursor.setAttribute('x1', -10);
        lane.cursor.setAttribute('x2', -10);
        if (lane.readout) lane.readout.el.textContent = '';
      }
      const time = $('cursor-time');
      if (time) time.textContent = 'Virtual time';
      const steps = $('cursor-steps');
      if (steps) steps.textContent = '';
      return;
    }
    const scale = makeScale(state.view[0], state.view[1], state.width);
    const x = scale.x(t);
    for (const lane of state.lanes) {
      lane.cursor.setAttribute('x1', x);
      lane.cursor.setAttribute('x2', x);
      if (!lane.readout) continue;
      const v = lane.readout.v;
      const parts = [];
      const end = m.recordedTo;
      const w = valueAt(v.written, t, end, v.gaps.written);
      const sv = valueAt(v.sampled, t, end, v.gaps.sampled);
      if (w) parts.push(readoutText('sent', w, v.info));
      if (sv) parts.push(readoutText('seen', sv, v.info));
      // The time is in the head of the axis lane; each lane says only its values (all of them on hover).
      lane.readout.el.textContent = parts.join(' · ');
      lane.readout.el.title = lane.readout.el.textContent;
    }
    $('cursor-time').textContent = `Cursor ${fmtMs(Number(t.toPrecision(6)))}`;
    const active = m.steps.filter((st) => {
      if (!isNum(st.start_ms)) return false;
      const e = [st.end_ms, st.at_ms].filter(isNum).reduce((a, b) => Math.max(a, b), st.start_ms);
      return t >= st.start_ms && (t <= e || st.status === 'unfinished');
    });
    $('cursor-steps').textContent = active.length
      ? active.map((st) => `#${st.index} ${st.kind} (${st.status})`).join(', ')
      : '';
  }

  // --- loading

  function readFile(file) {
    file.text().then(
      (text) => load(text, file.name),
      (e) => showError(`"${file.name}" could not be read (${e.message}). Choose the file again.`),
    );
  }

  function fromSrc(src) {
    if (location.protocol === 'file:') {
      showError(
        `?src= works only when the page comes from a web server, and this page was opened as a file. Drop the timeline file on the page instead.`,
      );
      return;
    }
    let url;
    try {
      url = new URL(src, location.href);
    } catch {
      showError(
        `?src=${src} is not an address. Name the timeline file on this server, or drop the file on the page.`,
      );
      return;
    }
    if (url.origin !== location.origin) {
      showError(
        `?src= must name a file on this page's own server (${location.origin}), and ${url.origin} is another one, so the file was not loaded. Download the file and drop it on the page instead.`,
      );
      return;
    }
    // A redirect could lead to another origin: the file must come from this address itself.
    fetch(url, { credentials: 'same-origin', redirect: 'error' })
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        return r.text();
      })
      .then(
        (text) => load(text, url.pathname.split('/').pop() || url.pathname),
        (e) =>
          showError(
            `${url.href} could not be loaded (${e.message}). Check the address, or drop the file on the page.`,
          ),
      );
  }

  function start() {
    $('open').addEventListener('click', () => $('file').click());
    $('file').addEventListener('change', (e) => {
      const file = e.target.files && e.target.files[0];
      if (file) readFile(file);
      e.target.value = '';
    });
    document.addEventListener('dragover', (e) => {
      e.preventDefault();
      document.body.classList.add('dragging');
    });
    document.addEventListener('dragleave', (e) => {
      if (e.relatedTarget === null) document.body.classList.remove('dragging');
    });
    document.addEventListener('drop', (e) => {
      e.preventDefault();
      document.body.classList.remove('dragging');
      const file = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
      if (file) readFile(file);
    });
    $('show-failure').addEventListener('click', () => setView(initialView(state.model)));
    $('show-all').addEventListener('click', () => setView(state.model.domain));
    $('zoom-out').addEventListener('click', () => {
      const [t0, t1] = state.view;
      const half = t1 - t0;
      setView([t0 - half / 2, t1 + half / 2]);
    });
    const chart = $('chart');
    chart.addEventListener('keydown', (e) => {
      if (!state.model) return;
      if (e.key === '+' || e.key === '=' || e.key === '-') {
        // Zoom in or out around the cursor (or the middle of the view).
        e.preventDefault();
        const [t0, t1] = state.view;
        const c = state.cursor ?? (t0 + t1) / 2;
        const k = e.key === '-' ? 2 : 0.5;
        setView([c - (c - t0) * k, c + (t1 - c) * k]);
        return;
      }
      if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
      e.preventDefault();
      const stepT =
        (state.model.tick ?? (state.view[1] - state.view[0]) / 100) * (e.shiftKey ? 10 : 1);
      const t = state.cursor ?? state.view[0];
      state.cursor = Math.min(
        state.view[1],
        Math.max(state.view[0], t + (e.key === 'ArrowLeft' ? -stepT : stepT)),
      );
      updateCursor();
    });
    let lastWidth = 0;
    new ResizeObserver(() => {
      const w = chart.clientWidth;
      if (state.model && w !== lastWidth) {
        lastWidth = w;
        renderChart();
      }
    }).observe(chart);

    const embedded = $('timeline-data');
    const src = new URLSearchParams(location.search).get('src');
    if (embedded && embedded.textContent.trim() !== '') load(embedded.textContent, null);
    else if (src) fromSrc(src);
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})(typeof globalThis !== 'undefined' ? globalThis : this);
