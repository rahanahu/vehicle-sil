import { Component, LOCALE_ID, computed, inject, input } from '@angular/core';
import { FrontResult, RunResult } from '@vehicle-sil/gui-client';
import { MeasurePipe } from '../core/format/measure-pipe';
import { FindingsList } from '../widgets/findings-list';
import {
  TONE_MARKS,
  cutShort,
  interruptedByLabel,
  latenessText,
  resultLine,
  resultTone,
  simulationLabel,
  speedLabel,
} from './labels';

/** 実行の結果: 結果と、どう終わったか、時間の進み方、理由（所見）。 */
@Component({
  selector: 'app-result-panel',
  imports: [FindingsList, MeasurePipe],
  template: `
    <!-- ランチャーの英語の値（result、simulation、interrupted_by）は、日本語の横に並べず title に置く -->
    <p class="outcome" [attr.data-tone]="tone()" [title]="result().result" data-testid="result">
      <span class="mark" aria-hidden="true">{{ mark() }}</span>
      <span [attr.lang]="line().english ? 'en' : null">{{ line().text }}</span>
    </p>
    <dl class="facts">
      @if (result().simulation; as simulation) {
        <div>
          <dt i18n>終わり方</dt>
          <dd [title]="simulation">{{ simulationText(simulation) }}</dd>
        </div>
      }
      @if (result().interrupted_by; as by) {
        <div>
          <dt i18n>止めたもの</dt>
          <dd [title]="by">{{ interruptedBy(by) }}</dd>
        </div>
      }
      <div>
        <dt i18n>所見の数</dt>
        <dd i18n>
          誤り {{ result().errors }}、警告 {{ result().warnings }}、注記 {{ result().notes }}
        </dd>
      </div>
    </dl>
    @if (result().timing; as timing) {
      <dl class="facts" data-testid="timing">
        <div>
          <dt i18n>速度</dt>
          <dd>{{ speedText() }}</dd>
        </div>
        @if (timing.sim_ms !== null) {
          <div>
            <dt i18n>仮想時刻</dt>
            <dd i18n>{{ timing.sim_ms / 1000 | measure: 3 }} s</dd>
          </div>
        }
        @if (timing.wall_ms !== null) {
          <div>
            <dt i18n>かかった実時間</dt>
            <dd i18n>{{ timing.wall_ms / 1000 | measure: 3 }} s</dd>
          </div>
        }
        @if (timing.actual_speed !== null) {
          <div>
            <dt i18n>実際の速さ</dt>
            <dd i18n>{{ timing.actual_speed | measure: 2 }} 倍</dd>
          </div>
        }
        @if (timing.paused_ms) {
          <div>
            <dt i18n>一時停止していた時間</dt>
            <dd i18n>{{ timing.paused_ms / 1000 | measure: 3 }} s</dd>
          </div>
        }
        @if (timing.paced_steps) {
          <div>
            <dt i18n>遅れた刻み</dt>
            <dd i18n>{{ timing.late_steps ?? 0 }} / {{ timing.paced_steps }}</dd>
          </div>
          <div>
            <dt i18n>最大の遅れ</dt>
            <dd i18n>{{ timing.max_late_ms ?? 0 }} ms</dd>
          </div>
          <div>
            <dt i18n>ずれの合計</dt>
            <dd i18n>{{ timing.slipped_ms ?? 0 }} ms</dd>
          </div>
          @if (timing.late_ms) {
            <div>
              <dt i18n>終わったときの遅れ</dt>
              <dd i18n>{{ timing.late_ms }} ms</dd>
            </div>
          }
        }
      </dl>
      @if (lateText(); as text) {
        <p class="failed" data-testid="timing-late"><span aria-hidden="true">⚠</span> {{ text }}</p>
      }
    }
    @if (leftProcesses()) {
      <p class="failed" i18n>プロセスが残った participant: {{ leftProcesses() }}</p>
    }
    <app-findings-list [findings]="result().findings" label="実行の所見" i18n-label />
  `,
  styles: `
    .outcome {
      margin: 0 0 var(--app-space-2);
      font: var(--mat-sys-title-large);
      display: flex;
      flex-wrap: wrap;
      align-items: baseline;
      gap: var(--app-space-2);
    }
    .failed,
    .outcome[data-tone='bad'] {
      color: var(--mat-sys-error);
    }
    .outcome[data-tone='good'] {
      color: var(--mat-sys-primary);
    }
    .facts {
      display: flex;
      flex-wrap: wrap;
      gap: 0 var(--app-space-4);
      margin: 0 0 var(--app-space-3);
    }
    .facts div {
      display: flex;
      gap: var(--app-space-2);
    }
    .facts dt {
      color: var(--mat-sys-on-surface-variant);
    }
    .facts dd {
      margin: 0;
    }
  `,
})
export class ResultPanel {
  private readonly list = new Intl.ListFormat(inject(LOCALE_ID), { type: 'conjunction' });
  /** ランチャーの結果（どう終わったか、時間の進み方、所見）。 */
  readonly result = input.required<RunResult>();
  /** 正面の口の `status.result`: 結果の色と文は、ここから決める。 */
  readonly outcome = input.required<FrontResult>();

  private readonly cut = computed(() => cutShort(this.outcome(), this.result().result));
  /** 成功は ✓、止めた実行と中断は ■ で色を付けない（失敗ではない）、ほかは ✕。 */
  protected readonly tone = computed(() => resultTone(this.outcome(), this.cut()));
  protected readonly mark = computed(() => TONE_MARKS[this.tone()]);
  protected readonly line = computed(() => resultLine(this.outcome(), this.result().result));
  protected readonly speedText = computed(() => speedLabel(this.result().timing?.speed ?? null));
  /**
   * 実時間への遅れ（許容を超えて遅れた刻みがあったときだけ。ランチャーが所見 behind-real-time を出す条件と同じ）。
   * 終わったときの遅れ（late_ms）は、数の欄で見せる。
   */
  protected readonly lateText = computed(() => {
    const t = this.result().timing;
    return t
      ? latenessText({
          lateSteps: t.late_steps ?? 0,
          maxLateMs: t.max_late_ms ?? 0,
          slippedMs: t.slipped_ms ?? 0,
        })
      : null;
  });
  /** プロセスが残った participant（言語の並べ方で）。なければ空。 */
  protected readonly leftProcesses = computed(() =>
    this.list.format(
      this.result()
        .participants.filter((p) => p.left_processes)
        .map((p) => p.name),
    ),
  );

  protected simulationText(simulation: string): string {
    return simulationLabel(simulation, this.result().scenario?.status ?? null);
  }

  protected interruptedBy(by: string): string {
    return interruptedByLabel(by);
  }
}
