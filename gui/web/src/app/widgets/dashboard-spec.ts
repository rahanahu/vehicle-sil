import { HISTORY_MAX, VariableInfo, VariablePeer } from '@vehicle-sil/gui-client';

/**
 * 部品の種類。`WIDGETS`（widgets.ts）が種類ごとのコンポーネントを持つ。
 * value: 値の表示、number: 数値の入力、slider: スライダー、switch: スイッチ、lamp: ランプ、select: 選択肢、
 * graph: グラフ。
 */
export type WidgetKind = 'value' | 'number' | 'slider' | 'switch' | 'lamp' | 'select' | 'graph';

/** 部品の種類の一覧。 */
export const WIDGET_KINDS: readonly WidgetKind[] = [
  'value',
  'number',
  'slider',
  'switch',
  'lamp',
  'select',
  'graph',
];

/** ダッシュボードに置く部品 1 つ: どの ECU（peer）のどの変数を、どの部品で出すか。 */
export interface DashboardItem {
  peer: string;
  name: string;
  widget: WidgetKind;
  /** 見出しに出す名前（省くと変数の名前）。 */
  label?: string;
  /** 部品ごとの設定（素のオブジェクト。部品が知っている欄だけ。WIDGET_OPTIONS）。 */
  options?: Readonly<Record<string, unknown>>;
}

/** 部品のまとまり（見出しと、並べる順の部品）。 */
export interface DashboardSection {
  title: string;
  items: DashboardItem[];
  /** 最初は畳んでおく（畳んでいる間は部品を作らず、値も受け取らない）。 */
  collapsed?: boolean;
}

/**
 * ダッシュボード: どの変数をどの部品でどこに置くか。素のオブジェクト（JSON にそのまま書ける）。
 * 置き場所は、まとまりの順と、まとまりの中の部品の順で決まる（並べ方は画面の幅に合わせて CSS が決める）。
 */
export interface DashboardSpec {
  sections: DashboardSection[];
}

/** 範囲が決まっている（両端が有限で、下が上より小さい）か。 */
export function hasRange(info: Pick<VariableInfo, 'min' | 'max'>): boolean {
  return (
    info.min !== null &&
    info.max !== null &&
    Number.isFinite(info.min) &&
    Number.isFinite(info.max) &&
    info.min < info.max
  );
}

/**
 * ECU の実行ファイルが自分で足す監視の変数か（アプリが宣言した変数ではない）。変数の一覧には、それを示す欄が
 * まだないので、名前の前置きで分ける（`peer.` は予約された名前、`j1939.<ネットワーク>.` は J1939 の統計とノード:
 * docs/architecture.md の「Peer の監視」）。見分け方はここ 1 か所だけ。
 */
export function isMonitoring(name: string): boolean {
  return MONITORING_PREFIXES.some((prefix) => name.startsWith(prefix));
}

const MONITORING_PREFIXES = ['peer.', 'j1939.'];

/**
 * 変数の既定の部品（docs/gui.md の「既定の部品」の表）。
 * - bool: 書ければスイッチ、読むだけならランプ。
 * - enum（列挙の項目がある）: 書ければ選択肢、読むだけなら値の表示（項目の名前）。
 * - integer、real: 書けて範囲が決まっていればスライダー、書けて範囲がなければ数値の入力、読むだけならグラフ。
 * - 知らない型: 値の表示。
 * 書けるかは `writable`（role が input か parameter）で決める。parameter も書ける部品になる（書けるのは最初の
 * ステップの前だけで、その後は部品が理由を出して操作を止める）。
 */
export function defaultWidget(info: VariableInfo): WidgetKind {
  switch (info.type) {
    case 'bool':
      return info.writable ? 'switch' : 'lamp';
    case 'enum':
      return info.writable && info.enum !== null && info.enum.length > 0 ? 'select' : 'value';
    case 'integer':
    case 'real':
      if (!info.writable) return 'graph';
      return hasRange(info) ? 'slider' : 'number';
    default:
      return 'value';
  }
}

/**
 * 変数の一覧から、ECU ごとのまとまりに既定の部品を並べる（変数のない ECU は出さない）。アプリの変数を宣言の順に
 * 並べ、監視の変数（isMonitoring）は、その後の畳んだまとまり（monitoringTitle(ECU の名前)）にする。
 */
export function autoDashboard(
  peers: readonly VariablePeer[],
  monitoringTitle: (peer: string) => string,
): DashboardSpec {
  const sections: DashboardSection[] = [];
  for (const peer of peers) {
    const item = (variable: VariableInfo): DashboardItem => ({
      peer: peer.name,
      name: variable.name,
      widget: defaultWidget(variable),
    });
    const app = peer.variables.filter((v) => !isMonitoring(v.name));
    const monitoring = peer.variables.filter((v) => isMonitoring(v.name));
    if (app.length > 0) sections.push({ title: peer.name, items: app.map(item) });
    if (monitoring.length > 0) {
      sections.push({
        title: monitoringTitle(peer.name),
        items: monitoring.map(item),
        collapsed: true,
      });
    }
  }
  return { sections };
}

/**
 * 名前で絞り込む。空白で区切った語のすべてが、ECU の名前、変数の名前、見出しのどこかに入っている部品だけを残す
 * （大文字と小文字を区別しない）。部品が残らないまとまりは消す。語がなければ、そのまま返す。
 */
export function filterDashboard(spec: DashboardSpec, query: string): DashboardSpec {
  const words = query.toLowerCase().split(/\s+/).filter(Boolean);
  if (words.length === 0) return spec;
  const matches = (item: DashboardItem) => {
    const text = `${item.peer} ${item.name} ${item.label ?? ''}`.toLowerCase();
    return words.every((word) => text.includes(word));
  };
  return {
    sections: spec.sections
      .map((section) => ({ ...section, items: section.items.filter(matches) }))
      .filter((section) => section.items.length > 0),
  };
}

/** 部品の数。 */
export function countItems(spec: DashboardSpec): number {
  return spec.sections.reduce((sum, section) => sum + section.items.length, 0);
}

/** 部品ごとに受け付ける設定と、その値の確かめ方。 */
export const WIDGET_OPTIONS: Readonly<
  Record<WidgetKind, Readonly<Record<string, (value: unknown) => boolean>>>
> = {
  value: {},
  number: {},
  slider: { step: (v) => typeof v === 'number' && Number.isFinite(v) && v > 0 },
  switch: {},
  lamp: {},
  select: {},
  graph: {
    points: (v) => typeof v === 'number' && Number.isInteger(v) && v >= 2 && v <= HISTORY_MAX,
  },
};

/**
 * 部品が、その変数に使えない理由。
 * - unknown-widget: 知らない部品の種類。
 * - wrong-type: 変数の型に合わない（スイッチに実数、など。types に、その部品が使える型）。
 * - read-only: 書く部品を、読むだけの変数に当てた。
 * - no-range: スライダーを、範囲のない変数に当てた。
 * - no-items: 選択肢を、項目のない列挙に当てた。
 * - unknown-option、bad-option: 部品が知らない設定、値の合わない設定（key）。
 */
export type ItemProblem =
  | { kind: 'unknown-widget' }
  | { kind: 'wrong-type'; types: readonly string[] }
  | { kind: 'read-only' }
  | { kind: 'no-range' }
  | { kind: 'no-items' }
  | { kind: 'unknown-option' | 'bad-option'; key: string };

/** 部品が使える変数の型と、書く部品か。 */
const FITS: Readonly<Record<WidgetKind, { types: readonly string[] | null; writes: boolean }>> = {
  value: { types: null, writes: false },
  lamp: { types: ['bool'], writes: false },
  graph: { types: ['integer', 'real', 'bool'], writes: false },
  switch: { types: ['bool'], writes: true },
  select: { types: ['enum'], writes: true },
  slider: { types: ['integer', 'real'], writes: true },
  number: { types: ['integer', 'real'], writes: true },
};

/**
 * ダッシュボードの部品 1 つを、変数の定義と照らして確かめる（使えれば null）。変数の定義がない（いまの実行に
 * ない）ときは、設定だけを確かめる（部品が「この実行にない変数」と出す）。
 */
export function checkItem(item: DashboardItem, info: VariableInfo | undefined): ItemProblem | null {
  const fit = (FITS as Partial<typeof FITS>)[item.widget];
  if (!fit) return { kind: 'unknown-widget' };
  const known = WIDGET_OPTIONS[item.widget];
  for (const [key, value] of Object.entries(item.options ?? {})) {
    const check = known[key];
    if (!check) return { kind: 'unknown-option', key };
    if (!check(value)) return { kind: 'bad-option', key };
  }
  if (!info) return null;
  if (fit.types && !fit.types.includes(info.type)) return { kind: 'wrong-type', types: fit.types };
  if (fit.writes && !info.writable) return { kind: 'read-only' };
  if (item.widget === 'slider' && !hasRange(info)) return { kind: 'no-range' };
  if (item.widget === 'select' && !(info.enum && info.enum.length > 0)) return { kind: 'no-items' };
  return null;
}

/** ダッシュボード全体を確かめる: 使えない部品の場所（まとまりと部品の番号）と理由。 */
export function checkDashboard(
  spec: DashboardSpec,
  find: (item: DashboardItem) => VariableInfo | undefined,
): { section: number; item: number; problem: ItemProblem }[] {
  const problems: { section: number; item: number; problem: ItemProblem }[] = [];
  spec.sections.forEach((section, s) =>
    section.items.forEach((item, i) => {
      const problem = checkItem(item, find(item));
      if (problem) problems.push({ section: s, item: i, problem });
    }),
  );
  return problems;
}
