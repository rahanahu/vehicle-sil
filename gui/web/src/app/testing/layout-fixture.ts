// レイアウトの e2e（e2e/layout.spec.ts）のための、中身を固定した試験用の画面。捨てない。
// 本物の画面によくある形（カードの並び、2 段組み、表、長い名前）を置き、3 つの幅で崩れないことを確かめる。
import { Component } from '@angular/core';
import { MatCardModule } from '@angular/material/card';
import { CheckResult, Finding, ParticipantView } from '@vehicle-sil/gui-client';
import { MeasurePipe } from '../core/format/measure-pipe';
import { CheckPanel } from '../run/check-panel';
import { FindingsList } from '../run/findings-list';
import { ParticipantStatus } from '../run/participant-status';

/** 実行の画面の部品に渡す、長い名前と長い文。 */
const LONG_NAME = 'PowertrainControllerWithAVeryLongParticipantNameThatDoesNotBreak';
const LONG_MESSAGE =
  "participant 'PowertrainControllerWithAVeryLongParticipantNameThatDoesNotBreak' failed while the " +
  'simulation was running (exit code 3): /very/long/path/to/a/file/that/does/not/break/anywhere.yaml';

@Component({
  selector: 'app-layout-fixture',
  imports: [MatCardModule, MeasurePipe, CheckPanel, FindingsList, ParticipantStatus],
  template: `
    <h1 i18n>レイアウトの試験</h1>

    <section class="cards" data-testid="fixture-cards" aria-label="カード" i18n-aria-label>
      @for (card of cards; track card.name) {
        <mat-card appearance="outlined" data-layout-item>
          <mat-card-header>
            <mat-card-title class="long-name">{{ card.name }}</mat-card-title>
          </mat-card-header>
          <mat-card-content>
            <dl class="metrics">
              @for (metric of card.metrics; track metric.name) {
                <div class="metric">
                  <dt>{{ metric.name }}</dt>
                  <dd>{{ metric.value | measure: 1 }}</dd>
                </div>
              }
            </dl>
          </mat-card-content>
        </mat-card>
      }
    </section>

    <section class="panes" data-testid="fixture-panes">
      <mat-card appearance="outlined" data-layout-item>
        <mat-card-header><mat-card-title i18n>一覧</mat-card-title></mat-card-header>
        <mat-card-content>
          <table class="table">
            <thead>
              <tr>
                <th scope="col" i18n>名前</th>
                <th scope="col" i18n>値</th>
                <th scope="col" i18n>単位</th>
              </tr>
            </thead>
            <tbody>
              @for (row of rows; track row.name) {
                <tr>
                  <td class="long-name">{{ row.name }}</td>
                  <td>{{ row.value | measure: 2 }}</td>
                  <td>{{ row.unit }}</td>
                </tr>
              }
            </tbody>
          </table>
        </mat-card-content>
      </mat-card>
      <mat-card appearance="outlined" data-layout-item>
        <mat-card-header><mat-card-title i18n>グラフの場所</mat-card-title></mat-card-header>
        <mat-card-content>
          <div class="chart" role="img" aria-label="グラフの場所" i18n-aria-label></div>
        </mat-card-content>
      </mat-card>
    </section>

    <section class="panes" data-testid="fixture-run" aria-label="実行の部品" i18n-aria-label>
      <mat-card appearance="outlined" data-layout-item>
        <mat-card-header><mat-card-title i18n>照合</mat-card-title></mat-card-header>
        <mat-card-content>
          <app-check-panel [check]="check" [disabled]="noneDisabled" />
        </mat-card-content>
      </mat-card>
      <mat-card appearance="outlined" data-layout-item>
        <mat-card-header><mat-card-title i18n>participant の状態</mat-card-title></mat-card-header>
        <mat-card-content>
          <app-participant-status [participants]="participants" />
          <app-findings-list [findings]="findings" />
        </mat-card-content>
      </mat-card>
    </section>
  `,
  styles: `
    :host {
      display: grid;
      gap: var(--app-space-4);
    }
    h1 {
      font: var(--mat-sys-headline-small);
      margin: 0;
    }
    .cards {
      display: grid;
      gap: var(--app-space-3);
      grid-template-columns: repeat(auto-fill, minmax(min(100%, var(--app-card-min)), 1fr));
    }
    mat-card {
      container: card / inline-size;
      min-inline-size: 0;
    }
    .long-name {
      overflow-wrap: anywhere;
    }
    .metrics {
      margin: 0;
    }
    .metric {
      display: flex;
      justify-content: space-between;
      gap: var(--app-space-2);
    }
    dd {
      margin: 0;
      font-variant-numeric: tabular-nums;
    }
    @container card (inline-size < 240px) {
      .metric {
        flex-direction: column;
        gap: 0;
      }
    }
    /* 中身の領域が広ければ 2 段組み（画面の幅でなく、中身の領域の幅で決める） */
    .panes {
      display: grid;
      gap: var(--app-space-3);
    }
    @container content (inline-size >= 900px) {
      .panes {
        grid-template-columns: 2fr 3fr;
      }
    }
    .table {
      inline-size: 100%;
      border-collapse: collapse;
      table-layout: fixed;
    }
    .table th,
    .table td {
      padding: var(--app-space-1) var(--app-space-2);
      text-align: start;
      border-block-end: 1px solid var(--mat-sys-outline-variant);
    }
    .chart {
      block-size: 160px;
      border-radius: var(--mat-sys-corner-small);
      background: var(--mat-sys-surface-container-high);
    }
  `,
})
export class LayoutFixture {
  protected readonly cards = Array.from({ length: 8 }, (_, i) => ({
    name: i === 0 ? 'ECU_POWERTRAIN_CONTROLLER_WITH_A_VERY_LONG_IDENTIFIER' : `ECU ${i + 1}`,
    metrics: [
      { name: 'cycle', value: (i + 1) * 10 },
      { name: 'frames', value: (i + 1) * 1234.56 },
    ],
  }));

  protected readonly finding: Finding = {
    severity: 'error',
    code: 'participant-failed',
    file: 'configs/a/very/long/directory/name/simulation-with-a-long-name.yaml',
    line: 12,
    participant: LONG_NAME,
    network: null,
    message: LONG_MESSAGE,
    detail: null,
  };
  protected readonly findings: Finding[] = [this.finding];
  protected readonly check: CheckResult = {
    file: 'simulation.yaml',
    result: 'failed',
    errors: 1,
    warnings: 0,
    notes: 0,
    stop_at_ms: 1000,
    participants: [
      {
        name: LONG_NAME,
        line: 12,
        required: true,
        enabled: true,
        step_ms: 10,
        status: 'described',
        variables: null,
        control: null,
      },
    ],
    networks: [
      { name: 'J1939_CAN_WITH_A_LONG_NETWORK_NAME', kind: 'can', participants: [LONG_NAME] },
    ],
    findings: [this.finding],
    scenario: null,
  };
  protected readonly noneDisabled: ReadonlySet<string> = new Set();
  protected readonly participants: ParticipantView[] = [
    {
      name: LONG_NAME,
      state: 'ended',
      required: true,
      pid: 1,
      exitCode: 3,
      signal: null,
      failure: null,
      message: null,
      stopRequested: false,
      omittedFields: ['environment_with_a_very_long_field_name_that_does_not_break', 'command'],
      controlLink: { state: 'version_mismatch', variables: null, version: 2, late_messages: null },
    },
    {
      name: 'Dashboard',
      state: 'joined',
      required: false,
      pid: 2,
      exitCode: null,
      signal: null,
      failure: null,
      message: null,
      stopRequested: false,
      omittedFields: [],
      controlLink: { state: 'connected', variables: 17, version: null, late_messages: null },
    },
  ];

  protected readonly rows = [
    { name: 'EngineSpeed', value: 1533.25, unit: 'rpm' },
    { name: 'VehicleSpeedWithAVeryLongSignalNameThatDoesNotBreak', value: 61.6, unit: 'km/h' },
    { name: 'CoolantTemperature', value: 80.07, unit: '°C' },
  ];
}
