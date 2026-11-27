// The timeline of vehicle-sil: draws a timeline file, the JSON that
// `vehicle_sil_launch run <configuration> --scenario <scenario> --timeline <file>` writes.
// The format is schema/timeline.schema.json and "Recording a scenario run on a timeline" in docs/simulation-run.md.
//
// It defines the custom element <vehicle-sil-timeline> (its attributes, properties and events: "Putting a timeline
// on your own screen" in docs/gui.md, and vehicle-sil-timeline.d.ts). The element draws into its own shadow root,
// so that the page's CSS and its own do not meet. timeline.html with page.js is the page that shows one element.
//
// One plain script: no framework, no build, no fonts or scripts from elsewhere. All text goes in with textContent
// (never innerHTML), because names, units and messages come from users and ECUs. It must never contain the closing
// tag of a script element, so that it can be put inline too.
// Every text it shows comes from messages.js (English and Japanese), loaded before this script; a page adds or
// changes texts with the element's messages property. Its CSS is timeline.css. bundle.js makes the one file that
// users load, vehicle-sil-timeline.js, from messages.js, timeline.css and this file.
// Everything above "The element" needs no DOM: tests/viewer.test.js runs it in Node.
(function (global) {
  'use strict';

  const FORMAT = 1;
  const MESSAGES = global.VehicleSilTimelineMessages;
  const LANGUAGES = ['en', 'ja'];
  let language = 'en';
  /** The texts that the element being drawn adds or changes ({ <language>: { <key>: text } }), or null. */
  let extra = null;

  /**
   * The language of the page: ?lang= in `search` when the page has it, else the first of the browser's languages
   * (`preferred`, as navigator.languages) that it has, else English. `known`: the languages there are texts for
   * (default: English and Japanese).
   */
  function pickLanguage(search, preferred, known) {
    const asked = new URLSearchParams(search || '').get('lang');
    return chooseLanguage([asked, ...(preferred || [])], known);
  }

  /** The first of the language tags (as "ja-JP") that `known` has, by the whole tag or its first part; else 'en'. */
  function chooseLanguage(tags, known) {
    const have = known || LANGUAGES;
    for (const tag of tags) {
      if (typeof tag !== 'string' || tag === '') continue;
      const lower = tag.toLowerCase();
      const found =
        have.find((k) => k.toLowerCase() === lower) ??
        have.find((k) => k.toLowerCase() === lower.split('-')[0]);
      if (found) return found;
    }
    return 'en';
  }

  function setLanguage(lang) {
    language = LANGUAGES.includes(lang) ? lang : 'en';
  }

  /**
   * A text in a language: the page's own texts (`more`, as the element's messages property) first, then
   * messages.js; English when the language does not have it; a function value is called with args. Unknown: the key.
   */
  function text(lang, key, args, more) {
    for (const where of [more?.[lang], MESSAGES?.[lang], more?.en, MESSAGES?.en]) {
      const value = where ? where[key] : undefined;
      if (value === undefined) continue;
      return typeof value === 'function' ? value(args || {}) : value;
    }
    return key;
  }

  /** A text of messages.js in the language of what is being drawn. */
  function t(key, args) {
    return text(language, key, args, extra);
  }

  /** A step's or the scenario's status in the page's language (an unknown one as the file writes it). */
  function statusWord(status) {
    const key = `status.${status}`;
    return MESSAGES && Object.prototype.hasOwnProperty.call(MESSAGES.en, key)
      ? t(key)
      : String(status);
  }

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
    const label = name ? t('file.named', { name }) : t('file.this');
    const how = t('file.howToWrite');
    let data = input;
    if (typeof input === 'string') {
      try {
        data = JSON.parse(input);
      } catch (e) {
        return { error: t('read.notJson', { label, error: e.message, how }) };
      }
    }
    if (!isObject(data) || !('vehicle_sil_timeline' in data)) {
      return { error: t('read.notTimeline', { label, how }) };
    }
    if (data.vehicle_sil_timeline !== FORMAT) {
      const by =
        typeof data.vehicle_sil === 'string'
          ? t('read.writtenBy', { version: data.vehicle_sil })
          : '';
      const format = JSON.stringify(data.vehicle_sil_timeline);
      return { error: t('read.otherFormat', { label, format, by, known: FORMAT }) };
    }
    if (data.stage !== 'run') {
      return { error: t('read.otherStage', { label, stage: JSON.stringify(data.stage) }) };
    }
    if (!isObject(data.scenario)) {
      return { error: t('read.noScenario', { label, how }) };
    }
    for (const key of ['variables', 'steps', 'series']) {
      if (!Array.isArray(data[key])) {
        return { error: t('read.noArray', { label, key, how }) };
      }
      const bad = data[key].findIndex((item) => !isObject(item));
      if (bad >= 0) {
        return { error: t('read.notObject', { label, key, index: bad, how }) };
      }
    }
    if (data.variables.some((v) => typeof v.name !== 'string')) {
      return { error: t('read.noName', { label, how }) };
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
      .join(t('compare.and'));
  }

  // ---------------------------------------------------------------------------------------------------------------
  // The model: what the page draws, worked out once per file

  /** Whether a value fits its variable's type. null fits all (a real that is no number); a type not known takes all. */
  function fitsType(value, type) {
    if (value === null || !knows(KNOWN.type, type)) return true;
    if (type === 'bool') return typeof value === 'boolean';
    if (type === 'real') return isNum(value);
    return Number.isInteger(value);
  }

  /**
   * Reads one series of a variable of type `type` into columns. Returns { series, notes } or { note } when the
   * page cannot draw it. A value that is not of the type becomes undefined (not drawn) and is named in the notes.
   */
  function readSeries(s, type) {
    const what = t('series.what', { source: s.source });
    if (!Object.prototype.hasOwnProperty.call(KNOWN.encoding, s.source)) {
      return { note: t('series.unknownSource', { what, source: s.source }) };
    }
    if (s.encoding !== KNOWN.encoding[s.source]) {
      return { note: t('series.unknownEncoding', { what, encoding: s.encoding }) };
    }
    if (!Array.isArray(s.points)) return { note: t('series.noPoints', { what }) };
    let ti = 0;
    let vi = 1;
    let bi = -1;
    if (Array.isArray(s.columns)) {
      ti = s.columns.indexOf('at_ms');
      vi = s.columns.indexOf('value');
      bi = s.columns.indexOf('base');
      if (ti < 0 || vi < 0) {
        return { note: t('series.noColumns', { what }) };
      }
    }
    const times = [];
    const v = [];
    const base = bi >= 0 ? [] : null;
    const wrong = [];
    let rounded = 0;
    for (const p of s.points) {
      if (!Array.isArray(p) || !isNum(p[ti])) {
        return { note: t('series.badPoint', { what }) };
      }
      times.push(p[ti]);
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
        t('series.wrongType', {
          count: wrong.length,
          what,
          words: t(`type.${type}`),
          type,
          value: JSON.stringify(value),
          at: fmtMs(at),
        }),
      );
    }
    if (rounded) {
      notes.push(t('series.rounded', { count: rounded, what, max: Number.MAX_SAFE_INTEGER }));
    }
    return {
      notes,
      series: {
        source: s.source,
        t: times,
        v,
        base,
        /** For points of the failed step's writes: point index -> applied (true, false, or null: not known). */
        applied: null,
        notAppliedFrom: Number.isInteger(s.not_applied_from) ? s.not_applied_from : null,
        /** written: a dither writes it too (its points have the column base). */
        dithered: bi >= 0,
        /** written: whether the file says the point before not_applied_from is in the ECU (see appliedBefore). */
        beforeApplied: false,
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
   * at: the instant from_ms; always and during: from_ms to to_ms, but not past the failure of the step or the
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
        notes.push(t('band.unknownMode', { step: step.index, mode: w.mode }));
        return;
      }
      const compare = Array.isArray(c.compare) ? c.compare.filter(isObject) : [];
      const unknownOp = compare.find((x) => !knows(KNOWN.op, x.op));
      if (unknownOp) {
        notes.push(t('band.unknownOp', { step: step.index, op: unknownOp.op }));
        return;
      }
      const from = w.from_ms;
      let to = isNum(w.to_ms) ? w.to_ms : from;
      let limit = null;
      const until = to;
      if (w.mode === 'once') {
        limit = isNum(w.to_ms) ? w.to_ms : null;
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

  const nonEmptyString = (v) => (typeof v === 'string' && v !== '' ? v : null);

  function stepFile(step, scenarioFile) {
    return (isObject(step) && nonEmptyString(step.file)) || nonEmptyString(scenarioFile) || null;
  }

  function lineFile(line, scenarioFile) {
    return (isObject(line) && nonEmptyString(line.file)) || nonEmptyString(scenarioFile) || null;
  }

  function stepTitle(step, scenarioFile) {
    const file = stepFile(step, scenarioFile);
    const args = {
      index: step.index,
      kind: step.kind,
      line: step.line,
      status: statusWord(step.status),
    };
    if (file && file !== scenarioFile) return t('step.titleInFile', { ...args, file });
    return t('step.title', args);
  }

  function lineTitle(line, scenarioFile) {
    const file = lineFile(line, scenarioFile);
    const where =
      line.step === null || line.step === undefined
        ? t('line.inputs')
        : t('line.step', { step: line.step });
    const args = { text: line.text ?? line.kind, where, line: line.line };
    if (file && file !== scenarioFile) return t('line.titleInFile', { ...args, file });
    return t('line.title', args);
  }

  function callStackFrames(callStack, scenarioFile) {
    if (!Array.isArray(callStack)) return [];
    const fallback = nonEmptyString(scenarioFile) || '';
    return callStack.filter(isObject).map((fr) => {
      const where = t('failure.where', {
        line: fr.line ?? '',
        file: nonEmptyString(fr.file) || fallback,
      });
      const name = nonEmptyString(fr.name);
      const params = isObject(fr.params)
        ? Object.entries(fr.params)
            .map(([k, v]) => `${k} = ${fmtValue(v)}`)
            .join(', ')
        : '';
      return [where, name, params].filter(Boolean).join(' · ');
    });
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
      const point = Number.isInteger(w.point) ? w.point : null;
      return {
        variable: w.variable,
        at: isNum(w.at_ms) ? w.at_ms : null,
        value: w.value,
        valueText:
          w.value === null || w.value === undefined
            ? t('write.notComputed')
            : fmtValue(w.value, info && info.info, true),
        point,
        applied: w.applied === true ? true : w.applied === false ? false : null,
        // point null: nothing was sent, or the point went at the file's limits (then limits.dropped names the
        // variable's written series).
        pointDropped:
          point === null &&
          isObject(tl.limits) &&
          Array.isArray(tl.limits.dropped) &&
          tl.limits.dropped.some(
            (d) => isObject(d) && d.variable === w.variable && d.source === 'written',
          ),
        cause: f.write === i,
      };
    });
    const known = writes.filter(Boolean);
    const at = f && isNum(f.at_ms) ? f.at_ms : isNum(step.at_ms) ? step.at_ms : null;
    return {
      step,
      file: stepFile(step, tl.scenario.file),
      name: nonEmptyString(step.name),
      callStack: Array.isArray(step.call_stack) ? step.call_stack.filter(isObject) : [],
      at,
      code: (f && f.code) || step.code || null,
      message: typeof step.message === 'string' ? step.message : null,
      conditions,
      writes: known,
    };
  }

  /**
   * Whether the file says that the point before not_applied_from of v's written series is in the ECU, so that the
   * ECU kept that value after the writes it did not apply. In a run that did not pass, a point before
   * not_applied_from may not have been applied (the run ended before its result was looked at), so only these say
   * so: the point is a write of the failed step with applied true; the run passed; or every line of the variable
   * that the point can be of is of a step that passed (a step passes only when its writes were applied), and no
   * dither writes the variable (a dither's write is no step's).
   */
  function appliedBefore(v, tl) {
    const series = v.written;
    const naf = series.notAppliedFrom;
    if (naf === null || naf < 1 || naf > series.t.length) return false;
    const i = naf - 1;
    if (series.applied && series.applied.get(i) === true) return true;
    if (tl.scenario.status === 'passed') return true;
    if (series.dithered || v.lines.some((l) => l.kind === 'dither')) return false;
    const t = series.t[i];
    const of = v.lines.filter(
      (l) =>
        isNum(l.from_ms) &&
        l.from_ms <= t &&
        (l.to_ms === null || (isNum(l.to_ms) && t <= l.to_ms)),
    );
    return (
      of.length > 0 &&
      of.every((l) => {
        const step = tl.steps.find((s) => isObject(s) && s.index === l.step);
        return step !== undefined && step.status === 'passed';
      })
    );
  }

  /** Builds everything the page draws from a file that readTimeline() accepted. */
  function buildModel(tl) {
    const sc = tl.scenario;
    const recordedTo = isNum(sc.recorded_to_ms) ? sc.recorded_to_ms : null;
    const timeStep = isNum(tl.time_step_ms) && tl.time_step_ms > 0 ? tl.time_step_ms : null;
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
        v.notes.push(t('variable.unknownType', { type: v.info.type }));
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
          sent: w.point !== null || w.pointDropped,
          cause: w.cause,
        });
      }
    }

    for (const v of vars) if (v.written) v.written.beforeApplied = appliedBefore(v, tl);

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
    const margin = Math.max(timeStep ?? 0, end * 0.04, end > 0 ? 0 : 10);
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
      name: nonEmptyString(sc.name),
      caseName: nonEmptyString(sc.case),
      status: sc.status,
      recordedTo,
      overAt: isNum(sc.over_at_ms) ? sc.over_at_ms : null,
      timeStep,
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
    const span = Math.max(right - start, (d1 - d0) * 0.2, (model.timeStep ?? 1) * 4);
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
   * that is not 'not-applied'. Returns { from, to, v } or null (no such point, not known from the start, or the
   * file does not say that the value before was applied: beforeApplied).
   */
  function keptValue(series, end) {
    const naf = series.notAppliedFrom;
    const n = series.t.length;
    if (
      naf === null ||
      naf < 1 ||
      naf >= n ||
      series.beforeApplied !== true ||
      pointState(series, naf) !== 'not-applied'
    )
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
   * (the value that stayed in the ECU after a write it did not apply, while that is known and where the file says
   * it was applied: keptValue); 'alone' (a point
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
   *                                            has (value undefined: from before the record); { lastSent }: the
   *                                            value sent before, which the file does not say was applied; or
   *                                            null: not known;
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
      let kept = null;
      if (known && naf === 0) kept = { value: undefined };
      else if (known && series.beforeApplied === true) kept = { value: series.v[naf - 1] };
      else if (known) kept = { lastSent: series.v[naf - 1] };
      return { value: series.v[lo], state, kept };
    }
    return { value: series.v[lo] };
  }

  /** The words of the cursor's read-out for one series ('sent' or 'seen') at a time: see valueAt(). */
  function readoutText(source, at, info) {
    const word = t(`word.${source}`);
    if (at.missing) return t('readout.missing', { word });
    if (at.value === undefined) return t('readout.wrongType', { word });
    const value = t('readout.value', { word, value: fmtValue(at.value, info) });
    if (at.state === 'not-known') return t('readout.notKnown', { value });
    if (at.state !== 'not-applied') return value;
    if (at.kept !== null && 'lastSent' in at.kept)
      return t('readout.notAppliedLastSent', { value, last: fmtValue(at.kept.lastSent, info) });
    const has =
      at.kept === null
        ? t('readout.hasUnknown')
        : at.kept.value === undefined
          ? t('readout.hasEarlier')
          : fmtValue(at.kept.value, info);
    return t('readout.notApplied', { value, has });
  }

  /**
   * The words of the table of a failed step's writes, from what the file says of each write: applied, point (null:
   * not sent, or its point was dropped at the limits) and whether it is the cause. Returns { heading, explain,
   * cause(write) }.
   */
  function writeWords(f) {
    const at = f.writes.length && f.writes[0].at !== null ? f.writes[0].at : f.at;
    const has = (applied) => f.writes.some((w) => w.applied === applied);
    const explain = [
      has(true) ? t('writes.explainApplied') : null,
      has(false) ? t('writes.explainNotApplied') : null,
      has(null) ? t('writes.explainNotKnown') : null,
    ]
      .filter(Boolean)
      .join(' ');
    const cause = (w) => {
      if (w.point === null && w.pointDropped) {
        const dropped = t('writes.pointDropped');
        return w.cause ? t('writes.causeAnd', { what: dropped }) : dropped;
      }
      if (w.point === null && w.applied === false)
        return w.cause ? t('writes.notSentCause') : t('writes.notSent');
      return w.cause ? t('writes.cause') : '';
    };
    return { heading: t('writes.heading', { at: fmtMs(at) }), explain, cause };
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
    MESSAGES,
    LANGUAGES,
    pickLanguage,
    chooseLanguage,
    setLanguage,
    t,
    text,
    statusWord,
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
    stepFile,
    lineFile,
    stepTitle,
    lineTitle,
    callStackFrames,
  };

  global.VehicleSilTimeline = api;
  if (typeof document === 'undefined' || typeof customElements === 'undefined') return;

  // ---------------------------------------------------------------------------------------------------------------
  // The element
  //
  // The drawing functions below work on the element being drawn: `current`, its `state` and its shadow `root`.
  // Every way into them (a property, an attribute, a listener, the answer of a fetch, the observer of the size)
  // first makes its element the current one with enter(), or is made with bound(), which does that.

  const SVG = 'http://www.w3.org/2000/svg';
  const LANE_NUMBER_HEIGHT = 76;
  const COMPACT_NUMBER_HEIGHT = 44;
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
      else if (k.startsWith('on')) e.addEventListener(k.slice(2), bound(val));
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

  /** What one element shows; the view, the cursor and the step are what its properties of the same names say. */
  function newState() {
    return {
      model: null,
      view: null,
      fit: new Set(),
      hidden: new Set(),
      cursor: null,
      /** The index of the chosen step, or null. */
      step: null,
      readouts: [],
      width: 0,
      /** The name of the file shown (null for data without a name, or none). */
      file: null,
    };
  }

  /** The element being drawn ({ host, root, state, options }), its state and its shadow root. */
  let current = null;
  let state = null;
  let root = null;

  /** Makes an element the one being drawn: its state, its root, its language and its texts. */
  function enter(element) {
    current = element;
    state = element.state;
    root = element.root;
    language = element.language;
    extra = element.options.messages;
  }

  /** fn, made to draw on the element that is current now, whenever it is called. */
  function bound(fn) {
    const element = current;
    return function (...args) {
      enter(element);
      return fn.apply(this, args);
    };
  }

  const $ = (id) => root.getElementById(id);

  /** Tells the page around: an event on the element, unless a property set from outside caused the change. */
  function emit(type, detail, always) {
    if (current.quiet && !always) return;
    current.host.dispatchEvent(
      new CustomEvent(type, { detail, bubbles: type !== 'load' && type !== 'error' }),
    );
  }

  /**
   * Says why a file cannot be shown. The element then shows no file, so that nothing in it is taken for this one.
   * keep: the input is what could not be read (it is read again, and refused again, in another language); otherwise
   * nothing is kept to show, and the message (a function: say(), in the language of the moment) is said again when
   * the language changes. again: the same refusal in another language, which is not told again.
   */
  function showError(say, status, keep, again) {
    const message = typeof say === 'function' ? say() : say;
    current.error = keep ? null : { say, status };
    if (!keep) {
      current.input = null;
      current.inputName = null;
    }
    const box = $('error');
    box.replaceChildren(h('strong', { text: t('page.cannotShow') }), message);
    box.hidden = false;
    state.model = null;
    state.file = null;
    state.readouts = [];
    $('view').hidden = true;
    $('title').textContent = t('page.title');
    $('facts').replaceChildren();
    if (!again) emit('error', { message, status: status ?? null }, true);
  }

  function clearError() {
    current.error = null;
    $('error').hidden = true;
  }

  /**
   * Shows a file (its text or its parsed JSON). again: the same file in another language or with other texts, which
   * keeps what the user chose (the view, the cursor, the step, the lanes); a new file starts from the properties.
   */
  function load(input, name, again) {
    const read = readTimeline(input, name);
    if (read.error) {
      showError(read.error, null, true, again);
      return;
    }
    clearError();
    const model = buildModel(read.timeline);
    const before = again && state.model ? state : null;
    state.model = model;
    state.file = name || null;
    if (before) {
      setView(state.view, true);
      state.step = model.steps.some((st) => st.index === state.step) ? state.step : null;
    } else {
      state.fit.clear();
      state.hidden.clear();
      const o = current.options;
      state.view = initialView(model);
      state.cursor = isNum(o.cursor) ? o.cursor : null;
      state.step = model.steps.some((st) => st.index === o.step) ? o.step : null;
      if (state.step !== null && isNum(stepOf(state.step)?.start_ms))
        state.view = stepView(stepOf(state.step));
      if (isNum(o.from) || isNum(o.to))
        setView([o.from ?? model.domain[0], o.to ?? model.domain[1]], true);
    }
    $('view').hidden = false;
    renderAll();
    if (!before) {
      emit(
        'load',
        {
          file: state.file,
          scenario: model.name ?? null,
          status: model.status,
          start: model.domain[0],
          end: model.domain[1],
        },
        true,
      );
    }
  }

  // --- the parts that do not change with the view

  function renderHeader(m) {
    const tl = m.tl;
    $('title').textContent = m.name ?? t('page.noName');
    const status = h('span', {
      class: `badge st-${statusClass(m.status)}`,
      title: String(m.status),
      text: statusWord(m.status),
    });
    const facts = [
      ['fact.file', state.file],
      ['fact.scenario', tl.scenario.file],
      ['fact.case', nonEmptyString(tl.scenario.case)],
      ['fact.configuration', tl.file],
      ['fact.variant', tl.variant],
      ['fact.timeStep', m.timeStep !== null ? fmtMs(m.timeStep) : null],
      ['fact.recordedTo', m.recordedTo !== null ? fmtMs(m.recordedTo) : t('fact.nothingRecorded')],
      ['fact.version', tl.vehicle_sil],
    ].filter(([, v]) => v !== null && v !== undefined);
    const params = Array.isArray(tl.params) ? tl.params.filter(isObject) : [];
    $('facts').replaceChildren(
      status,
      ...facts.map(([k, v]) =>
        h('span', { class: 'fact' }, h('span', { class: 'muted', text: `${t(k)} ` }), String(v)),
      ),
      ...params.map((p) =>
        h(
          'span',
          { class: 'fact' },
          h('span', { class: 'muted', text: `${t('fact.param')} ` }),
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
      const stepName = nonEmptyString(step.name);
      const where = [
        step.kind,
        stepName,
        t('failure.where', { line: step.line, file: step.file || m.tl.scenario.file }),
        f.code,
      ]
        .filter(Boolean)
        .join(' · ');
      const frames = callStackFrames(step.call_stack, m.tl.scenario.file);
      parts.push(
        h(
          'div',
          { class: 'banner bad', 'data-testid': 'failure' },
          h('h3', { text: t('failure.heading', { step: step.index, at: fmtMs(f.at) }) }),
          h('p', { class: 'muted small', text: `${where} · ${step.text}` }),
          frames.length
            ? h(
                'p',
                { class: 'muted small', 'data-testid': 'failure-call-stack' },
                `${t('failure.calledFrom')} ${frames.join(' → ')}`,
              )
            : null,
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
          h('h3', { text: t('passed.heading', { count: n }) }),
          skipped ? h('p', { text: t('passed.skipped', { count: skipped }) }) : null,
        ),
      );
    } else {
      const text =
        m.status === 'not_run'
          ? t('notDecided.notRun')
          : m.status === 'incomplete'
            ? t('notDecided.incomplete')
            : t('notDecided.unknown', { status: m.status });
      parts.push(
        h(
          'div',
          { class: 'banner warn', 'data-testid': 'not-decided' },
          h('h3', { text: t('notDecided.heading', { status: statusWord(m.status) }) }),
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
          h('strong', { text: t('dropped.heading') }),
          t('dropped.text'),
          m.dropped
            .map((d) =>
              t('dropped.item', {
                variable: d.variable,
                source: d.source,
                count: d.dropped,
                from: fmtMs(d.dropped_from_ms),
                to: fmtMs(d.dropped_before_ms),
              }),
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
          h('strong', { text: t('late.heading') }),
          t('late.text'),
        ),
      );
    }
    box.replaceChildren(...parts);
  }

  function renderConditions(f) {
    return h(
      'div',
      {},
      h('h4', { text: t('conditions.heading') }),
      h(
        'ul',
        { 'data-testid': 'failed-conditions' },
        ...f.conditions.map((c) => {
          const what = `${c.variable ?? '?'} ${c.compare ?? ''}`.trim();
          let got;
          if (c.valueText !== null)
            got =
              t('conditions.was', { value: c.valueText }) +
              (c.sampleMs !== null ? t('conditions.sampleOf', { at: fmtMs(c.sampleMs) }) : '');
          else if (c.mode === 'during') got = t('conditions.noSample');
          else got = t('conditions.noValue');
          return h('li', {}, h('code', { text: what }), `: ${got}`);
        }),
      ),
    );
  }

  function renderWrites(f) {
    const words = writeWords(f);
    const label = (w) =>
      w.applied === true
        ? t('writes.applied')
        : w.applied === false
          ? t('writes.notApplied')
          : t('writes.notKnown');
    return h(
      'div',
      {},
      h('h4', { text: words.heading }),
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
            ...['writes.colVariable', 'writes.colValue', 'writes.colInEcu', 'writes.colCause'].map(
              (x) => h('th', { scope: 'col', text: t(x) }),
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
      h('span', { class: 'muted', text: `${t('participants.label')} ` }),
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
        (typeof st.expect_skipped === 'string'
          ? t('step.expectSkipped', { why: st.expect_skipped })
          : '');
      const canShow = isNum(st.start_ms);
      const stFile = nonEmptyString(st.file);
      const lineCell =
        stFile && stFile !== m.tl.scenario.file
          ? `${stFile}:${st.line ?? ''}`
          : String(st.line ?? '');
      const stName = nonEmptyString(st.name);
      const stFrames = callStackFrames(st.call_stack, m.tl.scenario.file);
      return h(
        'tr',
        {
          class:
            [st.status === 'failed' ? 'failed-row' : '', st.index === state.step ? 'selected' : '']
              .filter(Boolean)
              .join(' ') || null,
          'data-step': st.index,
        },
        h(
          'td',
          {},
          canShow
            ? h('button', {
                type: 'button',
                class: 'link',
                title: t('step.showOnChart'),
                onclick: () => showStep(st, true),
                text: `#${st.index}`,
              })
            : `#${st.index}`,
        ),
        h('td', { text: lineCell }),
        h('td', { text: String(st.kind) }),
        h(
          'td',
          {},
          h('span', {
            class: `badge st-${statusClass(st.status)}`,
            title: String(st.status),
            text: statusWord(st.status),
          }),
        ),
        h('td', { class: 'num', text: fmtMs(st.start_ms) }),
        h('td', { class: 'num', text: fmtMs(st.at_ms) }),
        h('td', { class: 'num', text: fmtMs(st.end_ms) }),
        h(
          'td',
          {},
          stName ? h('strong', { text: `${stName} · ` }) : null,
          String(st.text ?? ''),
          stFrames.length
            ? h('div', {
                class: 'muted small',
                text: `${t('failure.calledFrom')} ${stFrames.join(' → ')}`,
              })
            : null,
        ),
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
        h('td', {
          text: l.step === null || l.step === undefined ? t('line.inputs') : `#${l.step}`,
        }),
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
    const findingRows = m.findings.map((x) => {
      const fFile = nonEmptyString(x.file);
      const fLine = x.line === null || x.line === undefined ? '' : String(x.line);
      const fWhere =
        fFile && fFile !== m.tl.scenario.file ? (fLine ? `${fFile}:${fLine}` : fFile) : fLine;
      return h(
        'tr',
        {},
        h('td', { text: String(x.severity ?? '') }),
        h('td', {}, h('code', { text: String(x.code) })),
        h('td', { text: fWhere }),
        h('td', { text: String(x.message ?? t('finding.codeOnly')) }),
      );
    });
    const table = (caption, heads, rows, testid) =>
      h(
        'details',
        { open: true },
        h('summary', { text: t('tables.count', { caption, count: rows.length }) }),
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
          : h('p', { class: 'muted', text: t('tables.none') }),
      );
    $('tables').replaceChildren(
      table(
        t('tables.steps'),
        cols('index', 'line', 'kind', 'status', 'start', 'decided', 'end', 'step', 'result'),
        stepRows,
        'steps-table',
      ),
      table(
        t('tables.lines'),
        cols('index', 'variable', 'kind', 'step', 'from', 'to', 'base', 'text'),
        lineRows,
        'lines-table',
      ),
      table(
        t('tables.findings'),
        cols('severity', 'code', 'line', 'message'),
        findingRows,
        'findings-table',
      ),
    );
  }

  /** The headings of a table's columns. */
  function cols(...names) {
    return names.map((name) => t(`col.${name}`));
  }

  /** The view around a step (from its start to its end and the end of what it watches). */
  function stepView(st) {
    const from = st.start_ms;
    const ends = [st.end_ms, st.at_ms, isObject(st.watch) ? st.watch.to_ms : null].filter(isNum);
    const to = Math.max(from, ...ends);
    const pad = Math.max((to - from) * 0.25, (state.model.timeStep ?? 1) * 3);
    return [Math.max(state.model.domain[0], from - pad), Math.min(state.model.domain[1], to + pad)];
  }

  /** The step of an index, or undefined. */
  function stepOf(index) {
    return state.model ? state.model.steps.find((st) => st.index === index) : undefined;
  }

  /** Chooses a step (null: none): marks it on the chart and in the table, and shows it when it has a time. */
  function showStep(st, scroll) {
    const changed = (st ? st.index : null) !== state.step;
    state.step = st ? st.index : null;
    for (const row of root.querySelectorAll('tr[data-step]'))
      row.classList.toggle('selected', Number(row.dataset.step) === state.step);
    if (st && isNum(st.start_ms)) setView(stepView(st));
    else renderChart();
    if (changed) emit('stepselect', { step: state.step });
    if (scroll) $('chart').scrollIntoView({ block: 'nearest' });
  }

  // --- the chart

  /** Shows a part of the run (clamped to it). noDraw: only set it (the caller draws). */
  function setView(view, noDraw) {
    const [d0, d1] = state.model.domain;
    let [t0, t1] = view;
    t0 = Math.max(d0, t0);
    t1 = Math.min(d1, t1);
    if (!(t1 > t0)) return;
    const changed = !state.view || state.view[0] !== t0 || state.view[1] !== t1;
    state.view = [t0, t1];
    if (noDraw) return;
    renderChart();
    if (changed) emit('viewchange', { from: t0, to: t1 });
  }

  function renderAll() {
    const m = state.model;
    renderHeader(m);
    renderSummary(m);
    renderParticipants(m);
    renderTables(m);
    renderChart();
  }

  /** The width of a lane's plot: the chart less the column of the lane heads (as timeline.css lays them out). */
  function plotWidth() {
    const chart = $('chart');
    const compact = current.options.compact;
    const head = compact
      ? chart.clientWidth >= 480
        ? 120
        : 0
      : chart.clientWidth >= 720
        ? 200
        : 0;
    return Math.max(compact ? 100 : 160, chart.clientWidth - head - 2);
  }

  /**
   * The lanes of the variables to draw, in order: with the variables property, those it names (a name, or a
   * prefix ending in '*') in its order, else the inputs of the scenario first. groups: whether to head the two
   * groups (only without the property). missing: the entries of the property that match no variable of the file.
   */
  function lanesToDraw(m) {
    const visible = (v) => !state.hidden.has(v.participant);
    const wanted = current.options.variables;
    if (!wanted) {
      const inputs = m.inputs.filter(visible);
      return { inputs, others: m.others.filter(visible), groups: true, missing: [] };
    }
    const every = [...m.inputs, ...m.others];
    const all = every.filter(visible);
    const chosen = [];
    const missing = [];
    for (const pattern of wanted) {
      const prefix = pattern.endsWith('*') ? pattern.slice(0, -1) : null;
      const matches = (v) => (prefix !== null ? v.name.startsWith(prefix) : v.name === pattern);
      if (!every.some(matches)) missing.push(pattern);
      for (const v of all) {
        if (!chosen.includes(v) && matches(v)) chosen.push(v);
      }
    }
    return { inputs: chosen, others: [], groups: false, missing };
  }

  function renderChart() {
    const m = state.model;
    if (!m) return;
    const width = plotWidth();
    state.width = width;
    const scale = makeScale(state.view[0], state.view[1], width);
    state.readouts = [];
    const chart = $('chart');
    chart.removeAttribute('data-hover');
    const children = [axisLane(scale), stepLane(m, scale)];
    const { inputs, others, groups, missing } = lanesToDraw(m);
    // A name of the variables property that matches nothing is said, not dropped in silence.
    const note = $('chart-note');
    const names = [...m.inputs, ...m.others].map((v) => v.name);
    note.hidden = missing.length === 0;
    note.textContent = missing.length
      ? t('chart.noMatch', {
          names: missing.join(', '),
          have: names.slice(0, 5).join(', ') + (names.length > 5 ? ', …' : ''),
        })
      : '';
    if (inputs.length) {
      if (groups) children.push(h('div', { class: 'group', text: t('chart.inputs') }));
      children.push(...inputs.map((v) => variableLane(m, v, scale)));
    }
    if (others.length) {
      if (groups) children.push(h('div', { class: 'group', text: t('chart.others') }));
      children.push(...others.map((v) => variableLane(m, v, scale)));
    }
    chart.replaceChildren(...children, h('div', { class: 'cursor-line', 'aria-hidden': 'true' }));
    delete $('step-details').dataset.step;
    $('range').textContent = t('chart.showing', {
      from: fmtMs(scale.t0),
      to: fmtMs(scale.t1),
      start: fmtMs(m.domain[0]),
      end: fmtMs(m.domain[1]),
    });
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
    attachPointer(svg, scale, height);
    return { lane, svg };
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

  function commonFront(svg, m, scale, height) {
    if (m.failure && isNum(m.failure.at) && m.failure.at >= scale.t0 && m.failure.at <= scale.t1) {
      const x = scale.x(m.failure.at);
      s('line', { class: 'fail-line', x1: x, x2: x, y1: 0, y2: height }, svg);
    }
  }

  function axisLane(scale) {
    const height = 22;
    const { lane, svg } = laneShell(
      h('span', {
        class: 'muted small',
        id: 'cursor-time',
        'aria-live': 'polite',
        text: t('chart.virtualTime'),
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
        isNum(st.watch.to_ms) &&
        st.watch.to_ms > end
          ? st.watch.to_ms
          : null;
      if ((limit ?? end) < scale.t0 || start > scale.t1) continue;
      const stName = nonEmptyString(st.name);
      const label = stName ? `#${st.index} ${stName}` : `#${st.index} ${st.kind}`;
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
      h('strong', { text: t('chart.steps') }),
      h('div', { class: 'readout small', id: 'cursor-steps' }),
    );
    const { lane, svg } = laneShell(head, height, scale, 'steps');
    commonBack(svg, m, scale, height);
    items.forEach((it, i) => {
      const y = rows[i] * STEP_ROW + 4;
      const bh = STEP_ROW - 6;
      const chosen = it.st.index === state.step ? ' selected' : '';
      const g = s(
        'g',
        { class: `step st-${statusClass(it.st.status)}${chosen}`, 'data-step': it.st.index },
        svg,
      );
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
      const stName = nonEmptyString(it.st.name);
      const stFrames = callStackFrames(it.st.call_stack, m.tl.scenario.file);
      const lines = [
        stepTitle(it.st, m.tl.scenario.file),
        stName,
        String(it.st.text ?? ''),
        ...stFrames.map((fr) => `${t('failure.calledFrom')} ${fr}`),
        t('step.times', {
          start: fmtMs(it.st.start_ms),
          decided: fmtMs(it.st.at_ms),
          end: fmtMs(it.st.end_ms),
        }),
      ].filter((x) => x !== null && x !== '');
      if (it.st.message) lines.push(it.st.message);
      if (typeof it.st.expect_skipped === 'string')
        lines.push(t('step.expectSkipped', { why: it.st.expect_skipped }));
      tooltip(g, lines.join('\n'));
    });
    commonFront(svg, m, scale, height);
    return lane;
  }

  function variableLane(m, v, scale) {
    const kind = laneKind(v);
    const both = v.written && v.sampled;
    const strip = v.lines.length ? STRIP_HEIGHT : 0;
    const bandCount = both ? 2 : 1;
    const numberHeight = current.options.compact ? COMPACT_NUMBER_HEIGHT : LANE_NUMBER_HEIGHT;
    const plotH = kind === 'number' ? numberHeight : BAND_HEIGHT * bandCount + 8;
    const height = strip + plotH;
    const info = v.info;
    const range =
      isNum(info.min) && isNum(info.max)
        ? t('lane.range', { min: fmtNum(info.min), max: fmtNum(info.max) })
        : null;
    const meta = [
      info.causality ?? null,
      info.type,
      info.unit || null,
      range,
      isNum(info.ecu_step_ms) ? t('lane.ecuStep', { ms: fmtMs(info.ecu_step_ms) }) : null,
    ]
      .filter(Boolean)
      .join(' · ');
    const fit = defaultFit(v) !== state.fit.has(v.name);
    const labels = current.options.labels;
    const label =
      labels &&
      Object.prototype.hasOwnProperty.call(labels, v.name) &&
      typeof labels[v.name] === 'string'
        ? labels[v.name]
        : null;
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
      label ? h('span', { class: 'var-label', text: label }) : null,
      h('code', { class: `var-name${v.failing ? ' failing' : ''}`, text: v.name }),
      h('div', { class: 'muted small', text: meta }),
      h(
        'div',
        { class: 'legend small' },
        v.written
          ? h(
              'span',
              { class: 'key sent' },
              t(both && kind !== 'number' ? 'legend.sentUpper' : 'legend.sent'),
            )
          : null,
        v.sampled
          ? h(
              'span',
              { class: 'key seen' },
              t(both && kind !== 'number' ? 'legend.seenLower' : 'legend.seen'),
            )
          : null,
        writtenStates.has('not-applied')
          ? h('span', { class: 'key not-applied' }, t('legend.notApplied'))
          : null,
        writtenStates.has('not-known')
          ? h('span', { class: 'key not-known' }, t('legend.notKnown'))
          : null,
        writtenStates.has('held') ? h('span', { class: 'key held' }, t('legend.held')) : null,
        v.written && v.written.base ? h('span', { class: 'key base' }, t('legend.base')) : null,
        kind === 'number' && isNum(info.min) && isNum(info.max)
          ? h('button', {
              type: 'button',
              class: 'link',
              'aria-pressed': fit ? 'true' : 'false',
              title: fit ? t('fit.toDeclared') : t('fit.toValues'),
              onclick: () => {
                if (state.fit.has(v.name)) state.fit.delete(v.name);
                else state.fit.add(v.name);
                renderChart();
              },
              text: fit ? t('fit.declared') : t('fit.values'),
            })
          : null,
      ),
      readout,
      ...v.notes.map((n) => h('div', { class: 'note small', text: t('lane.note', { text: n }) })),
    );
    const { lane, svg } = laneShell(head, height, scale, v.failing ? 'failing' : '');
    const hoverValue = h('span');
    const hover = h(
      'div',
      { class: 'hover-readout small', 'aria-hidden': 'true' },
      h('strong', { text: label ?? v.name }),
      hoverValue,
    );
    svg.parentElement.append(hover);
    state.readouts.push({ el: readout, hoverValue, v });
    svg.setAttribute('aria-label', t('lane.label', { name: label ?? v.name }));
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
        t('lane.dropped', {
          count: gap.dropped,
          source,
          from: fmtMs(gap.from),
          to: fmtMs(gap.before),
        }),
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
      s('text', { class: 'empty-label', x: 6, y: height / 2 + 4, text: t('lane.noValues') }, svg);
    }
    commonFront(svg, m, scale, height);
    return lane;
  }

  function drawLines(svg, m, v, scale) {
    for (const l of v.lines) {
      if (!isNum(l.from_ms)) continue;
      const known = knows(KNOWN.lineKind, l.kind);
      const x0 = scale.x(l.from_ms);
      const text = lineTitle(l, m.tl.scenario.file);
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
    'not-applied': 'state.notApplied',
    'not-known': 'state.notKnown',
    held: 'state.held',
  };
  const stateTitle = (st) => t(STATE_TITLES[st]);

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
          tooltip(
            pt,
            t('state.at', {
              title: stateTitle(seg.state),
              value: fmtValue(seg.v),
              at: fmtMs(seg.from),
            }),
          );
      }
    }
    if (groups.main.length) s('path', { class: `series ${cls}`, d: groups.main.join('') }, svg);
    for (const key of ['held', 'not-applied', 'not-known']) {
      if (groups[key].length)
        tooltip(
          s('path', { class: `series ${cls} ${key}`, d: groups[key].join('') }, svg),
          stateTitle(key),
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
    let before = null; // the last rect of the row: { from, y, x1 }
    for (const { seg, y, height: rh } of stateBandLayout(decimate(segs, scale), y0, height)) {
      // A value that is not of the variable's type is not drawn (the lane's note names it).
      if (seg.v === undefined) continue;
      let x0 = Math.max(-1, scale.x(seg.from));
      // Points of one time (a write sent after one that was refused) go side by side in the order sent, so that
      // a later one does not cover the one before, and past the mark of a failed write at that time (10 px wide).
      if (before && before.y === y && before.from === seg.from)
        x0 = Math.max(x0, before.x1, marked.has(seg.from) ? scale.x(seg.from) + 6 : -Infinity);
      const x1 = Math.min(scale.width + 1, scale.x(seg.to));
      // A point at the end of the record (or alone in a gap) still gets a visible width.
      const w = Math.max(seg.to === seg.from || unapplied(seg.state) ? 3 : 0.5, x1 - x0);
      before = { from: seg.from, y, x1: x0 + w };
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
          ? t('state.fromTo', {
              title: stateTitle('held'),
              value,
              from: fmtMs(seg.from),
              to: fmtMs(seg.to),
            })
          : unapplied(seg.state)
            ? t('state.at', { title: stateTitle(seg.state), value, at: fmtMs(seg.from) })
            : t('state.from', { value, from: fmtMs(seg.from) }),
      );
      if (seg.state === 'held' || (unapplied(seg.state) && marked.has(seg.from))) continue;
      const text = fill === 'mixed' ? '…' : value;
      const name =
        kind === 'enum' && info && Array.isArray(info.items)
          ? ((info.items.find((i) => i && i.value === seg.v) || {}).name ?? text)
          : text;
      const said = {
        'not-applied': t('state.notAppliedSuffix'),
        'not-known': t('state.notKnownSuffix'),
      }[seg.state];
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
    const range =
      b.mode === 'at'
        ? t('band.at', { at: fmtMs(b.from) })
        : t('band.range', { from: fmtMs(b.from), to: fmtMs(b.until) });
    const limit =
      b.limit !== null && b.limit !== b.to ? t('band.limit', { limit: fmtMs(b.limit) }) : '';
    const result = b.failed
      ? t('band.didNotHold')
      : b.status === 'passed'
        ? t('band.held')
        : t('band.stepStatus', { status: statusWord(b.status) });
    return t('band.title', {
      step: b.step,
      mode: b.mode,
      compare: compareText(b.compare, info),
      range,
      limit,
      result,
    });
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
      const text = t('band.expect', { compare: compareText(b.compare, info) });
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
            ? t('mark.noValueAt', { at: fmtMs(mk.at) })
            : t('mark.was', { value: mk.text, at: fmtMs(mk.at) }),
        );
        s('circle', { cx: x, cy, r: 5 }, g);
        const label = mk.text === null ? t('mark.noValue') : mk.text;
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
            ? t('mark.applied')
            : mk.applied === false
              ? mk.sent === false
                ? t('mark.notSent')
                : t('mark.notApplied')
              : t('mark.notKnownWhether');
        const g = tooltip(
          s('g', { class: `mark write ${cls}${mk.cause ? ' cause' : ''}` }, svg),
          t('mark.write', {
            value: fmtValue(mk.value, v.info, true),
            at: fmtMs(mk.at),
            word,
            cause: mk.cause ? t('mark.cause') : '',
          }),
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
        const label = t('mark.label', {
          value: fmtValue(mk.value, v.info),
          word: mk.applied === null ? t('mark.notKnown') : word,
        });
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

  /** el.addEventListener, drawing on the element that is current now. */
  function listen(el, type, fn) {
    el.addEventListener(type, bound(fn));
  }

  /** Moves the cursor as the user does, and says so. */
  function moveCursor(at, hoveredStep) {
    const changed = at !== state.cursor;
    state.cursor = at;
    updateCursor(hoveredStep);
    if (changed) emit('cursorchange', { cursor: at });
  }

  function attachPointer(svg, scale, height) {
    const plot = svg.parentElement;
    let down = null;
    let downStep = null;
    let sel = null;
    const xOf = (e) => {
      const r = svg.getBoundingClientRect();
      return Math.min(scale.width, Math.max(0, e.clientX - r.left));
    };
    listen(plot, 'pointerdown', (e) => {
      if (e.button !== 0) return;
      $('chart').removeAttribute('data-hover');
      down = xOf(e);
      // A click on a step chooses it (the pointer is captured, so the click itself goes to the lane).
      const step = e.target.closest ? e.target.closest('[data-step]') : null;
      downStep = step ? stepOf(Number(step.dataset.step)) : null;
      plot.setPointerCapture(e.pointerId);
    });
    listen(plot, 'pointermove', (e) => {
      const x = xOf(e);
      if (e.pointerType === 'mouse' && down === null) {
        $('chart').setAttribute('data-hover', '');
      }
      const step = e.target.closest('[data-step]');
      moveCursor(scale.t(x), step ? stepOf(Number(step.dataset.step)) : null);
      if (down !== null && Math.abs(x - down) > 4) {
        if (!sel) sel = s('rect', { class: 'selection', y: 0, height }, svg);
        sel.setAttribute('x', Math.min(x, down));
        sel.setAttribute('width', Math.abs(x - down));
      }
    });
    listen(plot, 'pointerleave', () => $('chart').removeAttribute('data-hover'));
    listen(plot, 'pointerup', (e) => {
      const x = xOf(e);
      const dragged = down !== null && Math.abs(x - down) > 4;
      if (dragged) setView([scale.t(Math.min(x, down)), scale.t(Math.max(x, down))]);
      else if (down !== null && downStep) showStep(downStep);
      down = null;
      downStep = null;
      if (sel) sel.remove();
      sel = null;
    });
    listen(plot, 'pointercancel', () => {
      down = null;
      downStep = null;
      if (sel) sel.remove();
      sel = null;
    });
    listen(plot, 'dblclick', () => setView(state.model.domain));
  }

  /** A readable description of the step, kept outside the plot so it never covers the waveforms. */
  function updateStepDetails(preferred) {
    const m = state.model;
    const panel = $('step-details');
    const step =
      preferred ??
      stepOf(state.step) ??
      m?.failure?.step ??
      m?.steps.find((st) => Array.isArray(st.watch?.conditions) && st.watch.conditions.length) ??
      m?.steps[0];
    panel.hidden = !step;
    if (!step || panel.dataset.step === String(step.index)) return;
    panel.dataset.step = String(step.index);
    const title = stepTitle(step, m.tl.scenario.file);
    const stepName = nonEmptyString(step.name);
    const frames = callStackFrames(step.call_stack, m.tl.scenario.file);
    const timings = t('step.times', {
      start: fmtMs(step.start_ms),
      decided: fmtMs(step.at_ms),
      end: fmtMs(step.end_ms),
    });
    const watch = step.watch;
    const conditions = Array.isArray(watch?.conditions)
      ? watch.conditions.filter((c) => isObject(c) && typeof c.variable === 'string')
      : [];
    const modes = {
      once: 'step.matchOnce',
      at: 'step.matchAt',
      always: 'step.matchThroughout',
      during: 'step.matchThroughout',
    };
    const mode = Object.hasOwn(modes, watch?.mode) ? modes[watch.mode] : null;
    panel.replaceChildren(
      h(
        'div',
        { class: 'detail-heading' },
        h('strong', { text: title }),
        stepName ? h('span', { text: stepName }) : null,
        h('span', { class: 'muted', text: timings }),
      ),
      frames.length
        ? h(
            'div',
            { class: 'muted small', 'data-testid': 'step-call-stack' },
            `${t('failure.calledFrom')} ${frames.join(' → ')}`,
          )
        : null,
      conditions.length
        ? h(
            'div',
            {},
            mode
              ? h('div', { text: t(mode, { from: fmtMs(watch.from_ms), to: fmtMs(watch.to_ms) }) })
              : null,
            h(
              'ul',
              {},
              conditions.map((c) =>
                h(
                  'li',
                  {},
                  h('code', { text: c.variable }),
                  ` ${compareText(c.compare, m.byName.get(c.variable)?.info)}`,
                ),
              ),
            ),
          )
        : h('div', { text: step.text ?? '' }),
    );
    panel.scrollTop = 0;
  }

  function updateCursor(hoveredStep) {
    const m = state.model;
    const at = state.cursor;
    const chart = $('chart');
    if (!m || at === null || at < state.view[0] || at > state.view[1]) {
      chart.removeAttribute('data-hover');
      chart.removeAttribute('data-cursor-side');
      for (const { el } of state.readouts) el.title = el.textContent = '';
      const time = $('cursor-time');
      if (time) time.title = time.textContent = t('chart.virtualTime');
      const steps = $('cursor-steps');
      if (steps) steps.title = steps.textContent = '';
      updateStepDetails();
      return;
    }
    const scale = makeScale(state.view[0], state.view[1], state.width);
    const x = scale.x(at);
    const right = x < scale.width / 2;
    chart.setAttribute('data-cursor-side', right ? 'right' : 'left');
    chart.style.setProperty('--cursor-x', `${x}px`);
    chart.style.setProperty('--cursor-room', `${(right ? scale.width - x : x) - 8}px`);
    for (const { el, hoverValue, v } of state.readouts) {
      const parts = [];
      const end = m.recordedTo;
      const w = valueAt(v.written, at, end, v.gaps.written);
      const sv = valueAt(v.sampled, at, end, v.gaps.sampled);
      if (w) parts.push(readoutText('sent', w, v.info));
      if (sv) parts.push(readoutText('seen', sv, v.info));
      const values = parts.join(' · ');
      el.title = el.textContent = values;
      hoverValue.textContent = values || '—';
    }
    const time = $('cursor-time');
    time.title = time.textContent = t('chart.cursor', { at: fmtMs(Number(at.toPrecision(6))) });
    const active = m.steps.filter((st) => {
      if (!isNum(st.start_ms)) return false;
      const e = [st.end_ms, st.at_ms].filter(isNum).reduce((a, b) => Math.max(a, b), st.start_ms);
      return at >= st.start_ms && (at <= e || st.status === 'unfinished');
    });
    const steps = $('cursor-steps');
    steps.title = steps.textContent = active
      .map((st) => `#${st.index} ${st.kind} (${statusWord(st.status)})`)
      .join(', ');
    const inspected = hoveredStep ?? (hoveredStep === undefined ? stepOf(state.step) : null);
    updateStepDetails(inspected ?? active.find((st) => isObject(st.watch)) ?? active[0]);
  }

  // --- loading

  /** The key under which the GUI's screens keep the server's token in sessionStorage (takeToken of the client). */
  const TOKEN_KEY = 'vehicle-sil-gui-token';

  /** The token that the GUI's screens keep in this tab, or null. */
  function storedToken() {
    try {
      return window.sessionStorage.getItem(TOKEN_KEY);
    } catch {
      return null; // the browser blocks site data
    }
  }

  /** Loads the file at src (an address on the page's own server) and shows it; an older load still under way is dropped. */
  function fromSrc(src) {
    const element = current;
    const seq = ++element.loads;
    if (element.abort) element.abort.abort();
    element.abort = null;
    if (location.protocol === 'file:') {
      showError(() => t('load.fileProtocol'));
      return;
    }
    let url;
    try {
      url = new URL(src, document.baseURI);
    } catch {
      showError(() => t('load.notAddress', { src }));
      return;
    }
    if (url.origin !== location.origin) {
      showError(() => t('load.otherOrigin', { origin: location.origin, other: url.origin }));
      return;
    }
    const o = element.options;
    const token = o.token ?? storedToken();
    const headers = {};
    for (const [k, v] of Object.entries(isObject(o.headers) ? o.headers : {}))
      if (typeof v === 'string') headers[k] = v;
    if (token) headers.Authorization = `Bearer ${token}`;
    let name = url.pathname.split('/').pop() || url.pathname;
    const abort = typeof AbortController === 'function' ? new AbortController() : null;
    element.abort = abort;
    element.host.setAttribute('aria-busy', 'true');
    const fresh = () => {
      if (element.abort === abort) element.abort = null; // this load is over
      if (seq !== element.loads) return false;
      element.host.removeAttribute('aria-busy');
      return true;
    };
    // A redirect could lead to another origin with the token and the headers: the file must come from this address
    // itself. 'manual' does not follow it, and says that it was one (an opaque answer).
    fetch(url, {
      credentials: 'same-origin',
      redirect: 'manual',
      headers,
      signal: abort ? abort.signal : undefined,
    })
      .then(
        bound((r) => {
          if (r.type === 'opaqueredirect') {
            throw Object.assign(new Error(), {
              say: () => t('load.redirected', { url: url.href }),
            });
          }
          if (r.status === 401) {
            // Said alone: the address is right, and the token is what to change.
            throw Object.assign(new Error(), { say: () => t('load.unauthorized'), status: 401 });
          }
          if (!r.ok) {
            return r.text().then(
              bound((body) => {
                const said = serverMessage(body);
                const say = () => t('load.http', { status: r.status, message: said });
                throw Object.assign(new Error(), { say, status: r.status });
              }),
            );
          }
          name = fileName(r.headers.get('Content-Disposition')) || name;
          return r.text();
        }),
      )
      .then(
        bound((text) => {
          if (!fresh()) return;
          element.input = text;
          element.srcName = name;
          element.inputName = element.host.fileName || name;
          load(text, element.inputName);
        }),
        bound((e) => {
          if (!fresh() || (e && e.name === 'AbortError')) return;
          // 401 and a redirect are said alone (the address is right); other HTTP errors and a failed request
          // within load.failed (the address may be wrong).
          const alone = e.say && (e.status === 401 || !e.status);
          const say = alone
            ? e.say
            : () => t('load.failed', { url: url.href, error: e.say ? e.say() : e.message });
          showError(say, e.status);
        }),
      );
  }

  /** The message of a GUI server's error ({"error": {"message"}}; in English, as the server writes it), or ''. */
  function serverMessage(body) {
    try {
      const message = JSON.parse(body)?.error?.message;
      return typeof message === 'string' ? message : '';
    } catch {
      return '';
    }
  }

  /** The file name of a Content-Disposition header (filename="..."), or null. */
  function fileName(header) {
    const match = /filename="([^"\\]+)"/.exec(header ?? '');
    return match ? match[1] : null;
  }

  /** A text of messages.js into an element: a string, or an array of strings and [tag, text] (never parsed). */
  function fill(el, value) {
    const parts = Array.isArray(value) ? value : [value];
    el.replaceChildren(
      ...parts.map((part) => (Array.isArray(part) ? h(part[0], { text: part[1] }) : part)),
    );
  }

  /** Stops a load under way: its answer is dropped. */
  function stopLoad(element) {
    element.loads++;
    if (element.abort) element.abort.abort();
    element.abort = null;
    element.host.removeAttribute('aria-busy');
  }

  /** Shows nothing: no file is given. */
  function clear() {
    stopLoad(current);
    current.input = null;
    current.inputName = null;
    state.model = null;
    state.file = null;
    state.readouts = [];
    clearError();
    $('view').hidden = true;
    $('title').textContent = t('page.title');
    $('facts').replaceChildren();
  }

  // --- the element's parts, look and keys

  /** The shadow root's contents. Texts come from messages.js (data-text, data-label) whenever the language changes. */
  function skeleton() {
    return [
      h(
        'header',
        { id: 'head' },
        h(
          'div',
          {},
          h('h2', { id: 'title', 'data-text': 'page.title' }),
          h('div', { id: 'facts', class: 'small' }),
        ),
        h('slot', { name: 'actions' }),
      ),
      h('div', { id: 'error', role: 'alert', hidden: true }),
      h(
        'section',
        { id: 'view', hidden: true },
        h('div', { id: 'summary' }),
        h(
          'div',
          { id: 'toolbar', class: 'toolbar small' },
          h('span', { id: 'range', class: 'muted' }),
          h('button', { id: 'show-failure', type: 'button', 'data-text': 'page.showFailure' }),
          h('button', { id: 'show-all', type: 'button', 'data-text': 'page.whole' }),
          h('button', { id: 'zoom-out', type: 'button', 'data-text': 'page.zoomOut' }),
          h('span', { id: 'participants', class: 'toolbar' }),
        ),
        h('p', { id: 'help', class: 'muted small', 'data-text': 'page.help' }),
        h('p', { id: 'chart-note', class: 'note small', role: 'status', hidden: true }),
        h('section', {
          id: 'step-details',
          class: 'small',
          'data-label': 'step.details',
          tabindex: '0',
          hidden: true,
        }),
        h('div', { id: 'chart', tabindex: '0', 'data-label': 'page.chartLabel' }),
        h('div', { id: 'tables' }),
      ),
    ];
  }

  function translate() {
    for (const el of root.querySelectorAll('[data-text]')) fill(el, t(el.dataset.text));
    for (const el of root.querySelectorAll('[data-label]'))
      el.setAttribute('aria-label', t(el.dataset.label));
    if (!state.model) $('title').textContent = t('page.title');
  }

  /** The buttons and keys of the chart. */
  function attachControls() {
    listen($('show-failure'), 'click', () => setView(initialView(state.model)));
    listen($('show-all'), 'click', () => setView(state.model.domain));
    listen($('zoom-out'), 'click', () => {
      const [t0, t1] = state.view;
      const half = t1 - t0;
      setView([t0 - half / 2, t1 + half / 2]);
    });
    const chart = $('chart');
    listen(chart, 'keydown', (e) => {
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
      chart.removeAttribute('data-hover');
      const stepT =
        (state.model.timeStep ?? (state.view[1] - state.view[0]) / 100) * (e.shiftKey ? 10 : 1);
      const t = state.cursor ?? state.view[0];
      moveCursor(
        Math.min(
          state.view[1],
          Math.max(state.view[0], t + (e.key === 'ArrowLeft' ? -stepT : stepT)),
        ),
      );
    });
    let lastWidth = 0;
    new ResizeObserver(
      bound(() => {
        const w = chart.clientWidth;
        if (state.model && w !== lastWidth) {
          lastWidth = w;
          renderChart();
        }
      }),
    ).observe(chart);
  }

  /** The style sheet of every element (timeline.css, which bundle.js puts into the one file), made once. */
  let sheet = null;
  function adoptStyles(shadow) {
    const css = global.VehicleSilTimelineStyles;
    if (typeof css !== 'string') return;
    if ('adoptedStyleSheets' in shadow && typeof CSSStyleSheet === 'function') {
      if (!sheet) {
        sheet = new CSSStyleSheet();
        sheet.replaceSync(css);
      }
      shadow.adoptedStyleSheets = [sheet];
    } else {
      shadow.append(h('style', { text: css }));
    }
  }

  const number = (v) => {
    if (v === null || v === undefined || v === '') return null;
    const n = typeof v === 'number' ? v : Number(v);
    return Number.isFinite(n) ? n : null;
  };
  const list = (v) => {
    if (v === null || v === undefined) return null;
    const items = (Array.isArray(v) ? v.map(String) : String(v).split(/[\s,]+/)).filter(Boolean);
    return items.length ? items : null;
  };

  /**
   * The simple options: attribute -> [property, kind, what a change redraws]. A string or boolean option is kept
   * in its attribute (the property sets it); a number or a list is kept as the property (the attribute sets it).
   */
  const OPTIONS = {
    src: ['src', 'string', 'source'],
    'file-name': ['fileName', 'string', 'name'],
    cursor: ['cursor', 'number', 'cursor'],
    step: ['step', 'number', 'step'],
    from: ['from', 'number', 'from'],
    to: ['to', 'number', 'to'],
    variables: ['variables', 'list', 'chart'],
    hide: ['hide', 'string', 'none'],
    compact: ['compact', 'boolean', 'chart'],
    theme: ['theme', 'string', 'none'],
  };
  /** The words of hide and the values of theme that timeline.css reads. */
  const PARTS = ['header', 'summary', 'toolbar', 'help', 'steps', 'readout', 'legend', 'tables'];
  const THEMES = ['light', 'dark', 'auto'];

  /**
   * Says once in the browser's console, for each element and value, that a value of an option cannot be used: it
   * is not an error of the file (the event error), but a mistake that the screen's author should see.
   */
  function warn(c, key, args) {
    const said = `${key} ${JSON.stringify(args)}`;
    if (c.warned.has(said)) return;
    c.warned.add(said);
    console.warn(text(c.language, key, args, c.options.messages));
  }

  /** Checks a value given to an option (an attribute or a property), and warns about one it cannot use. */
  function check(c, property, value) {
    if (['cursor', 'step', 'from', 'to'].includes(property)) {
      if (value !== null && value !== undefined && value !== '' && number(value) === null)
        warn(c, 'warn.number', { name: property, value: String(value) });
      const { from, to } = c.options;
      if ((property === 'from' || property === 'to') && isNum(from) && isNum(to) && from >= to)
        warn(c, 'warn.range', { from, to });
    } else if (property === 'hide') {
      for (const word of list(value) ?? [])
        if (!PARTS.includes(word)) warn(c, 'warn.hide', { value: word, known: PARTS.join(', ') });
    } else if (property === 'theme') {
      if (value !== null && value !== undefined && !THEMES.includes(String(value)))
        warn(c, 'warn.theme', { value: String(value) });
    }
  }

  /** The options that are properties only (objects, and the token, which has no place in the page's markup). */
  const PROPERTIES = {
    data: 'source',
    token: 'source',
    headers: 'source',
    labels: 'chart',
    messages: 'language',
  };

  class VehicleSilTimelineElement extends HTMLElement {
    static get observedAttributes() {
      return [...Object.keys(OPTIONS), 'lang'];
    }

    /** What the drawing functions work on (current, when this element is drawn); not part of the element's API. */
    #c;

    constructor() {
      super();
      const c = {
        host: this,
        root: this.attachShadow({ mode: 'open' }),
        state: newState(),
        options: {
          cursor: null,
          step: null,
          from: null,
          to: null,
          variables: null,
          compact: false,
          data: null,
          token: null,
          headers: null,
          labels: null,
          messages: null,
        },
        language: 'en',
        /** Whether a change comes from outside (a property or an attribute): it is not told back with an event. */
        quiet: false,
        /** The number of the latest load, and how to stop it: an older answer is dropped. */
        loads: 0,
        abort: null,
        /** What is shown (the text or the object, and its name), to draw it again in another language. */
        input: null,
        inputName: null,
        /** The name the server gave the file of src. */
        srcName: null,
        /** The refusal shown ({ say, status }), said again in another language. */
        error: null,
        /** The values already warned about (see warn). */
        warned: new Set(),
        changes: new Set(['source', 'language']),
        scheduled: false,
      };
      this.#c = c;
      adoptStyles(c.root);
      c.root.append(...skeleton());
      enter(c);
      attachControls();
    }

    connectedCallback() {
      // A property set before this script defined the element hides the class's accessor: set it again.
      for (const name of [...Object.values(OPTIONS).map(([p]) => p), ...Object.keys(PROPERTIES)]) {
        if (Object.prototype.hasOwnProperty.call(this, name)) {
          const value = this[name];
          delete this[name];
          this[name] = value;
        }
      }
      this.#schedule();
    }

    disconnectedCallback() {
      // A load under way stops, and starts again when the element is put back (a framework may move it).
      const c = this.#c;
      if (c.abort) {
        stopLoad(c);
        c.changes.add('source');
      }
    }

    attributeChangedCallback(name, before, after) {
      if (name === 'lang') return this.#change('language');
      const [property, kind, what] = OPTIONS[name];
      const c = this.#c;
      const o = c.options;
      if (name === 'hide' && after !== null && after.includes(',')) {
        // timeline.css reads words separated by spaces; commas are taken as for variables
        this.setAttribute('hide', (list(after) ?? []).join(' '));
        return;
      }
      if (kind === 'number') o[property] = number(after);
      else if (kind === 'list') o[property] = list(after);
      else if (kind === 'boolean') o[property] = after !== null;
      check(c, property, after);
      // A number set again applies again: the user may have moved the cursor or the view since.
      this.#change(what, before !== after || what === 'source' || kind === 'number');
    }

    /** Redraws what a change needs, once for all the changes made at the same time. */
    #change(what, really = true) {
      if (!really || what === 'none') return;
      this.#c.changes.add(what);
      this.#schedule();
    }

    /** Draws the changes made so far, once, after the code that made them. */
    #schedule() {
      const c = this.#c;
      if (c.scheduled || !this.isConnected) return;
      c.scheduled = true;
      queueMicrotask(() => this.#flush());
    }

    #flush() {
      const c = this.#c;
      c.scheduled = false;
      if (!this.isConnected) return;
      const what = c.changes;
      c.changes = new Set();
      const o = c.options;
      const known = [...LANGUAGES, ...Object.keys(isObject(o.messages) ? o.messages : {})];
      const navigatorLanguages = navigator.languages ?? [navigator.language];
      c.language = chooseLanguage([this.getAttribute('lang'), ...navigatorLanguages], known);
      c.quiet = true;
      try {
        enter(c);
        if (what.has('language')) translate();
        if (what.has('source')) {
          if (o.data !== null && o.data !== undefined) {
            stopLoad(c);
            c.input = o.data;
            c.inputName = this.fileName || null;
            load(o.data, c.inputName);
          } else if (this.src) {
            fromSrc(this.src);
          } else {
            clear();
          }
        } else if (what.has('language') && c.input !== null) {
          load(c.input, c.inputName, true);
        } else if (what.has('language') && c.error) {
          showError(c.error.say, c.error.status, false, true);
        } else if (what.has('chart') && state.model) {
          renderChart();
        }
        if (what.has('name') && !what.has('source') && state.model) {
          // Only the name shown changes: the file is not loaded again.
          c.inputName = this.fileName || (o.data === null ? c.srcName : null);
          state.file = c.inputName;
          renderHeader(state.model);
        }
        if (!state.model) return;
        if (what.has('step')) showStep(stepOf(o.step) ?? null);
        if (what.has('from') || what.has('to')) {
          // A side not given now stays where the view is (the user may have moved it).
          const from = what.has('from') && o.from !== null ? o.from : state.view[0];
          const to = what.has('to') && o.to !== null ? o.to : state.view[1];
          setView([from, to]);
        }
        if (what.has('cursor')) {
          state.cursor = o.cursor;
          updateCursor();
        }
      } finally {
        c.quiet = false;
      }
    }

    /** Loads src again (the file may have changed, or the token). */
    reload() {
      this.#change('source');
    }

    // --- the simple options

    get src() {
      return this.getAttribute('src') ?? '';
    }
    set src(v) {
      this.#setOrRemove('src', v === null || v === undefined || v === '' ? null : String(v));
    }
    get fileName() {
      return this.getAttribute('file-name') ?? '';
    }
    set fileName(v) {
      this.#setOrRemove('file-name', v === null || v === undefined || v === '' ? null : String(v));
    }
    get hide() {
      return this.getAttribute('hide') ?? '';
    }
    set hide(v) {
      const words = list(v);
      this.#setOrRemove('hide', words ? words.join(' ') : null);
    }
    get theme() {
      return this.getAttribute('theme') ?? 'auto';
    }
    set theme(v) {
      this.#setOrRemove('theme', v === null || v === undefined || v === '' ? null : String(v));
    }
    get compact() {
      return this.hasAttribute('compact');
    }
    set compact(v) {
      this.toggleAttribute('compact', Boolean(v));
    }
    get variables() {
      return this.#c.options.variables ? [...this.#c.options.variables] : null;
    }
    set variables(v) {
      this.#c.options.variables = list(v);
      this.#change('chart');
    }
    get cursor() {
      return this.#c.changes.has('cursor') || !this.#c.state.model
        ? this.#c.options.cursor
        : this.#c.state.cursor;
    }
    set cursor(v) {
      check(this.#c, 'cursor', v);
      this.#c.options.cursor = number(v);
      this.#change('cursor');
    }
    get step() {
      return this.#c.changes.has('step') || !this.#c.state.model
        ? this.#c.options.step
        : this.#c.state.step;
    }
    set step(v) {
      check(this.#c, 'step', v);
      this.#c.options.step = number(v);
      this.#change('step');
    }
    get from() {
      return this.#c.changes.has('from') || !this.#c.state.view
        ? this.#c.options.from
        : this.#c.state.view[0];
    }
    set from(v) {
      this.#c.options.from = number(v);
      check(this.#c, 'from', v);
      this.#change('from');
    }
    get to() {
      return this.#c.changes.has('to') || !this.#c.state.view
        ? this.#c.options.to
        : this.#c.state.view[1];
    }
    set to(v) {
      this.#c.options.to = number(v);
      check(this.#c, 'to', v);
      this.#change('to');
    }

    // --- the options that are properties only

    get data() {
      return this.#c.options.data;
    }
    set data(v) {
      this.#c.options.data = v === undefined ? null : v;
      this.#change('source');
    }
    get token() {
      return this.#c.options.token;
    }
    set token(v) {
      this.#c.options.token = typeof v === 'string' && v !== '' ? v : null;
      this.#change('source', !this.#c.options.data);
    }
    get headers() {
      return this.#c.options.headers;
    }
    set headers(v) {
      this.#c.options.headers = isObject(v) ? v : null;
      this.#change('source', !this.#c.options.data);
    }
    get labels() {
      return this.#c.options.labels;
    }
    set labels(v) {
      this.#c.options.labels = isObject(v) ? v : null;
      this.#change('chart');
    }
    get messages() {
      return this.#c.options.messages;
    }
    set messages(v) {
      this.#c.options.messages = isObject(v) ? v : null;
      this.#change('language');
    }

    #setOrRemove(name, value) {
      if (value === null) this.removeAttribute(name);
      else this.setAttribute(name, value);
    }
  }

  api.Element = VehicleSilTimelineElement;
  if (!customElements.get('vehicle-sil-timeline'))
    customElements.define('vehicle-sil-timeline', VehicleSilTimelineElement);
})(globalThis);
