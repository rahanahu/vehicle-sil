import { CUSTOM_ELEMENTS_SCHEMA, Component, ElementRef, signal, viewChild } from '@angular/core';
import type { Front, Intent } from '@vehicle-sil/gui-client';
import '@vehicle-sil/gui-client/elements';
import type {
  VsilSliderElement,
  VsilWriteElement,
  WriteOutcome,
  WriteQueuedInfo,
} from '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/timeline';
import type {
  VehicleSilTimelineElement,
  VehicleSilTimelineErrorDetail,
  VehicleSilTimelineLoadDetail,
} from '@vehicle-sil/gui-client/timeline';

@Component({
  selector: 'vsil-angular-consumer',
  standalone: true,
  schemas: [CUSTOM_ELEMENTS_SCHEMA],
  template: `
    <vsil-status [client]="front"></vsil-status>
    <vsil-run-controls
      [client]="front"
      [intent]="intent"
      label-start="Run"
      [labelStop]="'Stop'"
    ></vsil-run-controls>
    <vsil-time-controls [client]="front" [stepMs]="25" speeds="0.5 1 max"></vsil-time-controls>
    <vsil-value
      [client]="front"
      [variable]="varName()"
      [noUnit]="noUnit()"
      on="ON"
      off="off"
    ></vsil-value>
    <vsil-slider
      #sliderRef
      [client]="front"
      variable="Engine.e.throttle"
      (vsil-write-queued)="onWriteQueued($event)"
      (vsil-write-result)="onWriteResult($event)"
    ></vsil-slider>
    <vsil-write
      #writeRef
      [client]="front"
      variable="Engine.e.flag"
      [value]="false"
      label="Cut"
      (vsil-write-queued)="onWriteQueued($event)"
      (vsil-write-result)="onWriteResult($event)"
    ></vsil-write>
    <vsil-write [client]="front" variable="Engine.e.throttle" [value]="0"></vsil-write>
    <vsil-connect [client]="front" network="CAN1" [file]="'bench.yaml'"></vsil-connect>
    <vsil-outside [client]="front"></vsil-outside>
    <vsil-scenario-select [client]="front"></vsil-scenario-select>
    <vsil-case-select [client]="front"></vsil-case-select>
    <vsil-scenario-result [client]="front"></vsil-scenario-result>
    <vsil-timeline [client]="front"></vsil-timeline>
    <vehicle-sil-timeline
      #tlRef
      [compact]="true"
      [variables]="vars()"
      theme="dark"
      (load)="onLoad($event)"
      (error)="onError($event)"
      (cursorchange)="onCursor($event)"
    ></vehicle-sil-timeline>
  `,
})
export class AngularConsumerComponent {
  front: Front | null = null;
  intent: Intent = { speed: 'max' };
  readonly varName = signal('Dash.d.speed');
  readonly noUnit = signal(true);
  readonly vars = signal<string[]>(['Body.*']);
  readonly sliderRef = viewChild<ElementRef<VsilSliderElement>>('sliderRef');
  readonly writeRef = viewChild<ElementRef<VsilWriteElement>>('writeRef');
  readonly tlRef = viewChild<ElementRef<VehicleSilTimelineElement>>('tlRef');

  onWriteQueued(event: Event): void {
    const detail = (event as CustomEvent<WriteQueuedInfo>).detail;
    const seq: number = detail.seq;
    void seq;
  }

  onWriteResult(event: Event): void {
    const detail = (event as CustomEvent<WriteOutcome>).detail;
    if (detail.ok) {
      const at: number | null = detail.appliedAtMs;
      void at;
    } else {
      const code: string = detail.error.code;
      void code;
    }
  }

  onLoad(event: Event): void {
    const detail = (event as CustomEvent<VehicleSilTimelineLoadDetail>).detail;
    void detail.status;
  }

  onError(event: Event): void {
    const detail = (event as CustomEvent<VehicleSilTimelineErrorDetail>).detail;
    void detail.message;
  }

  onCursor(event: Event): void {
    const detail = (event as CustomEvent<{ cursor: number | null }>).detail;
    void detail.cursor;
  }

  syncElements(): void {
    const slider = this.sliderRef()?.nativeElement;
    if (slider) {
      slider.addEventListener('vsil-write-queued', (e) => {
        const seq: number = e.detail.seq;
        // @ts-expect-error seq is number, not string
        const bad: string = e.detail.seq;
        void [seq, bad];
      });
      slider.addEventListener('vsil-write-result', (e) => {
        if (e.detail.ok) {
          const at: number | null = e.detail.appliedAtMs;
          void at;
        } else {
          // @ts-expect-error error.code is string, not number
          const bad: number = e.detail.error.code;
          void bad;
        }
      });
    }
    const write = this.writeRef()?.nativeElement;
    if (write) {
      write.value = false;
      write.value = 0;
      write.addEventListener('vsil-write-queued', (e) => {
        const session: string = e.detail.session;
        void session;
      });
      write.addEventListener('vsil-write-result', (e) => {
        // @ts-expect-error appliedAtMs requires narrowing ok === true
        const bad: number | null = e.detail.appliedAtMs;
        void bad;
      });
    }
    const tl = this.tlRef()?.nativeElement;
    if (tl) {
      tl.variables = ['Body.*'];
      tl.compact = true;
      // @ts-expect-error theme rejects unknown literal
      tl.theme = 'invalid-theme';
    }
  }
}
