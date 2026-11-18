// The one place that decides which sentence a part shows for a reason (a button that cannot be used, a problem, a
// variable that cannot be written).
import type { Reason } from '../index';

/** Returns the sentence to show for `reason`; null or undefined keeps the English sentence of the owner of the reason. */
export type ReasonText = (reason: Reason) => string | null | undefined;

let custom: ReasonText | null = null;

/**
 * Replaces the sentences that the parts show for reasons (`controls.*.reason`, `caution`, `status.problem`, and a
 * variable's `why` and `problem`), for example to translate them: `fn` gets the reason (its `code` stays the same) and
 * returns the sentence, or null to keep the English one. `null` removes it. Call it before the parts are in the page:
 * a part draws the sentence again when its value changes.
 */
export function setReasonText(fn: ReasonText | null): void {
  custom = fn;
}

/** The sentence for `reason`. */
export function reasonText(reason: Reason): string {
  return custom?.(reason) ?? reason.message;
}
