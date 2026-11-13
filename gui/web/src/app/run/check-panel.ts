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
  /** 利用者が起動する（画面か構成ファイルで指定した）。 */
  manual: boolean;
  /** 構成ファイルで manual: true（画面からは戻せない）。 */
  manualInFile: boolean;
}

/** 照合の結果: 結果の 1 行、participant の一覧（「使う」と「自分で起動する」の切り替えつき）、所見。 */
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
  /** 画面で「自分で起動する」にした participant の名前（`--manual`）。切り替えると照合し直す。 */
  readonly manual = model<ReadonlySet<string>>(new Set());
  /** 「自分で起動する」を出すか（ランチャーの features に manual-participants があるときだけ）。 */
  readonly manualAvailable = input(false);
  /** 切り替えられないとき（実行中、照合の途中）。 */
  readonly locked = input(false);

  protected readonly outcome = computed(() => checkOutcomeLabel(this.check().result));
  protected readonly passed = computed(() => this.check().result === 'passed');

  protected readonly rows = computed<Row[]>(() => {
    const check = this.check();
    const disabled = this.disabled();
    const manual = this.manual();
    return check.participants
      .filter((p) => p.name !== null)
      .map((p) => {
        const name = p.name as string;
        const disabledHere = disabled.has(name);
        const manualHere = manual.has(name);
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
          manual: manualHere || p.manual,
          manualInFile: !manualHere && p.manual,
        };
      });
  });

  protected useLabel(name: string): string {
    return $localize`${name}:name: を使う`;
  }

  protected manualLabel(name: string): string {
    return $localize`${name}:name: を自分で起動する`;
  }

  protected setUsed(name: string, used: boolean): void {
    this.disabled.update((current) => {
      const next = new Set(current);
      if (used) next.delete(name);
      else next.add(name);
      return next;
    });
    // 外した participant は自分で起動しない（ランチャーは両方の指定を断る）
    if (!used && this.manual().has(name)) this.setManual(name, false);
  }

  protected setManual(name: string, manual: boolean): void {
    this.manual.update((current) => {
      const next = new Set(current);
      if (manual) next.add(name);
      else next.delete(name);
      return next;
    });
  }
}
