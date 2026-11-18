import { Component, computed, input, linkedSignal, model, signal } from '@angular/core';
import { MatButtonToggleModule } from '@angular/material/button-toggle';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { Speed } from '@vehicle-sil/gui-client';
import { numberText } from '../core/format/number-text';

/** file: 構成ファイルのとおり（ランチャーの既定）、max: 最速、real: 等速、factor: 倍率。 */
export type SpeedChoice = 'file' | 'max' | 'real' | 'factor';

function choiceOf(speed: Speed): SpeedChoice {
  return speed === null ? 'file' : speed === 'max' ? 'max' : speed === 1 ? 'real' : 'factor';
}

/** 1 つの画面に複数置けるように、要素の id を部品ごとに変える。 */
let pickers = 0;

/** 選んだ形のままで、その速度を表せるか（倍率の欄に 1 と打っても、等速に切り替えない）。 */
function fits(choice: SpeedChoice, speed: Speed): boolean {
  return choice === 'factor' ? typeof speed === 'number' : choiceOf(speed) === choice;
}

/**
 * 速度: 構成のとおり / 最速 / 等速 / 倍率。倍率は打った文字のまま持ち、範囲の中の数のときだけ speed に返す。
 * 範囲の外か数でないときは invalid を true にし（開始の理由に出す）、欄を離れたか Enter の後に、欄の下に理由を出す。
 */
@Component({
  selector: 'app-speed-picker',
  imports: [MatButtonToggleModule, MatFormFieldModule, MatInputModule],
  template: `
    <span class="label" [id]="id + '-label'">{{ label() }}</span>
    <mat-button-toggle-group
      [attr.aria-labelledby]="id + '-label'"
      [attr.aria-describedby]="limitsText() ? id + '-limits' : null"
      [value]="choice()"
      [disabled]="locked()"
      (change)="choose($event.value)"
    >
      @if (withFile()) {
        <mat-button-toggle value="file" i18n>構成のとおり</mat-button-toggle>
      }
      @for (option of options(); track option.value) {
        <mat-button-toggle
          [value]="option.value"
          [disabled]="!option.offered"
          disabledInteractive
          >{{ option.label }}</mat-button-toggle
        >
      }
    </mat-button-toggle-group>
    @if (limitsText(); as text) {
      <p class="hint" [id]="id + '-limits'" data-testid="speed-limits">{{ text }}</p>
    }
    @if (choice() === 'factor') {
      <mat-form-field class="factor" subscriptSizing="dynamic">
        <mat-label i18n>倍率</mat-label>
        <input
          matInput
          type="text"
          inputmode="decimal"
          autocomplete="off"
          data-testid="speed-factor"
          [value]="factorText()"
          [disabled]="locked()"
          [attr.aria-invalid]="shownError() !== null"
          [attr.aria-describedby]="shownError() !== null ? id + '-error' : null"
          #factorInput
          (input)="setFactor(factorInput.value)"
          (blur)="checked.set(true)"
          (keydown.enter)="enter($event)"
        />
      </mat-form-field>
    }
    <!-- 誤りは、欄を離れたときか Enter で出す（打っている間は消すだけ） -->
    <p
      class="error"
      [id]="id + '-error'"
      role="status"
      data-testid="speed-error"
      [class.shown]="shownError() !== null"
    >
      {{ shownError() }}
    </p>
  `,
  styles: `
    :host {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: var(--app-space-2);
      min-inline-size: 0;
    }
    .label {
      color: var(--mat-sys-on-surface-variant);
    }
    /* 狭い幅では、選択肢を折り返す（カードからはみ出さない） */
    mat-button-toggle-group {
      flex-wrap: wrap;
      max-inline-size: 100%;
    }
    .factor {
      inline-size: 8em;
    }
    .hint {
      flex-basis: 100%;
      margin: 0;
      color: var(--mat-sys-on-surface-variant);
    }
    .error {
      flex-basis: 100%;
      margin: 0;
      color: var(--mat-sys-error);
    }
    /* 誤りがない間も読み上げの領域として残し、見た目からは消す */
    .error:not(.shown) {
      position: absolute;
      inline-size: 1px;
      block-size: 1px;
      overflow: hidden;
      clip-path: inset(50%);
    }
  `,
})
export class SpeedPicker {
  protected readonly id = `speed-${++pickers}`;
  /** 倍率の範囲（サーバーが api/app で言う）。 */
  readonly min = input(0.000001);
  readonly max = input(1000000);
  /** 最速を選べるか（実行中は正面の口の `controls.speed.fastest`）。 */
  readonly fastest = input(true);
  /** 「構成のとおり」を選べるか（始める前だけ。実行中の速さの変更では出さない）。 */
  readonly withFile = input(true);
  /** 欄の名前。 */
  readonly label = input($localize`速度`);
  /** 変えられないとき（実行中）。 */
  readonly locked = input(false);
  /** 速度。null は構成ファイルのとおり、'max' は最速、数は倍率（1 が等速）。 */
  readonly speed = model.required<Speed>();
  /** 倍率の欄が範囲の外か、数でない。 */
  readonly invalid = model(false);

  /** 利用者が選んだ形。外から speed が変わったとき、選んだ形で表せなければ、speed から決め直す。 */
  protected readonly choice = linkedSignal<Speed, SpeedChoice>({
    source: this.speed,
    computation: (speed, previous) =>
      previous && fits(previous.value, speed) ? previous.value : choiceOf(speed),
  });
  /** 倍率の欄の文字（打っている途中の「1.」なども、そのまま持つ）。 */
  protected readonly factorText = linkedSignal<Speed, string>({
    source: this.speed,
    computation: (speed, previous) => {
      if (previous && Number(numberText(previous.value)) === speed) return previous.value;
      return typeof speed === 'number' && speed !== 1 ? String(speed) : (previous?.value ?? '2');
    },
  });

  /**
   * 構成のとおり以外の選択肢と、選べるか（範囲と最速を選べるかから）。選べないものは押せなくする（aria-disabled。
   * Material の並びは Tab で選ばれているものに入り、矢印キーは押せないものを飛ばすので、キーボードでは届かない）。
   * 選べる速さの文は並びの説明（aria-describedby）で、並びに入ると読まれる（切り替えは 1 つずつの説明を持てない）。
   */
  protected readonly options = computed(() => {
    const [min, max] = [this.min(), this.max()];
    return [
      { value: 'max', label: $localize`最速`, offered: this.fastest() },
      { value: 'real', label: $localize`等速`, offered: min <= 1 && 1 <= max },
      // 範囲が 1 だけなら、倍率で表せるものは等速と同じ
      { value: 'factor', label: $localize`倍率`, offered: !(min === 1 && max === 1) },
    ] satisfies { value: SpeedChoice; label: string; offered: boolean }[];
  });
  /** 選べない選択肢があるときの、選べる速さの文。 */
  protected readonly limitsText = computed(() => {
    if (this.options().every((o) => o.offered)) return null;
    const [min, max] = [this.min(), this.max()];
    if (min === max) {
      return min === 1
        ? $localize`この実行で選べる速さは等速だけです。`
        : $localize`この実行で選べる速さは ${min}:speed: 倍だけです。`;
    }
    const range = $localize`この実行で選べる速さは ${min}:min: 倍から ${max}:max: 倍までです。`;
    return this.fastest() ? range : `${range} ${$localize`最速は選べません。`}`;
  });

  protected readonly error = computed(() => {
    if (this.choice() !== 'factor' || this.factorOf(this.factorText()) !== null) return null;
    return $localize`倍率は ${this.min()}:min: から ${this.max()}:max: の数にしてください。`;
  });
  /** 倍率の誤りを出してよいか: 欄を離れたか Enter の後。打ち直すと消す（invalid は打つたびに決める）。 */
  protected readonly checked = signal(false);
  protected readonly shownError = computed(() => (this.checked() ? this.error() : null));

  protected choose(choice: SpeedChoice): void {
    this.choice.set(choice);
    const factor = this.factorOf(this.factorText());
    const speeds: Record<SpeedChoice, Speed> = {
      file: null,
      max: 'max',
      real: 1,
      factor: factor ?? this.speed(),
    };
    this.speed.set(speeds[choice]);
    this.invalid.set(choice === 'factor' && factor === null);
  }

  protected setFactor(text: string): void {
    this.factorText.set(text);
    this.checked.set(false);
    const factor = this.factorOf(text);
    this.invalid.set(factor === null);
    if (factor !== null) this.speed.set(factor);
  }

  /** Enter で誤りを出す。日本語入力の変換を確定する Enter（isComposing）は数えない。 */
  protected enter(event: Event): void {
    if (!(event as KeyboardEvent).isComposing) this.checked.set(true);
  }

  /** 倍率の欄の文字（全角の数字も）が範囲の中の数なら、その数。 */
  private factorOf(text: string): number | null {
    const trimmed = numberText(text);
    const value = Number(trimmed);
    return trimmed !== '' && Number.isFinite(value) && value >= this.min() && value <= this.max()
      ? value
      : null;
  }
}
