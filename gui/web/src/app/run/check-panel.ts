import { Component, computed, input, model } from '@angular/core';
import { MatSlideToggleModule } from '@angular/material/slide-toggle';
import { CheckResult } from '@vehicle-sil/gui-client';
import { FindingsList } from './findings-list';
import { checkOutcomeLabel, checkStatusLabel } from './labels';

/** participant の行。 */
interface Row {
  name: string;
  status: string;
  required: boolean | null;
  networks: string;
  /** 変数の数。わからなければ null。 */
  variables: number | null;
  /** 使うか（外していない）。 */
  used: boolean;
  /** 構成ファイルで外してある（画面からは戻せない）。 */
  disabledInFile: boolean;
}

/** 照合の結果: 結果の 1 行、participant の一覧（「使う」の切り替えつき）、所見。 */
@Component({
  selector: 'app-check-panel',
  imports: [MatSlideToggleModule, FindingsList],
  templateUrl: './check-panel.html',
  styleUrl: './check-panel.css',
})
export class CheckPanel {
  readonly check = input.required<CheckResult>();
  /** 画面で外した participant の名前。切り替えるとここが変わり、照合し直す。 */
  readonly disabled = model.required<ReadonlySet<string>>();
  /** 切り替えられないとき（実行中、照合の途中）。 */
  readonly locked = input(false);

  protected readonly outcome = computed(() => checkOutcomeLabel(this.check().result));
  protected readonly passed = computed(() => this.check().result === 'passed');

  protected readonly rows = computed<Row[]>(() => {
    const check = this.check();
    const disabled = this.disabled();
    return check.participants
      .filter((p) => p.name !== null)
      .map((p) => {
        const name = p.name as string;
        const disabledHere = disabled.has(name);
        return {
          name,
          status: checkStatusLabel(p.status),
          required: p.required,
          networks: check.networks
            .filter((n) => n.name !== null && n.participants.includes(name))
            .map((n) => n.name)
            .join(', '),
          variables: p.variables === null ? null : p.variables.length,
          used: !disabledHere && p.enabled !== false,
          disabledInFile: !disabledHere && p.enabled === false,
        };
      });
  });

  protected useLabel(name: string): string {
    return $localize`${name}:name: を使う`;
  }

  protected setUsed(name: string, used: boolean): void {
    this.disabled.update((current) => {
      const next = new Set(current);
      if (used) next.delete(name);
      else next.add(name);
      return next;
    });
  }
}
