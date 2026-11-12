import { Component, computed, input, linkedSignal, model } from '@angular/core';
import { MatButtonToggleModule } from '@angular/material/button-toggle';
import { MatFormFieldModule } from '@angular/material/form-field';
import { MatInputModule } from '@angular/material/input';
import { Speed } from '@vehicle-sil/gui-client';

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
 * 範囲の外か数でないときは、欄の下に理由を出し、invalid を true にする（開始の理由に出す）。
 */
@Component({
  selector: 'app-speed-picker',
  imports: [MatButtonToggleModule, MatFormFieldModule, MatInputModule],
  template: `
    <span class="label" [id]="id + '-label'">{{ label() }}</span>
    <mat-button-toggle-group
      [attr.aria-labelledby]="id + '-label'"
      [value]="choice()"
      [disabled]="locked()"
      (change)="choose($event.value)"
    >
      @if (withFile()) {
        <mat-button-toggle value="file" i18n>構成のとおり</mat-button-toggle>
      }
      <mat-button-toggle value="max" i18n>最速</mat-button-toggle>
      <mat-button-toggle value="real" i18n>等速</mat-button-toggle>
      <mat-button-toggle value="factor" i18n>倍率</mat-button-toggle>
    </mat-button-toggle-group>
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
          [attr.aria-invalid]="error() !== null"
          [attr.aria-describedby]="error() !== null ? id + '-error' : null"
          #factorInput
          (input)="setFactor(factorInput.value)"
        />
      </mat-form-field>
    }
    <p
      class="error"
      [id]="id + '-error'"
      role="alert"
      data-testid="speed-error"
      [class.shown]="error() !== null"
    >
      {{ error() }}
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
      if (previous && Number(previous.value) === speed) return previous.value;
      return typeof speed === 'number' && speed !== 1 ? String(speed) : (previous?.value ?? '2');
    },
  });

  protected readonly error = computed(() => {
    if (this.choice() !== 'factor' || this.valid(this.factorText())) return null;
    return $localize`倍率は ${this.min()}:min: から ${this.max()}:max: の数にしてください。`;
  });

  protected choose(choice: SpeedChoice): void {
    this.choice.set(choice);
    const factor = this.factorText();
    const speeds: Record<SpeedChoice, Speed> = {
      file: null,
      max: 'max',
      real: 1,
      factor: this.valid(factor) ? Number(factor) : this.speed(),
    };
    this.speed.set(speeds[choice]);
    this.invalid.set(choice === 'factor' && !this.valid(factor));
  }

  protected setFactor(text: string): void {
    this.factorText.set(text);
    const valid = this.valid(text);
    this.invalid.set(!valid);
    if (valid) this.speed.set(Number(text));
  }

  private valid(text: string): boolean {
    const value = Number(text);
    return (
      text.trim() !== '' && Number.isFinite(value) && value >= this.min() && value <= this.max()
    );
  }
}
