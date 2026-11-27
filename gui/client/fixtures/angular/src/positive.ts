import { CUSTOM_ELEMENTS_SCHEMA, Component, ElementRef, signal, viewChild } from '@angular/core';
import type { Front, Intent } from '@vehicle-sil/gui-client';
import '@vehicle-sil/gui-client/elements';
import type { VsilWriteElement } from '@vehicle-sil/gui-client/elements';
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
    <vsil-slider [client]="front" variable="Engine.e.throttle"></vsil-slider>
    <vsil-write
      #writeRef
      [client]="front"
      variable="Engine.e.flag"
      [value]="false"
      label="Cut"
    ></vsil-write>
    <vsil-write [client]="front" variable="Engine.e.throttle" [value]="0"></vsil-write>
    <vsil-connect [client]="front" network="CAN1" [file]="'bench.yaml'"></vsil-connect>
    <vsil-outside [client]="front"></vsil-outside>
    <vsil-scenario-select [client]="front"></vsil-scenario-select>
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
  readonly writeRef = viewChild<ElementRef<VsilWriteElement>>('writeRef');
  readonly tlRef = viewChild<ElementRef<VehicleSilTimelineElement>>('tlRef');

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
    const write = this.writeRef()?.nativeElement;
    if (write) {
      write.value = false;
      write.value = 0;
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
