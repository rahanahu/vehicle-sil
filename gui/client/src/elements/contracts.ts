// Single source of truth for the attributes, JS properties and custom events of all `vsil-*` custom elements.
// Framework adapters (`@vehicle-sil/gui-client/react`, `/vue`, `/svelte`) derive their types from this map and
// `VehicleSilTimelineContract` without duplicating element tables or pulling framework packages into core types.
import type { ActionError, Front, Intent, WriteResult } from '../index';
import type { VsilConnectElement, VsilOutsideElement } from './outside-parts';
import type {
  VsilRunControlsElement,
  VsilStatusElement,
  VsilTimeControlsElement,
} from './run-parts';
import type {
  VsilCaseSelectElement,
  VsilScenarioResultElement,
  VsilScenarioSelectElement,
  VsilTimelineElement,
} from './scenario-parts';
import type { VsilSliderElement, VsilValueElement, VsilWriteElement } from './variable-parts';

/** Declarative contract of one custom element: its DOM class, HTML attributes, JS properties and custom events. */
export interface ElementContract<
  El extends HTMLElement = HTMLElement,
  Attrs extends object = Record<never, never>,
  Props extends object = Record<never, never>,
  Events extends object = Record<never, never>,
> {
  readonly element: El;
  readonly attrs: Attrs;
  readonly props: Props;
  readonly events: Events;
}

/** HTML boolean attribute value accepted in markup (`<el attr>`, `attr=""`, `attr={true}`). Use the camelCase JS property for dynamic boolean `false` bindings. */
export type BooleanAttr = true | '';

/** Properties common to every `Part` custom element. */
export interface VsilPartProps {
  client?: Front | null;
}

/** Detail of the `vsil-write-queued` event: the write reached the ECU queue (`seq` assigned). */
export interface WriteQueuedInfo {
  readonly session: string;
  readonly seq: number;
  readonly peer: string;
  readonly name: string;
  readonly value: number | boolean | string;
}

/** Detail of the `vsil-write-result` event when the ECU applied the write. */
export interface WriteSuccess extends WriteResult {
  readonly ok: true;
}

/** Detail of the `vsil-write-result` event when the write did not apply. */
export interface WriteFailure {
  readonly ok: false;
  readonly peer: string;
  readonly name: string;
  readonly value: number | boolean | string;
  readonly error: ActionError;
  readonly seq?: number;
}

/** Detail of the `vsil-write-result` event: either applied (`ok: true`) or failed (`ok: false`). */
export type WriteOutcome = WriteSuccess | WriteFailure;

/** Custom DOM events dispatched by `<vsil-slider>` and `<vsil-write>`. */
export interface VsilWriteEventMap {
  'vsil-write-queued': CustomEvent<WriteQueuedInfo>;
  'vsil-write-result': CustomEvent<WriteOutcome>;
}

/** Contracts of all 12 `vsil-*` custom elements defined by `@vehicle-sil/gui-client/elements`. */
export interface VsilElementContracts {
  'vsil-status': ElementContract<VsilStatusElement, Record<never, never>, VsilPartProps>;
  'vsil-run-controls': ElementContract<
    VsilRunControlsElement,
    {
      'label-start'?: string;
      'label-stop'?: string;
      'label-force-stop'?: string;
    },
    VsilPartProps & {
      intent?: Intent | null;
      labelStart?: string | null;
      labelStop?: string | null;
      labelForceStop?: string | null;
    }
  >;
  'vsil-time-controls': ElementContract<
    VsilTimeControlsElement,
    {
      speeds?: string;
      'step-ms'?: string | number;
      'label-pause'?: string;
      'label-resume'?: string;
      'label-step'?: string;
      'label-run-until'?: string;
    },
    VsilPartProps & {
      speeds?: string | null;
      stepMs?: number | string | null;
      labelPause?: string | null;
      labelResume?: string | null;
      labelStep?: string | null;
      labelRunUntil?: string | null;
    }
  >;
  'vsil-value': ElementContract<
    VsilValueElement,
    {
      variable?: string;
      on?: string;
      off?: string;
      'no-unit'?: BooleanAttr;
    },
    VsilPartProps & {
      variable?: string | null;
      on?: string | null;
      off?: string | null;
      noUnit?: boolean;
    }
  >;
  'vsil-slider': ElementContract<
    VsilSliderElement,
    {
      variable?: string;
    },
    VsilPartProps & {
      variable?: string | null;
    },
    VsilWriteEventMap
  >;
  'vsil-write': ElementContract<
    VsilWriteElement,
    {
      variable?: string;
      value?: string;
      label?: string;
    },
    VsilPartProps & {
      variable?: string | null;
      value?: string | number | boolean | null;
      label?: string | null;
    },
    VsilWriteEventMap
  >;
  'vsil-scenario-select': ElementContract<
    VsilScenarioSelectElement,
    Record<never, never>,
    VsilPartProps
  >;
  'vsil-case-select': ElementContract<VsilCaseSelectElement, Record<never, never>, VsilPartProps>;
  'vsil-scenario-result': ElementContract<
    VsilScenarioResultElement,
    Record<never, never>,
    VsilPartProps
  >;
  'vsil-timeline': ElementContract<VsilTimelineElement, Record<never, never>, VsilPartProps>;
  'vsil-connect': ElementContract<
    VsilConnectElement,
    {
      network?: string;
      file?: string;
    },
    VsilPartProps & {
      network?: string | null;
      file?: string | null;
    }
  >;
  'vsil-outside': ElementContract<VsilOutsideElement, Record<never, never>, VsilPartProps>;
}
