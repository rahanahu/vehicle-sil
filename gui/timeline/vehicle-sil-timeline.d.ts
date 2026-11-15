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

export interface VehicleSilTimelineLoadDetail {
  /** The file's name (file-name, or the name the server gives), or null. */
  file: string | null;
  /** The scenario's name, or null. */
  scenario: string | null;
  /** The scenario's result as the file writes it: 'passed', 'failed', 'incomplete', 'not_run' or another. */
  status: string;
  /** The whole run that can be shown, in virtual ms. */
  start: number;
  end: number;
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

declare global {
  interface HTMLElementTagNameMap {
    'vehicle-sil-timeline': VehicleSilTimelineElement;
  }
}
