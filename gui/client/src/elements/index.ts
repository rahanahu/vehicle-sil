// The parts (custom elements) of a page that uses the front door: `import '@vehicle-sil/gui-client/elements'` defines
// them. They draw what `connect()` returns, use the page's connection (the same one as the page's own script), and have
// no styles of their own. This entry needs a browser (DOM); the main entry does not import it.
import { pageIntent } from './page-intent';
import { setReasonText } from './reasons';
import type { ReasonText } from './reasons';
import { VsilRunControlsElement, VsilStatusElement, VsilTimeControlsElement } from './run-parts';
import {
  VsilScenarioResultElement,
  VsilScenarioSelectElement,
  VsilTimelineElement,
} from './scenario-parts';
import { VsilSliderElement, VsilValueElement, VsilWriteElement } from './variable-parts';

export { pageIntent, setReasonText };
export type { ReasonText };
export {
  VsilRunControlsElement,
  VsilScenarioResultElement,
  VsilScenarioSelectElement,
  VsilSliderElement,
  VsilStatusElement,
  VsilTimeControlsElement,
  VsilTimelineElement,
  VsilValueElement,
  VsilWriteElement,
};

declare global {
  interface HTMLElementTagNameMap {
    'vsil-status': VsilStatusElement;
    'vsil-run-controls': VsilRunControlsElement;
    'vsil-time-controls': VsilTimeControlsElement;
    'vsil-value': VsilValueElement;
    'vsil-slider': VsilSliderElement;
    'vsil-write': VsilWriteElement;
    'vsil-scenario-select': VsilScenarioSelectElement;
    'vsil-scenario-result': VsilScenarioResultElement;
    'vsil-timeline': VsilTimelineElement;
  }
}

const PARTS: [string, CustomElementConstructor][] = [
  ['vsil-status', VsilStatusElement],
  ['vsil-run-controls', VsilRunControlsElement],
  ['vsil-time-controls', VsilTimeControlsElement],
  ['vsil-value', VsilValueElement],
  ['vsil-slider', VsilSliderElement],
  ['vsil-write', VsilWriteElement],
  ['vsil-scenario-select', VsilScenarioSelectElement],
  ['vsil-scenario-result', VsilScenarioResultElement],
  ['vsil-timeline', VsilTimelineElement],
];

// A name that is defined already (another copy of this module) is left alone.
for (const [name, part] of PARTS) if (!customElements.get(name)) customElements.define(name, part);
