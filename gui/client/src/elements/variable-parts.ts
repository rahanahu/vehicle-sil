// The parts of one variable: show it, set it with a slider, write one value with a button.
import { watch } from '../index';
import type { Front, FrontVariable, VariableView } from '../index';
import { Part, h, quietly, setData } from './part';
import { reasonText } from './reasons';

let noteIds = 0;

/** The line under a variable part: why it cannot be written, or why the last write failed. */
const writeNote = (v: VariableView): string => {
  if (v.problem) return reasonText(v.problem);
  if (!v.can_write && v.why) return reasonText(v.why);
  return v.writing ? 'writing…' : '';
};

/** A part that shows or sets the variable of its `variable` attribute (`"ECU.variable"`). */
abstract class VariablePart extends Part {
  static override observedAttributes = ['variable'];

  /** Whether the part writes (so that it says why it cannot). */
  protected readonly writes: boolean = false;

  /** Builds the controls once; returns what to do with each new view. */
  protected abstract build(
    variable: FrontVariable,
    root: HTMLElement,
    note: HTMLElement,
  ): (view: VariableView) => void;

  protected draw(client: Front, root: HTMLElement): void {
    const name = this.getAttribute('variable');
    if (!name) {
      throw new Error(
        `<${this.localName}> needs the attribute variable="ECU.variable" (the ECU's name, a dot and the variable's name)`,
      );
    }
    const variable = client.variable(name);
    const note = h('small', { id: `vsil-note-${++noteIds}`, class: 'vsil-note' });
    const show = this.build(variable, root, note);
    root.append(' ', note);
    this.own(
      watch(variable, (v) => {
        setData(this, 'stale', String(v.stale));
        setData(this, 'canWrite', String(v.can_write));
        setData(this, 'availability', v.availability);
        if (this.writes) note.textContent = writeNote(v);
        note.hidden = false;
        show(v);
      }),
    );
  }
}

/** `<vsil-value variable="ECU.variable" [on="LOST" off="ok"] [no-unit]>`: the value and its unit; a bool shows as the `on` and `off` text. */
export class VsilValueElement extends VariablePart {
  static override observedAttributes = ['variable', 'on', 'off', 'no-unit'];

  protected build(_variable: FrontVariable, root: HTMLElement, note: HTMLElement) {
    const output = root.appendChild(h('output'));
    return (v: VariableView) => {
      if (typeof v.value === 'boolean') {
        setData(this, 'on', String(v.value));
        output.textContent = v.value ? this.text('on', 'on') : this.text('off', 'off');
      } else {
        setData(this, 'on', null);
        const unit = this.hasAttribute('no-unit') || !v.unit ? '' : ` ${v.unit}`;
        output.textContent = v.value === null ? '-' : `${v.text}${unit}`;
      }
      note.textContent = v.note ?? '';
    };
  }
}

/** `<vsil-slider variable="ECU.variable">`: a number in the variable's range; it is written when the knob is released. */
export class VsilSliderElement extends VariablePart {
  protected override readonly writes = true;

  protected build(variable: FrontVariable, root: HTMLElement, note: HTMLElement) {
    const input = h('input', { type: 'range', 'aria-describedby': note.id });
    // Inside a page's <label> the name is the label's text: the range input already says its value, and the note
    // reaches it as its description (aria-describedby works on a hidden element), so neither goes into the name.
    const output = h('output', { 'aria-hidden': 'true' });
    note.setAttribute('aria-hidden', 'true');
    let dragging = false;
    input.addEventListener('input', () => {
      dragging = true;
      input.removeAttribute('aria-valuetext'); // while the knob moves it says its own position, not the value read
    });
    input.addEventListener('change', () => {
      dragging = false;
      void quietly(() => variable.write(Number(input.value)));
    });
    // A knob let go of where it was picked up gives no `change`: it follows the variable again.
    for (const name of ['pointerup', 'pointercancel', 'keyup', 'blur'])
      input.addEventListener(name, () => (dragging = false));
    root.append(input, ' ', output);
    return (v: VariableView) => {
      const ranged = v.min !== null && v.max !== null;
      input.disabled = !v.can_write || !ranged;
      if (!v.can_write) dragging = false; // a knob that cannot be released must not stay held
      if (ranged) {
        input.min = String(v.min);
        input.max = String(v.max);
        input.step = v.step === null ? 'any' : String(v.step);
      } else if (v.can_write && !v.problem) {
        note.textContent = 'this variable has no range to set';
      }
      // while a write is on its way the knob stays where it was put (the view still has the value that was read)
      if (!dragging && !v.writing && typeof v.value === 'number') input.value = String(v.value);
      output.textContent = v.value === null ? '-' : `${v.text}${v.unit ? ` ${v.unit}` : ''}`;
      // the output is hidden from a screen reader (it would become part of a label's name): the knob says the value itself
      if (v.value === null || dragging || v.writing) input.removeAttribute('aria-valuetext');
      else input.setAttribute('aria-valuetext', output.textContent);
    };
  }
}

/** The value of a `value` attribute: JSON (a number, true, false), else the text as it is. */
const target = (text: string | null): number | boolean | string => {
  if (text === null) return true;
  try {
    const value: unknown = JSON.parse(text);
    if (typeof value === 'number' || typeof value === 'boolean') return value;
  } catch {
    // not JSON: it is a text
  }
  return text;
};

/** `<vsil-write variable="ECU.variable" value="false" label="Cut">`: writes one value when pressed (not when it already is that value). */
export class VsilWriteElement extends VariablePart {
  static override observedAttributes = ['variable', 'value', 'label'];
  protected override readonly writes = true;

  protected build(variable: FrontVariable, root: HTMLElement, note: HTMLElement) {
    const value = target(this.getAttribute('value'));
    const button = h(
      'button',
      { type: 'button', 'aria-describedby': note.id },
      this.text('label', `${this.getAttribute('variable')} = ${String(value)}`),
    );
    button.addEventListener('click', () => {
      if (button.getAttribute('aria-disabled') !== 'true')
        void quietly(() => variable.write(value));
    });
    root.append(button);
    return (v: VariableView) => {
      const already = v.can_write && v.value === value;
      button.setAttribute('aria-disabled', String(!v.can_write || already));
      if (already && !v.problem) {
        // said to a screen reader (aria-describedby), not shown: the page shows the value itself
        note.textContent = `already ${String(value)}`;
        note.hidden = true;
      }
    };
  }
}
