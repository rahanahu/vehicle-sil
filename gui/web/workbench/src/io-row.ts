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
} from '@angular/core';
import {
  watch,
  createVariableEditor,
  type Front,
  type VariableEditor,
  type VariableEditorState,
  type VariableInfo,
} from '@vehicle-sil/gui-client';

const onVisibility = new WeakMap<Element, (visible: boolean) => void>();
const OUTPUT_REFRESH_MS = 150;
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
                [title]="view()?.why?.message ?? ''"
                (click)="toggle($event)"
              />
              <span class="switch-state">{{ view()?.value == null ? '—' : lampText() }}</span>
            }
            @case ('enum') {
              <select
                [attr.aria-label]="info().name"
                [disabled]="blocked()"
                [title]="view()?.why?.message ?? ''"
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
                  [title]="view()?.why?.message ?? ''"
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
      @if (view()?.writing) {
        <span class="write-status" role="status">Write pending</span>
      } @else if (error()) {
        <span
          class="write-error"
          [class.switch-error]="info().type === 'bool'"
          role="status"
          [title]="error()"
          >{{ uncertain() ? 'Write outcome unknown' : 'Write failed' }}</span
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
  private readonly editor = signal<VariableEditor | null>(null);
  private readonly editing = signal<VariableEditorState | null>(null);
  readonly view = computed(() => this.editing()?.view ?? null);
  readonly draft = signal<string | null>(null);
  readonly error = computed(() => this.view()?.problem?.message ?? '');
  readonly uncertain = computed(() =>
    ['no-answer', 'closed'].includes(this.view()?.write_status.kind ?? ''),
  );
  readonly shown = computed(() => this.editing()?.shown ?? null);
  readonly switchWaiting = computed(() => this.editing()?.waiting ?? false);
  readonly blocked = computed(() => !this.editing()?.canWrite);
  readonly lampText = computed(() =>
    this.view()?.value == null ? 'No sample' : this.view()?.value ? 'ON' : 'OFF',
  );

  private readonly host = inject<ElementRef<HTMLElement>>(ElementRef).nativeElement;
  private readonly visible = signal(false);

  constructor() {
    effect((cleanup) => {
      const editor = createVariableEditor(this.front(), {
        peer: this.peer(),
        name: this.info().name,
      });
      this.editor.set(editor);
      cleanup(() => {
        editor.close();
        this.editor.set(null);
        this.editing.set(null);
        this.draft.set(null);
      });
    });
    effect((cleanup) => {
      onVisibility.set(this.host, (visible) => this.visible.set(visible));
      viewport.observe(this.host);
      cleanup(() => {
        viewport.unobserve(this.host);
        onVisibility.delete(this.host);
      });
    });
    effect(() => this.editor()?.setVisible(this.visible()));
    effect((cleanup) => {
      const editor = this.editor();
      if (!editor || !this.visible()) return;
      const throttle = !this.info().writable && this.running();
      let timer: ReturnType<typeof setTimeout> | undefined;
      let latest: VariableEditorState | undefined;
      let previous: VariableEditorState | undefined;
      const stop = watch(editor.state, (value) => {
        if (
          !throttle ||
          !previous ||
          value.view.condition.kind !== previous.view.condition.kind ||
          value.view.availability !== previous.view.availability
        ) {
          clearTimeout(timer);
          timer = undefined;
          previous = value;
          this.editing.set(value);
          return;
        }
        latest = value;
        timer ??= setTimeout(() => {
          timer = undefined;
          if (!latest) return;
          previous = latest;
          this.editing.set(latest);
        }, OUTPUT_REFRESH_MS);
      });
      cleanup(() => {
        clearTimeout(timer);
        stop();
      });
    });
  }

  toggle(event: MouseEvent) {
    event.preventDefault();
    if (!this.blocked()) void this.send(this.view()?.value !== true);
  }

  commit(element: HTMLInputElement) {
    if (element.reportValidity()) void this.send(element.valueAsNumber);
  }

  send(value: number | boolean) {
    if (this.editor()?.write(value)) this.draft.set(null);
  }
}
