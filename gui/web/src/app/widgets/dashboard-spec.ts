import {
  DashboardFile,
  DashboardPanelModule,
  HISTORY_MAX,
  VariableInfo,
  VariablePeer,
  VariableRef,
  hasRange,
} from '@vehicle-sil/gui-client';

export { hasRange };

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

/**
 * 持ち込みのパネル 1 つ: custom element（`panel` はその名前）。画面は要素を作り、`client`（ライブラリの
 * GuiClient）、`options`、`variables` をプロパティで渡す。要素を定義するモジュールは DashboardSpec の modules。
 */
export interface DashboardPanel {
  panel: string;
  /** 見出しに出す名前（省くと要素の名前）。 */
  label?: string;
  options?: Readonly<Record<string, unknown>>;
  /** パネルが使う変数（パネルに渡すだけ。画面は確かめない）。 */
  variables?: readonly VariableRef[];
}

/** ファイルの `auto:` に書いた participant が、いまの実行にない（その場所に理由を出す）。 */
export interface DashboardMissing {
  missing: string;
}

/** まとまりに置くもの: 部品、持ち込みのパネル、ない participant の知らせ。 */
export type DashboardEntry = DashboardItem | DashboardPanel | DashboardMissing;

/** 持ち込みのパネルか。 */
export function isPanel(entry: DashboardEntry): entry is DashboardPanel {
  return 'panel' in entry;
}

/** 変数の部品か。 */
export function isWidget(entry: DashboardEntry): entry is DashboardItem {
  return 'widget' in entry;
}

/** 部品のまとまり（見出しと、並べる順の部品）。 */
export interface DashboardSection {
  title: string;
  items: DashboardEntry[];
  /**
   * 見出しに置く「バスにつなぐ」のスイッチ（その ECU の `ecu.connected`）。外れている間は、まとまりに印と文が出る。
   */
  connection?: VariableRef;
  /** 最初は畳んでおく（畳んでいる間は部品を作らず、値も受け取らない）。 */
  collapsed?: boolean;
}

/**
 * ダッシュボード: どの変数をどの部品でどこに置くか。素のオブジェクト（JSON にそのまま書ける）。
 * 置き場所は、まとまりの順と、まとまりの中の部品の順で決まる（並べ方は画面の幅に合わせて CSS が決める）。
 */
export interface DashboardSpec {
  sections: DashboardSection[];
  /** 画面の上に出す題（ダッシュボードのファイルの title）。 */
  title?: string;
  /** 持ち込みのパネルを定義するモジュール（ファイルに書いた名前と、画面の base からの相対の URL）。パネルを描く前に読み込む。 */
  modules?: readonly DashboardPanelModule[];
}

/** vehicle-sil がどの ECU にも付ける入力: false の間、ECU はすべてのバスから外れる。 */
export const CONNECTED_VARIABLE = 'ecu.connected';

/**
 * 変数が vehicle-sil の付けたものか（照合の結果の `participants[].variables[].builtin`）を引く関数。照合の結果が
 * ない、その変数が照合の結果にない（ECU の実行ファイルが実行中に足す監視の変数など）、古いランチャーでは null。
 */
export type BuiltinLookup = (peer: string, name: string) => boolean | null;

const UNKNOWN_BUILTIN: BuiltinLookup = () => null;

/**
 * アプリが宣言した変数でない（vehicle-sil が付けた、監視の）変数か。見分け方はここ 1 か所だけ。
 * 照合の結果の `builtin` がわかればそれで決める。わからなければ、名前の前置きで分ける（`ecu.` と `peer.` は
 * 予約された名前、`j1939.<ネットワーク>.` は J1939 の統計とノード: docs/architecture.md の「Peer の監視」）。
 * 実行中に足される監視の変数（`peer.`、`j1939.`）は照合の結果にないので、前置きで分かれる。
 */
export function isMonitoring(name: string, builtin: boolean | null = null): boolean {
  return builtin ?? MONITORING_PREFIXES.some((prefix) => name.startsWith(prefix));
}

const MONITORING_PREFIXES = ['ecu.', 'peer.', 'j1939.'];

/** 見出しの「バスにつなぐ」のスイッチにする変数か（書ける bool の `ecu.connected`）。 */
function isConnection(variable: VariableInfo): boolean {
  return variable.name === CONNECTED_VARIABLE && variable.type === 'bool' && variable.writable;
}

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
 * 変数の一覧から、ECU ごとのまとまりに既定の部品を並べる（変数のない ECU は出さない）。`ecu.connected` は見出しの
 * 「バスにつなぐ」のスイッチ（connection）にし、アプリの変数を宣言の順に並べ、監視の変数（isMonitoring）は、
 * その後の畳んだまとまり（monitoringTitle(ECU の名前)）にする。
 */
export function autoDashboard(
  peers: readonly VariablePeer[],
  monitoringTitle: (peer: string) => string,
  builtin: BuiltinLookup = UNKNOWN_BUILTIN,
): DashboardSpec {
  const sections: DashboardSection[] = [];
  for (const peer of peers) {
    const item = (variable: VariableInfo): DashboardItem => ({
      peer: peer.name,
      name: variable.name,
      widget: defaultWidget(variable),
    });
    const connection = peer.variables.find(isConnection);
    const rest = peer.variables.filter((v) => v !== connection);
    const watching = (v: VariableInfo) => isMonitoring(v.name, builtin(peer.name, v.name));
    const app = rest.filter((v) => !watching(v));
    const monitoring = rest.filter(watching);
    if (app.length > 0 || connection) {
      sections.push({
        title: peer.name,
        items: app.map(item),
        ...(connection ? { connection: { peer: peer.name, name: connection.name } } : {}),
      });
    }
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
  const matchesText = (text: string) => words.every((word) => text.toLowerCase().includes(word));
  const matches = (item: DashboardEntry) => {
    const text = (
      isPanel(item)
        ? `${item.panel} ${item.label ?? ''} ${(item.variables ?? []).map((v) => `${v.peer} ${v.name}`).join(' ')}`
        : isWidget(item)
          ? `${item.peer} ${item.name} ${item.label ?? ''}`
          : `auto ${item.missing}`
    ).toLowerCase();
    return words.every((word) => text.includes(word));
  };
  return {
    ...spec,
    sections: spec.sections
      .map((section) => {
        const { connection, ...rest } = section;
        const kept =
          connection && matchesText(`${connection.peer} ${connection.name}`) ? { connection } : {};
        return { ...rest, ...kept, items: section.items.filter(matches) };
      })
      .filter((section) => section.items.length > 0 || section.connection),
  };
}

/** 部品の数（見出しのスイッチも 1 つと数える）。 */
export function countItems(spec: DashboardSpec): number {
  return spec.sections.reduce(
    (sum, section) => sum + section.items.length + (section.connection ? 1 : 0),
    0,
  );
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
  // 種類と設定の名前は利用者が書いたもの: 自分のキーだけを引く（constructor や __proto__ で原型に当てない）
  if (!Object.hasOwn(FITS, item.widget)) return { kind: 'unknown-widget' };
  const fit = FITS[item.widget];
  const known = WIDGET_OPTIONS[item.widget];
  for (const [key, value] of Object.entries(item.options ?? {})) {
    if (!Object.hasOwn(known, key)) return { kind: 'unknown-option', key };
    if (!known[key](value)) return { kind: 'bad-option', key };
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
      if (!isWidget(item)) return;
      const problem = checkItem(item, find(item));
      if (problem) problems.push({ section: s, item: i, problem });
    }),
  );
  return problems;
}

/**
 * ダッシュボードのファイル（サーバーが確かめた形。`GET api/dashboard`）と、いまの変数の一覧から、描くデータを作る。
 * - `variable`: その変数を、書いた部品で（省けば既定の部品。いまの実行にない変数は値の表示）。
 * - `panel`: 持ち込みのパネル。
 * - `auto: <participant>`: その participant のアプリの変数のうち、ファイルに書いていないもの（`variable` と
 *   パネルの `variables` のどれにもないもの）を既定の部品で、その場所に。監視の変数（isMonitoring）は、その
 *   まとまりの後の畳んだまとまり（monitoringTitle）に。同じ participant の 2 つ目の auto は何も置かない。
 * 部品が変数に合うかは、ここでは確かめない（描くときに checkItem）。
 */
export function fileDashboard(
  file: DashboardFile,
  peers: readonly VariablePeer[],
  monitoringTitle: (peer: string) => string,
  builtin: BuiltinLookup = UNKNOWN_BUILTIN,
): DashboardSpec {
  const key = (peer: string, name: string) => `${peer}\u0000${name}`;
  const placed = new Set<string>();
  for (const section of file.sections) {
    for (const item of section.items) {
      if ('variable' in item) placed.add(key(item.variable.peer, item.variable.name));
      if ('panel' in item) for (const v of item.variables) placed.add(key(v.peer, v.name));
    }
  }
  const infos = new Map<string, VariableInfo>();
  for (const peer of peers) for (const v of peer.variables) infos.set(key(peer.name, v.name), v);
  const autoDone = new Set<string>();
  const sections: DashboardSection[] = [];
  for (const section of file.sections) {
    const items: DashboardEntry[] = [];
    const monitoring: DashboardSection[] = [];
    let connection: VariableRef | undefined;
    for (const item of section.items) {
      if ('variable' in item) {
        const { peer, name } = item.variable;
        const info = infos.get(key(peer, name));
        items.push({
          peer,
          name,
          widget: (item.widget ?? (info ? defaultWidget(info) : 'value')) as WidgetKind,
          ...(item.label !== null ? { label: item.label } : {}),
          ...(Object.keys(item.options).length > 0 ? { options: item.options } : {}),
        });
      } else if ('panel' in item) {
        items.push({
          panel: item.panel,
          ...(item.label !== null ? { label: item.label } : {}),
          options: item.options,
          variables: item.variables,
        });
      } else if (!autoDone.has(item.auto)) {
        autoDone.add(item.auto);
        const peer = peers.find((p) => p.name === item.auto);
        if (!peer && peers.length > 0) items.push({ missing: item.auto });
        let rest = (peer?.variables ?? []).filter((v) => !placed.has(key(item.auto, v.name)));
        // ecu.connected: このまとまりの見出しのスイッチ（まとまりに 1 つ。2 つ目の ECU のものは部品で並ぶ）
        const own = connection ? undefined : rest.find(isConnection);
        if (own) {
          connection = { peer: item.auto, name: own.name };
          rest = rest.filter((v) => v !== own);
        }
        const entry = (v: VariableInfo): DashboardItem => ({
          peer: item.auto,
          name: v.name,
          widget: defaultWidget(v),
        });
        const watching = (v: VariableInfo) =>
          !isConnection(v) && isMonitoring(v.name, builtin(item.auto, v.name));
        items.push(...rest.filter((v) => !watching(v)).map(entry));
        const watchers = rest.filter(watching);
        if (watchers.length > 0) {
          monitoring.push({
            title: monitoringTitle(item.auto),
            items: watchers.map(entry),
            collapsed: true,
          });
        }
      }
    }
    sections.push({
      title: section.title,
      items,
      ...(section.collapsed ? { collapsed: true } : {}),
      ...(connection ? { connection } : {}),
    });
    sections.push(...monitoring);
  }
  return {
    sections,
    ...(file.title !== null ? { title: file.title } : {}),
    modules: file.panels,
  };
}
