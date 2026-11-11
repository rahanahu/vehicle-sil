// 通しの確認用（後で捨てる）。消し方は README.md の「仮の画面」。
// 本物の画面の流れのサービスも、この形（liveConnection を持ち、state と messages を出す）で書く。
import { Injectable } from '@angular/core';
import { TelemetrySample } from '../core/api';
import { liveConnection } from '../core/streams/live-connection';

@Injectable({ providedIn: 'root' })
export class Telemetry {
  private readonly connection = liveConnection<TelemetrySample>('ws');

  /** 接続の状態。 */
  readonly state = this.connection.state;

  /** 届いた通知。画面は latestValue / recentValues で signal にして読む。 */
  readonly samples = this.connection.messages;
}
