/**
 * 読める値: いまの値（get）と、変わったときの知らせ（subscribe）。ライブラリの状態は、どれもこの形で出す。
 *
 * - 素の JavaScript からそのまま使える（枠組みに依らない）。枠組みの包み（Angular の signal など）は、get() で
 *   最初の値を取り、subscribe() で後の値を受けて写す。
 * - subscribe(listener) は、値が変わるたびに listener を呼ぶ（いまの値では呼ばない）。返す関数を呼ぶと購読をやめる。
 * - 知らせは同期で、値を変えた処理の中で届く。知らせの途中で購読をやめた listener にも、その回の知らせは届く。
 * - listener の中で同じ値を変えると、新しい値が全員に届き、古い値の残りの知らせはやめる（古い値が後から届かない）。
 * - listener が投げても、ほかの listener への知らせとライブラリの状態の進みは止まらない。投げたものは、クライアントの
 *   `onError`（createClient の設定）へ渡す。
 */
export interface Readable<T> {
  get(): T;
  subscribe(listener: (value: T) => void): () => void;
}

/** 購読者が投げた例外を受ける関数。 */
export type ErrorSink = (error: unknown) => void;

/** 既定の ErrorSink: 後で投げ直す（知らせの途中では投げない）。 */
export const rethrowLater: ErrorSink = (error) =>
  queueMicrotask(() => {
    throw error;
  });

/** listener をそれぞれ呼ぶ。投げたら onError に渡して、次へ進む。 */
export function callEach<A extends unknown[]>(
  listeners: Iterable<(...args: A) => void>,
  args: A,
  onError: ErrorSink,
  stillCurrent: () => boolean = () => true,
): void {
  for (const listener of [...listeners]) {
    if (!stillCurrent()) return;
    try {
      listener(...args);
    } catch (error) {
      try {
        onError(error);
      } catch (sinkError) {
        // onError 自身が投げても、残りの listener には届ける
        rethrowLater(sinkError);
      }
    }
  }
}

/** 書ける値。ライブラリの中で状態を持つのに使う。外へは readonly() で書けない形にして出す。 */
export class Writable<T> implements Readable<T> {
  private value: T;
  private version = 0;
  private readonly listeners = new Set<(value: T) => void>();

  constructor(
    initial: T,
    /** 同じ値とみなす比べ方。同じなら知らせない。 */
    private readonly equal: (a: T, b: T) => boolean = Object.is,
    /** 購読者が投げた例外を受ける。 */
    private readonly onError: ErrorSink = rethrowLater,
  ) {
    this.value = initial;
  }

  get(): T {
    return this.value;
  }

  set(value: T): void {
    if (this.equal(this.value, value)) return;
    this.value = value;
    const version = ++this.version;
    // 知らせの途中で値がまた変わったら、残りの購読者には新しい値が届いているので、古い値を渡さない
    callEach(this.listeners, [value], this.onError, () => this.version === version);
  }

  update(change: (value: T) => T): void {
    this.set(change(this.value));
  }

  subscribe(listener: (value: T) => void): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  /** 書けない形（get と subscribe だけ）。 */
  readonly(): Readable<T> {
    return { get: () => this.get(), subscribe: (listener) => this.subscribe(listener) };
  }
}

/**
 * いくつかの読める値から作る値。元のどれかが変わるたびに compute で作り直し、equal で違うときだけ知らせる。
 * 元の値をずっと購読する（作った値と同じ寿命のもの、たとえばクライアントの中で使う）。
 */
export function derived<T>(
  sources: readonly Readable<unknown>[],
  compute: () => T,
  equal: (a: T, b: T) => boolean = Object.is,
  onError: ErrorSink = rethrowLater,
): Readable<T> {
  const value = new Writable(compute(), equal, onError);
  for (const source of sources) source.subscribe(() => value.set(compute()));
  return value.readonly();
}

/** 浅く比べる（オブジェクトの欄がどれも Object.is で同じ）。状態のまとめ（スナップショット）に使う。 */
export function shallowEqual<T extends object>(a: T, b: T): boolean {
  if (Object.is(a, b)) return true;
  const keys = Object.keys(a) as (keyof T)[];
  return keys.length === Object.keys(b).length && keys.every((key) => Object.is(a[key], b[key]));
}
