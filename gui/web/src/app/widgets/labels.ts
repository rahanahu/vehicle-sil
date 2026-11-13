// 部品の言葉。状態の名前（機械が読む値）から、画面の言葉を作る。
import {
  ValueCondition,
  VariableInfo,
  VariablesStatus,
  WriteBlock,
  WriteStatus,
} from '@vehicle-sil/gui-client';
import { formatMeasure } from '../core/format/measure-pipe';
import { ItemProblem, WidgetKind } from './dashboard-spec';

/** bool と、有限でない実数の言葉（formatValue に渡す）。 */
export const VALUE_WORDS = {
  on: $localize`オン`,
  off: $localize`オフ`,
  notFinite: $localize`数でない値`,
};

const ROLES: Record<string, string> = {
  input: $localize`入力`,
  output: $localize`出力`,
  parameter: $localize`パラメーター`,
};

/** 変数の役割の名前（知らない役割はそのまま）。 */
export function roleLabel(info: VariableInfo | undefined): string {
  return info ? (Object.hasOwn(ROLES, info.role) ? ROLES[info.role] : info.role) : '';
}

/** 仮想時間（ミリ秒）を秒で。 */
const seconds = (ms: number | null) => formatMeasure(ms === null ? null : ms / 1000, 3);

const UNAVAILABLE: Record<string, string> = {
  unknown: $localize`実行していません`,
  off: $localize`この実行では使えません`,
  connecting: $localize`ランチャーにつないでいます`,
  failed: $localize`ランチャーにつながりません`,
};

function unavailableText(status: VariablesStatus): string {
  return UNAVAILABLE[status] ?? $localize`使えません`;
}

/** 印（色に頼らないための文字。読み上げない）と文。 */
export interface MarkedText {
  mark: string;
  text: string;
}

/** 値の状態の文。live は、値を読んだステップの時刻。 */
export function conditionText(condition: ValueCondition): MarkedText {
  switch (condition.kind) {
    case 'unavailable':
      return { mark: '—', text: unavailableText(condition.status) };
    case 'missing':
      return { mark: '⚠', text: $localize`この実行にない変数です` };
    case 'waiting':
      return { mark: '…', text: $localize`値はまだありません` };
    case 'live':
      return { mark: '', text: $localize`${seconds(condition.reading.sampleMs)}:time: s の値` };
    case 'paused': {
      const at = seconds(condition.reading.simMs ?? condition.reading.sampleMs);
      return { mark: '⏸', text: $localize`止まっている時刻 ${at}:time: s の値` };
    }
    case 'stale': {
      const why = {
        connection: $localize`古い値: 画面とサーバーの接続が切れています`,
        reconnecting: $localize`古い値: ランチャーにつなぎ直しています`,
        peer: $localize`古い値: ECU がつながっていません`,
        reading: $localize`古い値: 値を読めていません`,
      }[condition.why];
      return { mark: '⚠', text: why };
    }
    case 'ended':
      return { mark: '■', text: $localize`終わった実行の最後の値` };
  }
}

const BLOCKS: Record<WriteBlock, string> = {
  'read-only': $localize`読むだけの変数です`,
  disconnected: $localize`サーバーとの接続が切れているので書けません`,
  'not-ready': $localize`いまは書けません（実行がないか、ランチャーにつながっていません）`,
  ended: $localize`実行が終わったので書けません`,
  missing: $localize`この実行にない変数なので書けません`,
  'peer-disconnected': $localize`ECU がつながっていないので書けません`,
  'no-range': $localize`範囲（最小と最大）のない変数なので、スライダーは使えません（数値の入力を使ってください）`,
  'no-value': $localize`値がまだないので動かせません（値が届くと動かせます）`,
  parameter: $localize`パラメーターは、止めて始めた実行の最初のステップの前だけ書けます`,
  scenario: $localize`シナリオの実行中は書けません（見るだけです）`,
};

/** 書けない理由の文。 */
export const blockText = (block: WriteBlock): string => BLOCKS[block];

/** 書き込みの様子の文（idle は空）。english: 文の後ろに添える英語の文（サーバーとライブラリの文）。 */
export function writeText(status: WriteStatus): MarkedText & { english: string | null } {
  switch (status.kind) {
    case 'idle':
      return { mark: '', text: '', english: null };
    case 'sending':
      return { mark: '…', text: $localize`送っています`, english: null };
    case 'queued':
      return {
        mark: '…',
        text: $localize`受け付けました。ECU のステップで反映されるのを待っています`,
        english: null,
      };
    case 'applied':
      return status.atMs === null
        ? { mark: '✓', text: $localize`反映しました`, english: null }
        : {
            mark: '✓',
            text: $localize`${seconds(status.atMs)}:time: s のステップで反映しました`,
            english: null,
          };
    case 'invalid':
      return { mark: '✗', text: status.message, english: null };
    case 'dropped':
      // ライブラリの文（英語）は使わず、画面の言葉で言う
      return { mark: '✗', text: droppedText(), english: null };
    case 'refused':
      return { mark: '✗', text: $localize`サーバーが断りました:`, english: status.message };
    case 'failed':
      return { mark: '✗', text: $localize`反映されませんでした:`, english: status.message };
    case 'no-answer':
      return {
        mark: '?',
        text: $localize`答えが来ませんでした。反映されたかわかりません（送り直していません）。値を確かめてから書き直してください:`,
        english: status.message,
      };
    case 'closed':
      return {
        mark: '?',
        text: $localize`接続を閉じたので、反映されたかわかりません`,
        english: null,
      };
  }
}

/** 覚えていた値を送らなかった（書き込みの様子が dropped）ときの文。 */
const droppedText = () =>
  $localize`書けなくなったので、待たせていた値は送っていません。値を確かめてから書き直してください`;

const NUMBER_PROBLEMS = {
  'not-a-number': () => $localize`数を入れてください`,
  'not-integer': () => $localize`整数を入れてください`,
  below: (min: string) => $localize`${min}:min: 以上にしてください`,
  above: (max: string) => $localize`${max}:max: 以下にしてください`,
};

/** 数値の入力の誤りの文。 */
export function numberProblemText(
  problem: 'not-a-number' | 'not-integer' | 'below' | 'above',
  info: Pick<VariableInfo, 'min' | 'max'>,
): string {
  switch (problem) {
    case 'below':
      return NUMBER_PROBLEMS.below(String(info.min));
    case 'above':
      return NUMBER_PROBLEMS.above(String(info.max));
    default:
      return NUMBER_PROBLEMS[problem]();
  }
}

const WIDGET_NAMES: Record<string, string> = {
  value: $localize`値の表示`,
  number: $localize`数値の入力`,
  slider: $localize`スライダー`,
  switch: $localize`スイッチ`,
  lamp: $localize`ランプ`,
  select: $localize`選択肢`,
  graph: $localize`グラフ`,
};

/** 部品の種類の名前（知らない種類はそのまま）。 */
export const widgetName = (kind: WidgetKind | string): string =>
  Object.hasOwn(WIDGET_NAMES, kind) ? WIDGET_NAMES[kind] : kind;

/** 部品がその変数に使えない理由の文（直し方を含む）。 */
export function itemProblemText(problem: ItemProblem, widget: string, type: string): string {
  const name = widgetName(widget);
  switch (problem.kind) {
    case 'unknown-widget': {
      const kinds = Object.keys(WIDGET_NAMES).join(', ');
      return $localize`部品の種類「${widget}:widget:」はありません。${kinds}:kinds: のどれかにしてください`;
    }
    case 'wrong-type': {
      const types = problem.types.join(', ');
      return $localize`${name}:widget: は ${type}:type: の変数に使えません（使える型: ${types}:types:）`;
    }
    case 'read-only':
      return $localize`${name}:widget: は書く部品なので、読むだけの変数に使えません（値の表示かグラフにしてください）`;
    case 'no-range':
      return $localize`スライダーは、範囲（最小と最大）のない変数に使えません（数値の入力にしてください）`;
    case 'no-items':
      return $localize`選択肢は、項目のない列挙に使えません（値の表示にしてください）`;
    case 'unknown-option': {
      const key = problem.key;
      return $localize`${name}:widget: に設定「${key}:key:」はありません`;
    }
    case 'bad-option': {
      const key = problem.key;
      return $localize`${name}:widget: の設定「${key}:key:」の値が合いません`;
    }
  }
}

/** auto: に書いた participant が、いまの実行にない理由の文。 */
export const missingParticipantText = (peer: string): string =>
  $localize`auto: の participant「${peer}:peer:」は、この実行にありません。名前を確かめてください（実行の画面の participant の表にある名前）`;

/** 監視の変数のまとまりの見出し。 */
export const monitoringTitle = (peer: string): string => $localize`${peer}:peer: の監視`;

/** 持ち込みのパネルを置けない理由（failures: 読み込めなかったモジュールの、ファイルに書いた名前と理由）。 */
export type PanelProblem =
  | { kind: 'not-defined'; name: string; failures: readonly { module: string; message: string }[] }
  | { kind: 'failed'; name: string; message: string }
  | { kind: 'load-failed'; name: string; failures: readonly { module: string; message: string }[] };

/** 持ち込みのパネルを置けない理由の文（直し方を含む。モジュールの誤りの文は英語のまま）。 */
export function panelProblemText(problem: PanelProblem): string {
  const name = problem.name;
  if (problem.kind === 'failed') {
    const message = problem.message;
    return $localize`パネル <${name}:name:> を作れませんでした: ${message}:message:`;
  }
  if (problem.kind === 'load-failed') {
    const failures = problem.failures.map((f) => `${f.module}: ${f.message}`).join('; ');
    return $localize`読み込めなかったモジュールがあります。パネル <${name}:name:> は、この画面で前に読み込んだ定義で描いています（別の構成ファイルの、同じ名前のパネルかもしれません。ページを読み直すと、このファイルのモジュールで描きます）: ${failures}:failures:`;
  }
  if (problem.failures.length > 0) {
    const failures = problem.failures.map((f) => `${f.module}: ${f.message}`).join('; ');
    return $localize`パネル <${name}:name:> のモジュールを読み込めませんでした（ブラウザの開発者ツールのコンソールに詳細があります）: ${failures}:failures:`;
  }
  return $localize`パネル <${name}:name:> を定義したモジュールがありません。ダッシュボードのファイルの panels: に挙げたモジュールで、この名前の custom element を定義してください（customElements.define）`;
}

/** ECU の見出しの「バスにつなぐ」のスイッチの、読み上げの名前。 */
export const busToggleLabel = (peer: string): string => $localize`${peer}:peer: をバスにつなぐ`;
