// Types of vehicle-sil-timeline.js: the custom element <vehicle-sil-timeline>, which draws a timeline file of
// vehicle-sil. Importing the file defines the element (it exports nothing):
//
//   import '@vehicle-sil/gui-client/timeline';            // the npm package of the client library
//   const timeline = document.createElement('vehicle-sil-timeline');
//
// docs/gui.md ("Putting a timeline on your own screen") says what each attribute, property and event does.

/** Words of the element that a page adds or changes: { <language>: { <key of messages.js>: text } }. */
export type VehicleSilTimelineMessages = Record<
  string,
  Record<string, string | ((args: Record<string, unknown>) => string)>
>;

/** The parts of the element that `hide` can leave out. */
export type VehicleSilTimelinePart =
  'header' | 'summary' | 'toolbar' | 'help' | 'steps' | 'readout' | 'legend' | 'tables';

/** One comparison in a step's watch condition (constant value or right-hand variable). */
export interface VehicleSilTimelineComparison {
  op: string;
  /** Constant value compared against; null when `variable` names a right-hand variable. */
  value: boolean | number | null;
  /** Right-hand variable (`<participant>.<name>`) when comparing two variables; omitted for constants. */
  variable?: string;
  /** Tolerance for `near`; null for other operators. */
  tolerance: number | null;
}

/** One variable condition watched by a step. */
export interface VehicleSilTimelineWatchCondition {
  variable: string;
  compare: VehicleSilTimelineComparison[];
}

/** A right-hand variable's value and sample time recorded on a failed condition. */
export interface VehicleSilTimelineFailedCompared {
  variable: string;
  value: boolean | number | null;
  sample_ms: number | null;
}

/** One unmet condition recorded in a failed step. */
export interface VehicleSilTimelineFailedCondition {
  condition: number;
  value: boolean | number | null;
  sample_ms: number | null;
  /** Right-hand variables of failed comparisons on this condition; omitted when none failed. */
  compared?: VehicleSilTimelineFailedCompared[];
}

/** Metadata about one variable in a loaded timeline, emitted in `load` event detail. */
export interface VehicleSilTimelineLoadVariable {
  name: string;
  participant: string;
  type: string;
  causality: string | null;
  unit: string;
  written: boolean;
  watched: boolean;
  recorded: boolean;
  failing: boolean;
  relatedToFailure: boolean;
  changedNearFailure: boolean;
}

/** One unmet condition summary in `load` event detail. */
export interface VehicleSilTimelineLoadFailureCondition {
  variable: string | null;
  compare: string | null;
  valueText: string | null;
  sampleMs: number | null;
  compared: Array<{
    variable: string | null;
    valueText: string | null;
    sampleMs: number | null;
  }>;
}

/** One write summary in `load` event detail for a failed step. */
export interface VehicleSilTimelineLoadFailureWrite {
  variable: string;
  at: number | null;
  valueText: string;
  applied: boolean | null;
  cause: boolean;
}

/** Failure summary emitted in `load` event detail when the scenario failed. */
export interface VehicleSilTimelineLoadFailure {
  step: number;
  at: number | null;
  name: string | null;
  text: string;
  message: string | null;
  conditions: VehicleSilTimelineLoadFailureCondition[];
  writes: VehicleSilTimelineLoadFailureWrite[];
}

/** Options for zoom-preserving navigation methods on `<vehicle-sil-timeline>`. */
export interface VehicleSilTimelineFocusOptions {
  /** Keep the current zoom width when already zoomed in (defaults to true). */
  preserveZoom?: boolean;
  /**
   * Explicit pre-failure lead window in virtual ms (for `jumpToFailure()`).
   * Frames `[failure.at - leadMs, failure.at + (trailMs ?? leadMs * 0.25)]` clamped to the run domain without writing sticky `from`/`to`.
   */
  leadMs?: number;
  /**
   * Explicit post-failure trail window in virtual ms (for `jumpToFailure()`).
   * When finite and >= 0, sets the right edge to `min(domain[1], failure.at + trailMs)`.
   */
  trailMs?: number;
}

export interface VehicleSilTimelineLoadDetail {
  /** The file's name (file-name, or the name the server gives), or null. */
  file: string | null;
  /** True when the timeline was recorded from a manual run without a scenario (`scenario === null`). */
  manual: boolean;
  /** The scenario's name, or null. */
  scenario: string | null;
  /** The scenario's result as the file writes it: 'passed', 'failed', 'incomplete', 'not_run', 'manual' (when `manual` is true), or another. */
  status: string;
  /** The whole run that can be shown, in virtual ms. */
  start: number;
  end: number;
  /** The last virtual timestamp recorded in the timeline, or null. */
  recordedTo: number | null;
  /** Number of scenario steps in the loaded timeline. */
  stepCount: number;
  /** Failure summary when the scenario failed, else null. */
  failure: VehicleSilTimelineLoadFailure | null;
  /** Variables present in the timeline with failure-relevance and activity flags. */
  variables: VehicleSilTimelineLoadVariable[];
}

export interface VehicleSilTimelineErrorDetail {
  /** Why the file cannot be shown, and what to do (in the element's language). */
  message: string;
  /** The HTTP status when src was answered with an error, else null. */
  status: number | null;
}

export interface VehicleSilTimelineEventMap {
  load: CustomEvent<VehicleSilTimelineLoadDetail>;
  error: CustomEvent<VehicleSilTimelineErrorDetail>;
  cursorchange: CustomEvent<{ cursor: number | null }>;
  stepselect: CustomEvent<{ step: number | null }>;
  viewchange: CustomEvent<{ from: number; to: number }>;
  variablehide: CustomEvent<{ variable: string }>;
}

export interface VehicleSilTimelineElement extends HTMLElement {
  /** The address of a timeline file on the page's own server (attribute src). */
  src: string;
  /** The name shown for the file (attribute file-name). */
  fileName: string;
  /** The file itself: its text or its parsed JSON. Shown instead of src while it is not null. */
  data: string | object | null;
  /** The token sent with src (Authorization: Bearer). Default: the one the GUI's screens keep in the tab. */
  token: string | null;
  /** More headers sent with src. */
  headers: Record<string, string> | null;
  /** The cursor, in virtual ms (attribute cursor), or null. */
  cursor: number | null;
  /** The chosen step, by its index (attribute step), or null. */
  step: number | null;
  /** The part of the run shown, in virtual ms (attributes from and to). */
  from: number | null;
  to: number | null;
  /** The variables to draw, in this order: names, or prefixes ending in '*' (attribute variables). */
  variables: string[] | null;
  /** A label for a variable, by its name, shown in its lane. */
  labels: Record<string, string> | null;
  /** The parts to leave out: words of VehicleSilTimelinePart separated by spaces (attribute hide). */
  hide: string;
  /** Smaller lanes for a small box (attribute compact). */
  compact: boolean;
  /**
   * 'light', 'dark', or 'auto': the color-scheme of the page around the element (light where the page declares
   * none; attribute theme).
   */
  theme: 'light' | 'dark' | 'auto';
  /** Words that the page adds or changes; a language here can be named in lang. */
  messages: VehicleSilTimelineMessages | null;
  /** Loads src again. */
  reload(): void;
  /** Jumps to the failure timestamp, pinning the cursor and selecting the failed step (preserves zoom by default). */
  jumpToFailure(options?: VehicleSilTimelineFocusOptions): boolean;
  /** Focuses a step by its 1-based index, pinning the cursor to its timestamp (preserves zoom when requested). */
  focusStep(index: number, options?: VehicleSilTimelineFocusOptions): boolean;
  /** Resets the horizontal view to the whole run without modifying sticky `from`/`to` options. */
  resetView(): void;
  /** Pans the current horizontal view by `fraction` of the current span (e.g. -0.25 left, +0.25 right), clamped to the run domain. */
  pan(fraction: number): void;
  /** Widens the current horizontal view around its midpoint, clamped to the run domain. */
  zoomOut(): void;
  /** Clears a pinned cursor and any Δt reference marker without changing zoom or step selection. */
  unpinCursor(): void;

  addEventListener<K extends keyof VehicleSilTimelineEventMap>(
    type: K,
    listener: (this: VehicleSilTimelineElement, ev: VehicleSilTimelineEventMap[K]) => unknown,
    options?: boolean | AddEventListenerOptions,
  ): void;
  addEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject,
    options?: boolean | AddEventListenerOptions,
  ): void;
  removeEventListener<K extends keyof VehicleSilTimelineEventMap>(
    type: K,
    listener: (this: VehicleSilTimelineElement, ev: VehicleSilTimelineEventMap[K]) => unknown,
    options?: boolean | EventListenerOptions,
  ): void;
  removeEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject,
    options?: boolean | EventListenerOptions,
  ): void;
}

/** HTML attributes accepted by `<vehicle-sil-timeline>`. */
export interface VehicleSilTimelineAttributes {
  src?: string;
  'file-name'?: string;
  cursor?: string | number;
  step?: string | number;
  from?: string | number;
  to?: string | number;
  variables?: string;
  hide?: string;
  compact?: boolean | '';
  theme?: 'light' | 'dark' | 'auto';
}

/** JS properties accepted by `<vehicle-sil-timeline>`. */
export interface VehicleSilTimelineProperties {
  src?: string;
  fileName?: string;
  data?: string | object | null;
  token?: string | null;
  headers?: Record<string, string> | null;
  cursor?: number | null;
  step?: number | null;
  from?: number | null;
  to?: number | null;
  variables?: string[] | null;
  labels?: Record<string, string> | null;
  hide?: string;
  compact?: boolean;
  theme?: 'light' | 'dark' | 'auto';
  messages?: VehicleSilTimelineMessages | null;
}

/** Single-source contract entry of `<vehicle-sil-timeline>` for framework type adapters. */
export interface VehicleSilTimelineContract {
  readonly 'vehicle-sil-timeline': {
    readonly element: VehicleSilTimelineElement;
    readonly attrs: VehicleSilTimelineAttributes;
    readonly props: VehicleSilTimelineProperties;
    readonly events: VehicleSilTimelineEventMap;
  };
}

declare global {
  interface HTMLElementTagNameMap {
    'vehicle-sil-timeline': VehicleSilTimelineElement;
  }
}
