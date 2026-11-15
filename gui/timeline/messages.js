// The words of the timeline page (timeline.html and viewer.js), in English and Japanese. Every text that the page
// shows comes from here; names, units, values and messages from the file and the launcher are shown as they are.
// A value is a string, a function of named arguments that returns a string, or (for a paragraph with markup) an
// array of strings and [tag, text] pairs. Both languages have the same keys (tests/viewer.test.js checks it).
// A plain script like viewer.js (no build), loaded before it; in Node, module.exports.
(function (global) {
  'use strict';

  const en = {
    // --- reading a file
    'file.named': ({ name }) => `"${name}"`,
    'file.this': 'The file',
    'file.howToWrite':
      'Open the file that "vehicle_sil_launch run <configuration> --scenario <scenario> --timeline <file>" wrote.',
    'read.notJson': ({ label, error, how }) => `${label} is not JSON (${error}). ${how}`,
    'read.notTimeline': ({ label, how }) =>
      `${label} is not a timeline file: it has no "vehicle_sil_timeline" key. ${how}`,
    'read.writtenBy': ({ version }) => ` (it was written by vehicle-sil ${version})`,
    'read.otherFormat': ({ label, format, by, known }) =>
      `${label} is timeline format ${format}${by}, and this page reads format ${known} only. ` +
      'Open it with the timeline page of the vehicle-sil version that wrote it.',
    'read.otherStage': ({ label, stage }) =>
      `${label} has stage ${stage}, and this page draws only "run" (the record of a run). ` +
      'Open it with the timeline page of the vehicle-sil version that wrote it.',
    'read.noScenario': ({ label, how }) =>
      `${label} has no "scenario" object, so it is not a whole timeline file. ${how}`,
    'read.noArray': ({ label, key, how }) =>
      `${label} has no "${key}" array, so it is not a whole timeline file. ${how}`,
    'read.notObject': ({ label, key, index, how }) =>
      `${label}: ${key}[${index}] is not an object, so the file cannot be read. ${how}`,
    'read.noName': ({ label, how }) =>
      `${label}: a variable has no "name", so the file cannot be read. ${how}`,

    // --- what the page says about parts it does not draw
    'compare.and': ' and ',
    'type.real': 'a number',
    'type.integer': 'an integer',
    'type.bool': 'true or false',
    'type.enum': 'the value of an item (an integer)',
    'series.what': ({ source }) => `the ${source} series`,
    'series.unknownSource': ({ what, source }) =>
      `${what} is not drawn: this page does not know the source "${source}".`,
    'series.unknownEncoding': ({ what, encoding }) =>
      `${what} is not drawn: this page does not know the encoding "${encoding}".`,
    'series.noPoints': ({ what }) => `${what} is not drawn: it has no "points" array.`,
    'series.noColumns': ({ what }) =>
      `${what} is not drawn: its "columns" have no "at_ms" or "value".`,
    'series.badPoint': ({ what }) => `${what} is not drawn: a point is not [time, value].`,
    'series.wrongType': ({ count, what, words, type, value, at }) =>
      `${count} value${count === 1 ? '' : 's'} of ${what} ${count === 1 ? 'is' : 'are'} not ${words}, ` +
      `as its type ${type} says, so ${count === 1 ? 'it is' : 'they are'} not drawn (the first: ${value} at ${at}).`,
    'series.rounded': ({ count, what, max }) =>
      `${count} value${count === 1 ? '' : 's'} of ${what} ${count === 1 ? 'is' : 'are'} beyond ` +
      `±${max}, which this page cannot hold exactly: shown rounded.`,
    'band.unknownMode': ({ step, mode }) =>
      `the condition of step ${step} is not drawn: this page does not know the mode "${mode}".`,
    'band.unknownOp': ({ step, op }) =>
      `the condition of step ${step} is not drawn: this page does not know "${op}".`,
    'variable.unknownType': ({ type }) =>
      `its type "${type}" is not known to this page; the values are drawn as numbers.`,
    'write.notComputed': 'not computed',

    // --- the read-out beside a lane, and the states of sent values
    'word.sent': 'sent',
    'word.seen': 'seen',
    'readout.missing': ({ word }) => `${word}: values dropped at the file's limits`,
    'readout.wrongType': ({ word }) => `${word}: a value that is not of the type (see the note)`,
    'readout.value': ({ word, value }) => `${word} ${value}`,
    'readout.notKnown': ({ value }) => `${value} (not known whether the ECU applied it)`,
    'readout.notAppliedLastSent': ({ value, last }) =>
      `${value} (not applied; the ECU keeps what it had: the last value sent before is ${last}, ` +
      'and the file does not say whether it was applied)',
    'readout.notApplied': ({ value, has }) => `${value} (not applied; the ECU has ${has})`,
    'readout.hasUnknown': 'a value that is not known',
    'readout.hasEarlier': 'its earlier value',

    // --- the writes of a failed step
    'writes.heading': ({ at }) => `Writes for ${at}`,
    'writes.explainApplied': 'Applied: in the ECU, although the step failed.',
    'writes.explainNotApplied':
      'Not applied: the ECU refused it, or it came after the refused write in the same command, or it was not sent.',
    'writes.explainNotKnown': 'Not known: no result says whether the ECU applied it.',
    'writes.pointDropped': "its point was dropped at the file's limits",
    'writes.causeAnd': ({ what }) => `the cause of the failure; ${what}`,
    'writes.notSentCause': 'not sent: the cause of the failure',
    'writes.notSent': 'not sent',
    'writes.cause': 'the cause of the failure',
    'writes.applied': 'applied',
    'writes.notApplied': 'not applied',
    'writes.notKnown': 'not known',
    'writes.colVariable': 'Variable',
    'writes.colValue': 'Value',
    'writes.colInEcu': 'In the ECU',
    'writes.colCause': 'Cause',

    // --- the page around the chart
    'page.title': 'Scenario timeline',
    'page.titleOf': ({ name }) => `${name} - timeline`,
    'page.scenario': 'Scenario',
    'page.open': 'Open a file',
    'page.drop': [
      ['strong', 'Drop a timeline file here'],
      ', or choose one with ',
      ['em', 'Open a file'],
      '.',
    ],
    'page.what': [
      'A timeline file is the JSON that ',
      ['code', 'vehicle_sil_launch run <configuration> --scenario <scenario> --timeline <file>'],
      ' writes. Nothing leaves this computer: the page reads the file in the browser.',
    ],
    'page.showFailure': 'Show the failure',
    'page.whole': 'Whole run',
    'page.zoomOut': 'Zoom out',
    'page.help':
      "Steps: a bar from start to end, a tick where the result was decided, a thin line on to a wait's limit. " +
      'Shaded: the values a condition allows (green: it held; red: it did not; grey: not decided by it). ' +
      'Red dashed line: the failure. Grey on the right: after the end of the record. Sent values: red dashed where ' +
      'the ECU did not apply them, orange dotted where that is not known, and a thick grey line for the value the ECU ' +
      'kept where the file says that value was applied. Drag across the chart to zoom in, double-click for the whole ' +
      'run; hover (or focus the chart and use the arrow keys, + and - to zoom) for the values at a time.',
    'page.chartLabel':
      "The scenario's steps and variables over virtual time. The tables below hold the same contents.",
    'page.cannotShow': 'This file cannot be shown. ',
    'page.noName': '(a scenario without a name)',
    'fact.file': 'timeline file',
    'fact.scenario': 'scenario',
    'fact.configuration': 'configuration',
    'fact.variant': 'variant',
    'fact.timeStep': 'controller time step',
    'fact.recordedTo': 'recorded to',
    'fact.nothingRecorded': 'nothing recorded',
    'fact.version': 'vehicle-sil',
    'fact.param': 'param',

    // --- statuses of steps and of the scenario (the file's words, as the GUI says them)
    'status.passed': 'passed',
    'status.failed': 'failed',
    'status.unfinished': 'unfinished',
    'status.not_run': 'not run',
    'status.skipped': 'skipped',
    'status.incomplete': 'incomplete',

    // --- the summary above the chart
    'failure.heading': ({ step, at }) => `Step ${step} failed at ${at}`,
    'failure.where': ({ line, file }) => `line ${line} of ${file}`,
    'passed.heading': ({ count }) => `Passed: ${count} step${count === 1 ? '' : 's'}`,
    'passed.skipped': ({ count }) => `${count} skipped (see the table of steps).`,
    'notDecided.heading': ({ status }) => `Scenario ${status}`,
    'notDecided.notRun':
      'The scenario did not run, so there is nothing to draw. The findings below say why.',
    'notDecided.incomplete':
      'The scenario ended partway (incomplete): the run stopped before the scenario was decided. ' +
      'The findings below say why.',
    'notDecided.unknown': ({ status }) =>
      `The scenario ended with status "${status}", which this page does not know. ` +
      'The steps and findings below show what the file says.',
    'dropped.heading': 'Points are missing. ',
    'dropped.text':
      'The launcher dropped older points at the limits of the file, so these values are missing ' +
      '(shown as a shaded box with a dashed border): ',
    'dropped.item': ({ variable, source, count, from, to }) =>
      `${variable} (${source}): ${count} points from ${from} to ${to}`,
    'late.heading': 'ECU values arrived late in this run ',
    'late.text':
      '(control-link-late-values). The value series of another run of the same scenario can differ.',
    'conditions.heading': 'Not met',
    'conditions.was': ({ value }) => `it was ${value}`,
    'conditions.sampleOf': ({ at }) => ` (sample of ${at})`,
    'conditions.noSample': 'no sample came in the interval',
    'conditions.noValue': 'it had no value yet',
    'participants.label': 'Participants',

    // --- the tables below the chart
    'tables.steps': 'Steps',
    'tables.lines': 'Input lines',
    'tables.findings': 'Findings',
    'tables.count': ({ caption, count }) => `${caption} (${count})`,
    'tables.none': 'None.',
    'col.index': '#',
    'col.line': 'Line',
    'col.kind': 'Kind',
    'col.status': 'Status',
    'col.start': 'Start',
    'col.decided': 'Decided',
    'col.end': 'End',
    'col.step': 'Step',
    'col.result': 'Result',
    'col.variable': 'Variable',
    'col.from': 'From',
    'col.to': 'To',
    'col.base': 'Base',
    'col.text': 'Text',
    'col.severity': 'Severity',
    'col.code': 'Code',
    'col.message': 'Message',
    'step.expectSkipped': ({ why }) => `expect not checked: ${why}`,
    'step.showOnChart': 'Show this step on the chart',
    'line.inputs': 'inputs',
    'finding.codeOnly': '(code only)',

    // --- the chart
    'chart.inputs': 'Inputs: what the scenario sent (and saw)',
    'chart.others': 'Outputs and other variables: what the scenario saw',
    'chart.showing': ({ from, to, start, end }) => `Showing ${from} to ${to} of ${start} to ${end}`,
    'chart.virtualTime': 'Virtual time',
    'chart.cursor': ({ at }) => `Cursor ${at}`,
    'chart.steps': 'Steps',
    'step.title': ({ index, kind, line, status }) =>
      `Step ${index} (${kind}, line ${line}): ${status}`,
    'step.times': ({ start, decided, end }) => `start ${start}, decided ${decided}, end ${end}`,
    'lane.range': ({ min, max }) => `${min} to ${max}`,
    'lane.ecuStep': ({ ms }) => `step ${ms}`,
    'lane.label': ({ name }) => `${name}: values over virtual time`,
    'lane.note': ({ text }) => `Note: ${text}`,
    'lane.noValues': 'no values recorded',
    'lane.dropped': ({ count, source, from, to }) =>
      `${count} points of the ${source} series were dropped at the file's limits: values are missing from ` +
      `${from} to ${to}`,
    'legend.sent': 'sent',
    'legend.sentUpper': 'sent (upper)',
    'legend.seen': 'seen',
    'legend.seenLower': 'seen (lower)',
    'legend.notApplied': 'not applied',
    'legend.notKnown': 'not known',
    'legend.held': 'kept in the ECU',
    'legend.base': 'base',
    'fit.toDeclared': 'Scale to the declared range (min to max)',
    'fit.toValues': 'Scale to the values',
    'fit.declared': 'declared range',
    'fit.values': 'fit values',
    'line.title': ({ text, where, line }) => `${text} (${where}, line ${line})`,
    'line.step': ({ step }) => `step ${step}`,
    'state.notApplied': 'Sent, and the ECU did not apply it',
    'state.notKnown': 'Sent; not known whether the ECU applied it',
    'state.held':
      'The value that stayed in the ECU: it was applied, and the writes after it were not',
    'state.at': ({ title, value, at }) => `${title}: ${value} at ${at}`,
    'state.fromTo': ({ title, value, from, to }) => `${title}: ${value} from ${from} to ${to}`,
    'state.from': ({ value, from }) => `${value} from ${from}`,
    'state.notAppliedSuffix': ' (not applied)',
    'state.notKnownSuffix': ' (not known)',
    'band.at': ({ at }) => `at ${at}`,
    'band.range': ({ from, to }) => `${from} to ${to}`,
    'band.limit': ({ limit }) => `, limit ${limit}`,
    'band.didNotHold': 'did not hold',
    'band.held': 'held',
    'band.stepStatus': ({ status }) => `step ${status}`,
    'band.title': ({ step, mode, compare, range, limit, result }) =>
      `Step ${step} (${mode}): ${compare}, ${range}${limit}: ${result}`,
    'band.expect': ({ compare }) => `expect ${compare}`,
    'mark.noValueAt': ({ at }) => `No value at ${at}`,
    'mark.was': ({ value, at }) => `It was ${value} (sample of ${at}): the condition did not hold`,
    'mark.noValue': 'no value',
    'mark.applied': 'applied',
    'mark.notSent': 'not sent',
    'mark.notApplied': 'not applied',
    'mark.notKnownWhether': 'not known whether applied',
    'mark.notKnown': 'not known',
    'mark.cause': ' (the cause of the failure)',
    'mark.write': ({ value, at, word, cause }) => `Write of ${value} for ${at}: ${word}${cause}`,
    'mark.label': ({ value, word }) => `${value} ${word}`,

    // --- loading
    'load.cannotRead': ({ name, error }) =>
      `"${name}" could not be read (${error}). Choose the file again.`,
    'load.fileProtocol':
      '?src= works only when the page comes from a web server, and this page was opened as a file. ' +
      'Drop the timeline file on the page instead.',
    'load.notAddress': ({ src }) =>
      `?src=${src} is not an address. Name the timeline file on this server, or drop the file on the page.`,
    'load.otherOrigin': ({ origin, other }) =>
      `?src= must name a file on this page's own server (${origin}), and ${other} is another one, so the file ` +
      'was not loaded. Download the file and drop it on the page instead.',
    'load.failed': ({ url, error }) =>
      `${url} could not be loaded (${error}). Check the address, or drop the file on the page.`,
    'load.unauthorized':
      "HTTP 401: the GUI server did not accept this page's token. Open the timeline again with the GUI's " +
      '"Timeline" link',
    'load.http': ({ status, message }) => `HTTP ${status}${message ? `: ${message}` : ''}`,
  };

  const ja = {
    // --- ファイルを読む
    'file.named': ({ name }) => `「${name}」`,
    'file.this': 'このファイル',
    'file.howToWrite':
      '「vehicle_sil_launch run <構成ファイル> --scenario <シナリオ> --timeline <ファイル>」が書いたファイルを開いてください。',
    'read.notJson': ({ label, error, how }) => `${label}は JSON ではありません（${error}）。${how}`,
    'read.notTimeline': ({ label, how }) =>
      `${label}は時間の軸のファイルではありません。キー "vehicle_sil_timeline" がありません。${how}`,
    'read.writtenBy': ({ version }) => `（vehicle-sil ${version} が書いたもの）`,
    'read.otherFormat': ({ label, format, by, known }) =>
      `${label}は時間の軸の形の版 ${format}${by}で、このページが読むのは版 ${known} だけです。` +
      'ファイルを書いた版の vehicle-sil の、時間の軸のページで開いてください。',
    'read.otherStage': ({ label, stage }) =>
      `${label}の stage は ${stage} で、このページが描くのは "run"（実行の記録）だけです。` +
      'ファイルを書いた版の vehicle-sil の、時間の軸のページで開いてください。',
    'read.noScenario': ({ label, how }) =>
      `${label}にはオブジェクト "scenario" がないので、時間の軸のファイルの全体ではありません。${how}`,
    'read.noArray': ({ label, key, how }) =>
      `${label}には配列 "${key}" がないので、時間の軸のファイルの全体ではありません。${how}`,
    'read.notObject': ({ label, key, index, how }) =>
      `${label}: ${key}[${index}] がオブジェクトではないので、ファイルを読めません。${how}`,
    'read.noName': ({ label, how }) =>
      `${label}: "name" のない変数があるので、ファイルを読めません。${how}`,

    // --- 描かない部分について
    'compare.and': ' かつ ',
    'type.real': '数',
    'type.integer': '整数',
    'type.bool': 'true か false',
    'type.enum': '項目の値（整数）',
    'series.what': ({ source }) => `系列 ${source}`,
    'series.unknownSource': ({ what, source }) =>
      `${what}は描きません。このページは source "${source}" を知りません。`,
    'series.unknownEncoding': ({ what, encoding }) =>
      `${what}は描きません。このページは encoding "${encoding}" を知りません。`,
    'series.noPoints': ({ what }) => `${what}は描きません。配列 "points" がありません。`,
    'series.noColumns': ({ what }) =>
      `${what}は描きません。"columns" に "at_ms" か "value" がありません。`,
    'series.badPoint': ({ what }) => `${what}は描きません。[時刻, 値] でない点があります。`,
    'series.wrongType': ({ count, what, words, type, value, at }) =>
      `${what}の値のうち ${count} 個は、型 ${type} が言う${words}ではないので、描きません（最初のもの: ${at} の ${value}）。`,
    'series.rounded': ({ count, what, max }) =>
      `${what}の値のうち ${count} 個は ±${max} を超えていて、このページでは正確に持てません。丸めて出します。`,
    'band.unknownMode': ({ step, mode }) =>
      `手順 ${step} の条件は描きません。このページは mode "${mode}" を知りません。`,
    'band.unknownOp': ({ step, op }) =>
      `手順 ${step} の条件は描きません。このページは "${op}" を知りません。`,
    'variable.unknownType': ({ type }) =>
      `型 "${type}" をこのページは知りません。値は数として描きます。`,
    'write.notComputed': '計算されていない',

    // --- 段の横の読み値と、送った値の状態
    'word.sent': '送った値',
    'word.seen': '見た値',
    'readout.missing': ({ word }) => `${word}: ファイルの上限で捨てた値`,
    'readout.wrongType': ({ word }) => `${word}: 型に合わない値（注記を見てください）`,
    'readout.value': ({ word, value }) => `${word} ${value}`,
    'readout.notKnown': ({ value }) => `${value}（ECU に入ったか不明）`,
    'readout.notAppliedLastSent': ({ value, last }) =>
      `${value}（入らなかった。ECU は前の値のまま。その前に送った最後の値は ${last} で、` +
      'それが入ったかはファイルにありません）',
    'readout.notApplied': ({ value, has }) => `${value}（入らなかった。ECU の値は ${has}）`,
    'readout.hasUnknown': 'わからない値',
    'readout.hasEarlier': '前の値',

    // --- 失敗した手順の書き込み
    'writes.heading': ({ at }) => `${at} の書き込み`,
    'writes.explainApplied': '入った: 手順は失敗したが、ECU に入った。',
    'writes.explainNotApplied':
      '入らなかった: ECU が断った、同じコマンドで断られた書き込みより後だった、または送っていない。',
    'writes.explainNotKnown': '不明: ECU に入ったかを言う結果がない。',
    'writes.pointDropped': 'その点はファイルの上限で捨てた',
    'writes.causeAnd': ({ what }) => `失敗の原因。${what}`,
    'writes.notSentCause': '送っていない: 失敗の原因',
    'writes.notSent': '送っていない',
    'writes.cause': '失敗の原因',
    'writes.applied': '入った',
    'writes.notApplied': '入らなかった',
    'writes.notKnown': '不明',
    'writes.colVariable': '変数',
    'writes.colValue': '値',
    'writes.colInEcu': 'ECU に',
    'writes.colCause': '原因',

    // --- 図のまわり
    'page.title': 'シナリオの時間の軸',
    'page.titleOf': ({ name }) => `${name} - 時間の軸`,
    'page.scenario': 'シナリオ',
    'page.open': 'ファイルを開く',
    'page.drop': [
      ['strong', '時間の軸のファイルをここに落とす'],
      'か、',
      ['em', 'ファイルを開く'],
      'で選びます。',
    ],
    'page.what': [
      '時間の軸のファイルは、',
      ['code', 'vehicle_sil_launch run <構成ファイル> --scenario <シナリオ> --timeline <ファイル>'],
      ' が書く JSON です。ページはファイルをブラウザの中で読み、このコンピューターの外に何も送りません。',
    ],
    'page.showFailure': '失敗の所',
    'page.whole': '実行の全体',
    'page.zoomOut': '縮小',
    'page.help':
      '手順: 始まりから終わりまでの棒、結果が決まった所の縦線、待つ手順の上限までの細い線。' +
      '帯: 条件が許す値（緑は成り立った、赤は成り立たなかった、灰はその条件では決まらなかった）。' +
      '赤の破線: 失敗の時刻。右の灰: 記録の終わりより後。送った値: ECU に入らなかったものは赤の破線、' +
      '入ったか不明なものはオレンジの点線、入ったとファイルが言う値のうち ECU に残った値は太い灰の線。' +
      '図の上をドラッグすると拡大し、ダブルクリックで実行の全体に戻ります。' +
      'ある時刻の値は、図の上に置くと出ます（図にフォーカスして矢印キー、+ と - で拡大と縮小）。',
    'page.chartLabel': 'シナリオの手順と変数を仮想時刻で並べた図。下の表に同じ内容があります。',
    'page.cannotShow': 'このファイルは表示できません。',
    'page.noName': '（名前のないシナリオ）',
    'fact.file': '時間の軸のファイル',
    'fact.scenario': 'シナリオ',
    'fact.configuration': '構成ファイル',
    'fact.variant': 'variant',
    'fact.timeStep': 'コントローラーの時間の刻み',
    'fact.recordedTo': '記録の終わり',
    'fact.nothingRecorded': '記録なし',
    'fact.version': 'vehicle-sil',
    'fact.param': 'パラメーター',

    // --- 手順とシナリオの状態（GUI と同じ言い方）
    'status.passed': '通った',
    'status.failed': '失敗',
    'status.unfinished': '途中で終わった',
    'status.not_run': '動かなかった',
    'status.skipped': '飛ばした',
    'status.incomplete': '終わる前に止まった',

    // --- 図の上の要約
    'failure.heading': ({ step, at }) => `手順 ${step} が ${at} で失敗しました`,
    'failure.where': ({ line, file }) => `${file} の ${line} 行`,
    'passed.heading': ({ count }) => `通りました: ${count} 個の手順`,
    'passed.skipped': ({ count }) => `${count} 個を飛ばしました（手順の表を見てください）。`,
    'notDecided.heading': ({ status }) => `シナリオ: ${status}`,
    'notDecided.notRun':
      'シナリオは動かなかったので、描くものがありません。理由は下の指摘にあります。',
    'notDecided.incomplete':
      'シナリオは途中で終わりました（incomplete）。シナリオの結果が決まる前に実行が止まりました。' +
      '理由は下の指摘にあります。',
    'notDecided.unknown': ({ status }) =>
      `シナリオは状態 "${status}" で終わりましたが、このページはこの状態を知りません。` +
      'ファイルにあるものを、下の手順と指摘に出します。',
    'dropped.heading': '点が欠けています。',
    'dropped.text':
      'ランチャーがファイルの上限で古い点を捨てたので、次の値が欠けています（破線の枠の、塗った箱で示します）: ',
    'dropped.item': ({ variable, source, count, from, to }) =>
      `${variable}（${source}）: ${from} から ${to} までの ${count} 点`,
    'late.heading': 'この実行では、ECU の値が遅れて届きました',
    'late.text':
      '（control-link-late-values）。同じシナリオの別の実行では、値の系列が違うことがあります。',
    'conditions.heading': '成り立たなかった条件',
    'conditions.was': ({ value }) => `値は ${value}`,
    'conditions.sampleOf': ({ at }) => `（${at} のサンプル）`,
    'conditions.noSample': '区間の中にサンプルが来なかった',
    'conditions.noValue': 'まだ値がなかった',
    'participants.label': 'participant',

    // --- 図の下の表
    'tables.steps': '手順',
    'tables.lines': '入力の線',
    'tables.findings': '指摘',
    'tables.count': ({ caption, count }) => `${caption}（${count}）`,
    'tables.none': 'ありません。',
    'col.index': '#',
    'col.line': '行',
    'col.kind': '種類',
    'col.status': '状態',
    'col.start': '始まり',
    'col.decided': '決まった',
    'col.end': '終わり',
    'col.step': '手順',
    'col.result': '結果',
    'col.variable': '変数',
    'col.from': 'から',
    'col.to': 'まで',
    'col.base': '基準',
    'col.text': '文',
    'col.severity': '重さ',
    'col.code': 'code',
    'col.message': '文面',
    'step.expectSkipped': ({ why }) => `expect を確かめていない: ${why}`,
    'step.showOnChart': 'この手順を図で見る',
    'line.inputs': '入力',
    'finding.codeOnly': '（code だけ）',

    // --- 図
    'chart.inputs': '入力: シナリオが送った値（と見た値）',
    'chart.others': '出力とほかの変数: シナリオが見た値',
    'chart.showing': ({ from, to, start, end }) =>
      `${start} から ${end} のうち、${from} から ${to} を表示`,
    'chart.virtualTime': '仮想時刻',
    'chart.cursor': ({ at }) => `カーソル ${at}`,
    'chart.steps': '手順',
    'step.title': ({ index, kind, line, status }) =>
      `手順 ${index}（${kind}、${line} 行）: ${status}`,
    'step.times': ({ start, decided, end }) =>
      `始まり ${start}、決まった ${decided}、終わり ${end}`,
    'lane.range': ({ min, max }) => `${min} から ${max}`,
    'lane.ecuStep': ({ ms }) => `刻み ${ms}`,
    'lane.label': ({ name }) => `${name}: 仮想時刻での値`,
    'lane.note': ({ text }) => `注記: ${text}`,
    'lane.noValues': '値の記録なし',
    'lane.dropped': ({ count, source, from, to }) =>
      `系列 ${source} の ${count} 点を、ファイルの上限で捨てました。${from} から ${to} までの値が欠けています`,
    'legend.sent': '送った値',
    'legend.sentUpper': '送った値（上）',
    'legend.seen': '見た値',
    'legend.seenLower': '見た値（下）',
    'legend.notApplied': '入らなかった',
    'legend.notKnown': '不明',
    'legend.held': 'ECU に残った値',
    'legend.base': '基準',
    'fit.toDeclared': '宣言の範囲（min から max）に合わせる',
    'fit.toValues': '値に合わせる',
    'fit.declared': '宣言の範囲',
    'fit.values': '値に合わせた',
    'line.title': ({ text, where, line }) => `${text}（${where}、${line} 行）`,
    'line.step': ({ step }) => `手順 ${step}`,
    'state.notApplied': '送ったが、ECU に入らなかった',
    'state.notKnown': '送ったが、ECU に入ったか不明',
    'state.held': 'ECU に残った値: これは入り、その後の書き込みは入らなかった',
    'state.at': ({ title, value, at }) => `${title}: ${at} の ${value}`,
    'state.fromTo': ({ title, value, from, to }) => `${title}: ${from} から ${to} まで ${value}`,
    'state.from': ({ value, from }) => `${from} から ${value}`,
    'state.notAppliedSuffix': '（入らなかった）',
    'state.notKnownSuffix': '（不明）',
    'band.at': ({ at }) => `${at} で`,
    'band.range': ({ from, to }) => `${from} から ${to}`,
    'band.limit': ({ limit }) => `、上限 ${limit}`,
    'band.didNotHold': '成り立たなかった',
    'band.held': '成り立った',
    'band.stepStatus': ({ status }) => `手順は ${status}`,
    'band.title': ({ step, mode, compare, range, limit, result }) =>
      `手順 ${step}（${mode}）: ${compare}、${range}${limit}: ${result}`,
    'band.expect': ({ compare }) => `expect ${compare}`,
    'mark.noValueAt': ({ at }) => `${at} に値がない`,
    'mark.was': ({ value, at }) => `値は ${value}（${at} のサンプル）: 条件は成り立たなかった`,
    'mark.noValue': '値なし',
    'mark.applied': '入った',
    'mark.notSent': '送っていない',
    'mark.notApplied': '入らなかった',
    'mark.notKnownWhether': '入ったか不明',
    'mark.notKnown': '不明',
    'mark.cause': '（失敗の原因）',
    'mark.write': ({ value, at, word, cause }) => `${at} の ${value} の書き込み: ${word}${cause}`,
    'mark.label': ({ value, word }) => `${value} ${word}`,

    // --- 読み込み
    'load.cannotRead': ({ name, error }) =>
      `「${name}」を読めませんでした（${error}）。ファイルを選び直してください。`,
    'load.fileProtocol':
      '?src= は、ページを Web サーバーから開いたときだけ使えます。このページはファイルとして開いています。' +
      '時間の軸のファイルをページに落としてください。',
    'load.notAddress': ({ src }) =>
      `?src=${src} はアドレスではありません。このサーバーの時間の軸のファイルを指すか、ファイルをページに落としてください。`,
    'load.otherOrigin': ({ origin, other }) =>
      `?src= は、このページと同じサーバー（${origin}）のファイルを指す必要があります。${other} は別のサーバーなので、` +
      '読みませんでした。ファイルをダウンロードして、ページに落としてください。',
    'load.failed': ({ url, error }) =>
      `${url} を読み込めませんでした（${error}）。アドレスを確かめるか、ファイルをページに落としてください。`,
    'load.unauthorized':
      'HTTP 401: GUI サーバーが、このページのトークンを受け付けませんでした。GUI の「時間の軸」のリンクから開き直してください',
    'load.http': ({ status, message }) => `HTTP ${status}${message ? `: ${message}` : ''}`,
  };

  const MESSAGES = { en, ja };
  if (typeof module === 'object' && module.exports) module.exports = MESSAGES;
  else global.VehicleSilTimelineMessages = MESSAGES;
})(typeof globalThis !== 'undefined' ? globalThis : this);
