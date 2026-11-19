import { CheckResult } from '@vehicle-sil/gui-client';
import { Document, LineCounter, YAMLMap, isMap, isSeq, parseDocument } from 'yaml';

/*
 * Designer の編集の論理。Angular に依らない関数だけで、入力は 2 つ: 構成ファイルの YAML の文と、その文の照合の答え
 * （`CheckResult`）。規則は持たない: 誤りは次の照合が言う。画面が判断に使うのは、照合の答えと、文の木の中の位置
 * （ある行が `variants.<名前>` の中にあるか）だけ。
 */

/** 開いた文書。`doc` は編集で書き換わる（画面は編集のたびに `text` から開き直す）。 */
export interface DesignerModel {
  readonly doc: Document.Parsed;
  readonly lineCounter: LineCounter;
  /** 表で編集できる（YAML として読めて、JSON の構成でない）。false のときは文の欄だけ。 */
  readonly editable: boolean;
  /** 文が YAML として読めないときの誤り（なければ空）。 */
  readonly errors: readonly { message: string; line: number | null }[];
  /** JSON の構成（先頭が `{`）。ランチャーは JSON として読むので、表で書き換えない。 */
  readonly json: boolean;
}

/** 文を開く。読めない文でも開く（`errors` に出る。表の編集は止まる）。 */
export function open(text: string): DesignerModel {
  const json = text
    .replace(/^\uFEFF/, '')
    .trimStart()
    .startsWith('{');
  const lineCounter = new LineCounter();
  const doc = parseDocument(text, { lineCounter, keepSourceTokens: false });
  const errors = doc.errors.map((e) => ({
    message: e.message.split('\n')[0],
    line: e.linePos?.[0].line ?? null,
  }));
  return { doc, lineCounter, json, errors, editable: !json && errors.length === 0 };
}

/** 書き出す。エイリアスは書かない（ランチャーはアンカーとエイリアスを読まない）。長い行を折り返さない（手で書いた形を保つ）。 */
export function text(model: DesignerModel): string {
  return model.doc.toString({ flowCollectionPadding: false, lineWidth: 0 });
}

// --- 見る ---------------------------------------------------------------------------------------------

export interface NetworkView {
  name: string;
  kind: string;
  /** 照合が言う、このネットワークにいる participant。 */
  participants: string[];
}

/** 写像の 1 行（照合の `network_map[]`）。 */
export interface MapLine {
  /** シミュレーションの名前（鍵）。 */
  name: string;
  /** 実行ファイルの名前（値）。 */
  from: string;
  line: number | null;
}

/** 実行ファイルのネットワークの名前ごとの口（同じ名前の `can()` と `j1939()` は 1 つにまとめる）。 */
export interface ChannelView {
  /** 実行ファイルの名前。 */
  from: string;
  kinds: string[];
  uses: string[];
  /** ECU が報告した MAC（なければ null: 報告なし）。 */
  mac: string | null;
  /** つながっているシミュレーションのネットワーク（写像の行がなければ、実行ファイルの名前のまま）。 */
  network: string;
  /** 写像の行（なければ null）。 */
  line: number | null;
  /** その行が `variants.<名前>` の中にあるとき、その variant の名前。表では変えられない。 */
  overriddenBy: string | null;
}

export interface ParticipantView {
  name: string;
  /** 照合の状態（`described`、`not_described` など）。 */
  status: string;
  /** 構成ファイルの `describe: false`。 */
  describeOff: boolean;
  /** 実行ファイルの申告を読めた（チャネルが分かる）。false のとき `channels` は空。 */
  known: boolean;
  channels: ChannelView[];
  /** 照合が言う写像の全部の行（チャネルが分からない participant の行も、申告にない名前の行も）。 */
  mapLines: MapLine[];
  /** 共通の部分の `participants` にいる（表から写像を書ける）。 */
  inCommon: boolean;
}

export interface DesignerView {
  editable: boolean;
  networks: NetworkView[];
  participants: ParticipantView[];
  /** 構成ファイルの variant の名前（なければ空）。 */
  variants: string[];
  /** 照合した variant（なければ null）。 */
  variant: string | null;
  /** 申告を読んでいない participant の名前（照合の状態が `not_described`）。 */
  notDescribed: string[];
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);
const asText = (value: unknown): string | null => (typeof value === 'string' ? value : null);
const asList = (value: unknown): unknown[] => (Array.isArray(value) ? value : []);

/** 文の木の中の `variants.<名前>` の行の範囲。 */
function variantRanges(model: DesignerModel): { name: string; from: number; to: number }[] {
  const variants = model.doc.get('variants', true);
  if (!isMap(variants)) return [];
  const lineAt = (offset: number) => model.lineCounter.linePos(offset).line;
  const ranges: { name: string; from: number; to: number }[] = [];
  for (const pair of variants.items) {
    const key = pair.key as { value?: unknown; range?: [number, number, number] | null };
    const value = pair.value as { range?: [number, number, number] | null } | null;
    if (!key.range) continue;
    const from = lineAt(key.range[0]);
    const end = value?.range ? value.range[1] : key.range[1];
    ranges.push({
      name: String(key.value ?? ''),
      from,
      to: Math.max(from, lineAt(Math.max(end - 1, 0))),
    });
  }
  return ranges;
}

/** 共通の部分の `participants` の名前。 */
function commonNames(model: DesignerModel): Set<string> {
  const seq = model.doc.get('participants', true);
  const names = new Set<string>();
  if (isSeq(seq)) {
    for (const item of seq.items) {
      if (isMap(item)) names.add(String(item.get('name') ?? ''));
    }
  }
  return names;
}

/** 画面に出す形にする。文と、その文の照合の答えだけを読む。 */
export function view(model: DesignerModel, check: CheckResult): DesignerView {
  const ranges = variantRanges(model);
  const inCommon = commonNames(model);
  const networks = check.networks
    .filter((n) => n.name !== null)
    .map((n) => ({ name: n.name as string, kind: n.kind ?? '', participants: n.participants }));
  const participants = check.participants
    .filter((p) => p.name !== null)
    .map((p): ParticipantView => {
      const name = p.name as string;
      const raw = p.raw ?? {};
      const reported = raw['reported'];
      const reportedNetworks =
        p.status === 'described' && isRecord(reported) ? asList(reported['networks']) : null;
      const mapLines: MapLine[] = asList(raw['network_map']).flatMap((entry) => {
        if (!isRecord(entry)) return [];
        const from = asText(entry['from']);
        const mapped = asText(entry['name']);
        const line = typeof entry['line'] === 'number' ? entry['line'] : null;
        return from !== null && mapped !== null ? [{ name: mapped, from, line }] : [];
      });
      const byName = new Map<string, { kinds: string[]; uses: string[]; mac: string | null }>();
      for (const entry of reportedNetworks ?? []) {
        if (!isRecord(entry)) continue;
        const from = asText(entry['name']);
        if (from === null) continue;
        const channel = byName.get(from) ?? { kinds: [], uses: [], mac: null };
        const kind = asText(entry['kind']);
        const use = asText(entry['use']);
        if (kind !== null) channel.kinds.push(kind);
        if (use !== null) channel.uses.push(use);
        channel.mac ??= asText(entry['mac']);
        byName.set(from, channel);
      }
      const channels = [...byName].map(([from, c]): ChannelView => {
        const row = mapLines.find((l) => l.from === from);
        const line = row?.line ?? null;
        const variant =
          line === null ? null : (ranges.find((r) => line >= r.from && line <= r.to)?.name ?? null);
        return { from, ...c, network: row?.name ?? from, line, overriddenBy: variant };
      });
      return {
        name,
        status: p.status,
        describeOff: raw['describe'] === false,
        known: reportedNetworks !== null,
        channels,
        mapLines,
        inCommon: inCommon.has(name),
      };
    });
  const variants = asList(check.raw?.['variants']).filter(
    (v): v is string => typeof v === 'string',
  );
  return {
    editable: model.editable,
    networks,
    participants,
    variants,
    variant: asText(check.raw?.['variant']),
    notDescribed: participants.filter((p) => p.status === 'not_described').map((p) => p.name),
  };
}

// --- 書き換える（共通の部分だけ。variant の中は書き換えない） --------------------------------------------------

const NODE = { aliasDuplicateObjects: false } as const;

/** 共通の部分の participant の文の木の節（なければ、表では書けない、という誤り）。 */
function participantNode(model: DesignerModel, name: string) {
  const seq = model.doc.get('participants', true);
  const node = isSeq(seq) ? seq.items.find((p) => isMap(p) && p.get('name') === name) : undefined;
  if (!isMap(node)) {
    throw new Error(
      `participant ${name} is not in the common part of the file: edit it in the text`,
    );
  }
  return node;
}

/** ネットワークを足す（`networks` の末尾）。同じ名前かどうかは見ない（次の照合が言う）。 */
export function addNetwork(model: DesignerModel, name: string, kind: string): void {
  let networks = model.doc.get('networks', true);
  if (!isSeq(networks)) {
    networks = model.doc.createNode([], NODE);
    model.doc.set('networks', networks);
  }
  if (isSeq(networks)) {
    networks.flow = false;
    networks.add(model.doc.createNode({ name, kind }, NODE));
  }
}

/**
 * ネットワークを消す。共通の部分の participant の `network_names` で、これを鍵にする行も消す（残すと次の照合が
 * 誤りにする）。variant の中の行は消さない（照合が言う）。
 */
export function removeNetwork(model: DesignerModel, name: string): void {
  const networks = model.doc.get('networks', true);
  if (isSeq(networks)) {
    const index = networks.items.findIndex((n) => isMap(n) && n.get('name') === name);
    if (index >= 0) networks.items.splice(index, 1);
  }
  const participants = model.doc.get('participants', true);
  if (!isSeq(participants)) return;
  for (const participant of participants.items) {
    if (!isMap(participant)) continue;
    const map = participant.get('network_names', true);
    if (!isMap(map)) continue;
    map.delete(name);
    if (map.items.length === 0) participant.delete('network_names');
  }
}

/**
 * `participant` の実行ファイルのネットワーク `from` を、シミュレーションのネットワーク `network` につなぐ。共通の部分の
 * `network_names` に `network: from` を書く。同じ名前なら行は要らない（ランチャーは実行ファイルの名前のままにする）ので、
 * 消す。`network` が、この participant の別の口の行の鍵なら、その行を置き換える（照合が言う）。
 */
export function wire(
  model: DesignerModel,
  participant: string,
  from: string,
  network: string,
): void {
  const node = participantNode(model, participant);
  const existing = node.get('network_names', true);
  let map: YAMLMap | null = isMap(existing) ? existing : null;
  if (map) {
    const old = map.items.find((pair) => {
      const value = pair.value as { value?: unknown } | null;
      return String(value?.value ?? value) === from;
    });
    if (old) map.delete(old.key);
  }
  if (network !== from) {
    if (!map) {
      map = model.doc.createNode({}, NODE) as YAMLMap;
      node.set('network_names', map);
    }
    map.set(network, from);
  }
  if (map && map.items.length === 0) node.delete('network_names');
}
