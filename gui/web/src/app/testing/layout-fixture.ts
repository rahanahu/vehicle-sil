// レイアウトの e2e（e2e/layout.spec.ts）のための、中身を固定した試験用の画面。捨てない。
// 本物の画面によくある形（カードの並び、2 段組み、表、長い名前）を置き、3 つの幅で崩れないことを確かめる。
import { Component } from '@angular/core';
import { MatCardModule } from '@angular/material/card';
import { MeasurePipe } from '../core/format/measure-pipe';

@Component({
  selector: 'app-layout-fixture',
  imports: [MatCardModule, MeasurePipe],
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

  protected readonly rows = [
    { name: 'EngineSpeed', value: 1533.25, unit: 'rpm' },
    { name: 'VehicleSpeedWithAVeryLongSignalNameThatDoesNotBreak', value: 61.6, unit: 'km/h' },
    { name: 'CoolantTemperature', value: 80.07, unit: '°C' },
  ];
}
