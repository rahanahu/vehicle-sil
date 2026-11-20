import {
  ChangeDetectionStrategy,
  Component,
  ElementRef,
  computed,
  effect,
  inject,
  input,
  output,
  signal,
  untracked,
} from '@angular/core';
import { watch, type Front, type VariableInfo, type VariableView } from '@vehicle-sil/gui-client';

const onVisibility = new WeakMap<Element, (visible: boolean) => void>();
const OUTPUT_REFRESH_MS = 150;
const SWITCH_READBACK_MS = 500;
const viewport = new IntersectionObserver(
  (entries) => {
    for (const entry of entries) onVisibility.get(entry.target)?.(entry.isIntersecting);
  },
  { rootMargin: '150px' },
);

@Component({
  selector: 'app-io-row',
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <div class="signal" [attr.data-inactive]="view()?.stale || view()?.availability === 'inactive'">
      <span
        class="signal-name"
        tabindex="0"
        [title]="info().name + (info().description ? ' — ' + info().description : '')"
        >{{ info().name }}</span
      >
      <div class="value">
        @if (info().writable) {
          @switch (info().type) {
            @case ('bool') {
              <input
                class="switch"
                type="checkbox"
                switch
                role="switch"
                [attr.aria-label]="info().name"
                [checked]="view()?.value === true"
                [disabled]="!view()?.can_write"
                [attr.aria-disabled]="switchWaiting() || null"
                [attr.aria-busy]="switchWaiting() || null"
                (click)="toggle($event)"
              />
              <span class="switch-state">{{ view()?.value == null ? '—' : lampText() }}</span>
            }
            @case ('enum') {
              <select
                [attr.aria-label]="info().name"
                [disabled]="blocked()"
                (change)="send(+$any($event.target).value)"
              >
                <option disabled [selected]="shown() == null">—</option>
                @for (item of info().enum; track item.value) {
                  <option [value]="item.value" [selected]="shown() === item.value">
                    {{ item.name }}
                  </option>
                }
              </select>
            }
            @default {
              @if (info().min != null && info().max != null && info().min! < info().max!) {
                <input
                  type="range"
                  [attr.aria-label]="'Adjust ' + info().name"
                  [min]="info().min!"
                  [max]="info().max!"
                  [step]="info().step ?? (info().type === 'integer' ? 1 : 'any')"
                  [value]="shown() ?? info().min!"
                  [disabled]="blocked()"
                  (input)="draft.set($any($event.target).value)"
                  (change)="send($any($event.target).valueAsNumber)"
                />
              }
              <input
                type="number"
                [attr.aria-label]="info().name"
                [disabled]="blocked()"
                [min]="info().min"
                [max]="info().max"
                [step]="info().type === 'integer' ? 1 : 'any'"
                [value]="draft() ?? shown() ?? ''"
                (input)="draft.set($any($event.target).value)"
                (blur)="draft.set(null)"
                (keydown.enter)="commit($any($event.target))"
                (keydown.escape)="draft.set(null)"
                [title]="view()?.why?.message ?? ''"
              />
              <span>{{ info().unit }}</span>
            }
          }
        } @else if (info().type === 'bool') {
          <span
            class="lamp"
            [attr.data-on]="view()?.value === true"
            [attr.data-missing]="view()?.value == null"
            role="img"
            [attr.aria-label]="lampText()"
            [title]="lampText()"
          ></span>
        } @else {
          <span>{{ view()?.text || '—' }} {{ info().unit }}</span>
        }
      </div>
      @if (error()) {
        <span
          class="write-error"
          [class.switch-error]="info().type === 'bool'"
          role="status"
          [title]="error()"
          >Write failed</span
        >
      }
      <span class="signal-actions">
        <button
          type="button"
          [attr.aria-label]="(pinned() ? 'Unpin ' : 'Pin ') + info().name"
          (click)="pin.emit()"
        >
          {{ pinned() ? 'Unpin' : 'Pin' }}
        </button>
        <button
          type="button"
          [attr.aria-label]="'Add ' + info().name + ' to waveforms'"
          (click)="trace.emit()"
        >
          + Wave
        </button>
      </span>
    </div>
  `,
})
export class IoRow {
  readonly front = input.required<Front>();
  readonly peer = input.required<string>();
  readonly info = input.required<VariableInfo>();
  readonly pinned = input.required<boolean>();
  readonly running = input.required<boolean>();
  readonly pin = output();
  readonly trace = output();
  readonly view = signal<VariableView | null>(null);
  readonly draft = signal<string | null>(null);
  readonly pending = signal<number | boolean | null>(null);
  private readonly awaitingSample = signal<boolean | null>(null);
  readonly error = signal('');
  readonly binding = computed(() =>
    this.front().variable({ peer: this.peer(), name: this.info().name }),
  );
  readonly shown = computed(() => this.pending() ?? this.view()?.value);
  readonly switchWaiting = computed(
    () => this.pending() !== null || this.awaitingSample() !== null,
  );
  readonly blocked = computed(() => !this.view()?.can_write || this.switchWaiting());
  readonly lampText = computed(() =>
    this.view()?.value == null ? 'No sample' : this.view()?.value ? 'ON' : 'OFF',
  );

  private readonly host = inject<ElementRef<HTMLElement>>(ElementRef).nativeElement;
  private readonly visible = signal(false);

  constructor() {
    effect((cleanup) => {
      onVisibility.set(this.host, (visible) => this.visible.set(visible));
      viewport.observe(this.host);
      cleanup(() => {
        viewport.unobserve(this.host);
        onVisibility.delete(this.host);
      });
    });
    effect((cleanup) => {
      if (!this.visible()) return;
      const throttle = !this.info().writable && this.running();
      let timer: ReturnType<typeof setTimeout> | undefined;
      let latest: VariableView | undefined;
      const stop = watch(this.binding(), (value) => {
        const expected = untracked(this.awaitingSample);
        if (expected !== null && (value.value === expected || !value.can_write)) {
          this.awaitingSample.set(null);
        }
        const current = untracked(this.view);
        if (
          !throttle ||
          !current ||
          value.condition.kind !== current.condition.kind ||
          value.availability !== current.availability
        ) {
          clearTimeout(timer);
          timer = undefined;
          this.view.set(value);
          return;
        }
        latest = value;
        timer ??= setTimeout(() => {
          timer = undefined;
          if (latest) this.view.set(latest);
        }, OUTPUT_REFRESH_MS);
      });
      cleanup(() => {
        clearTimeout(timer);
        stop();
      });
    });
    effect((cleanup) => {
      if (this.awaitingSample() === null) return;
      const timer = setTimeout(() => this.awaitingSample.set(null), SWITCH_READBACK_MS);
      cleanup(() => clearTimeout(timer));
    });
  }

  toggle(event: MouseEvent) {
    event.preventDefault();
    if (!this.blocked()) void this.send(this.view()?.value !== true);
  }

  commit(element: HTMLInputElement) {
    if (element.reportValidity()) void this.send(element.valueAsNumber);
  }

  async send(value: number | boolean) {
    if (this.blocked() || (typeof value === 'number' && !Number.isFinite(value))) return;
    this.error.set('');
    this.draft.set(null);
    this.pending.set(value);
    try {
      await this.binding().write(value);
      const current = this.view();
      if (typeof value === 'boolean' && current?.can_write && current.value !== value) {
        this.awaitingSample.set(value);
      }
    } catch (error) {
      this.error.set(error instanceof Error ? error.message : String(error));
    } finally {
      this.pending.set(null);
    }
  }
}
