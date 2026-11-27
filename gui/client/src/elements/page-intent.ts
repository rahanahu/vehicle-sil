// What the choosing parts of the page selected for the next run. The start button reads it. Not in the client: a
// draft is not the truth, and each tab may have its own.
import type { Intent } from '../index';

type Listener = (intent: Intent) => void;

let intent: Intent = {};
const listeners = new Set<Listener>();

/** The page's selection for the next run (`<vsil-scenario-select>` and `<vsil-case-select>` write it, `<vsil-run-controls>` starts with it). */
export const pageIntent = {
  get: (): Intent => intent,
  subscribe(listener: Listener): () => void {
    listeners.add(listener);
    return () => listeners.delete(listener);
  },
  /** Merges `patch`; a field that is undefined or an empty string is removed. */
  update(patch: Intent | { [K in keyof Intent]?: Intent[K] | '' }): void {
    const next: Record<string, unknown> = { ...intent, ...patch };
    for (const key of Object.keys(next))
      if (next[key] === undefined || next[key] === '') delete next[key];
    intent = next as Intent;
    for (const listener of [...listeners]) listener(intent);
  },
};
