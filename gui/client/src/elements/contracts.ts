// Single source of truth for the attributes, JS properties and custom events of all `vsil-*` custom elements.
// Framework adapters (`@vehicle-sil/gui-client/react`, `/vue`, `/svelte`) derive their types from this map and
// `VehicleSilTimelineContract` without duplicating element tables or pulling framework packages into core types.
import type { Front, Intent } from '../index';
import type { VsilConnectElement, VsilOutsideElement } from './outside-parts';
import type {
  VsilRunControlsElement,
  VsilStatusElement,
  VsilTimeControlsElement,
} from './run-parts';
import type {
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

/** Contracts of all 11 `vsil-*` custom elements defined by `@vehicle-sil/gui-client/elements`. */
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
    }
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
    }
  >;
  'vsil-scenario-select': ElementContract<
    VsilScenarioSelectElement,
    Record<never, never>,
    VsilPartProps
  >;
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
