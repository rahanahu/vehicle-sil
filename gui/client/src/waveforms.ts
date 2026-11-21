import { clientErrorSink, type GuiClient } from './client';
import { Writable, type Readable } from './values';
import type { HistoryPoint, VariableRef, VariableWatch, WatchOptions } from './variables';

export interface WaveformSeries {
  readonly ref: Readonly<VariableRef>;
  readonly points: readonly Readonly<HistoryPoint>[];
}

export interface Waveforms {
  readonly series: Readable<readonly WaveformSeries[]>;
  setSignals(refs: readonly VariableRef[]): void;
  close(): void;
}

/** Keeps histories for the selected signals of the current run in selection order. */
export function createWaveforms(client: GuiClient, options: WatchOptions = {}): Waveforms {
  const settings = { ...options, history: options.history ?? 600 };
  const series = new Writable<readonly WaveformSeries[]>([], Object.is, clientErrorSink(client));
  let historyTarget = client.session.currentTarget();
  let selection: VariableRef[] = [];
  let active: VariableWatch | null = null;
  let unsubscribe: (() => void) | undefined;
  let closed = false;

  const setSignals = (refs: readonly VariableRef[]) => {
    if (closed) return;
    const seen = new Set<string>();
    const next = refs.flatMap(({ peer, name }) => {
      const key = JSON.stringify([peer, name]);
      if (seen.has(key)) return [];
      seen.add(key);
      return [{ peer, name }];
    });
    if (
      next.length === selection.length &&
      next.every((ref, i) => ref.peer === selection[i].peer && ref.name === selection[i].name)
    )
      return;

    const previous = active;
    unsubscribe?.();
    // Register the replacement before closing the old watch, so their common signals retain history.
    const feed = next.length ? client.variables.watch(next, settings) : null;
    selection = next;
    active = feed;
    const refresh = () => {
      if (active !== feed || closed) return;
      historyTarget = client.session.currentTarget();
      const current =
        historyTarget && historyTarget.session === client.variables.state.get().session;
      series.set(
        next.map((ref) => ({ ref: { ...ref }, points: current && feed ? feed.history(ref) : [] })),
      );
    };
    unsubscribe = feed?.values.subscribe(refresh);
    previous?.close();
    refresh();
  };

  return {
    series: {
      subscribe: (listener) => series.subscribe(listener),
      get() {
        const target = client.session.currentTarget();
        // The session is notified before the raw variable messages clear their projection.
        if (
          target?.instance !== historyTarget?.instance ||
          target?.session !== historyTarget?.session
        )
          return series.get().map(({ ref }) => ({ ref, points: [] }));
        return series.get();
      },
    },
    setSignals,
    close() {
      if (closed) return;
      closed = true;
      unsubscribe?.();
      active?.close();
      active = null;
      series.set([]);
    },
  };
}
