import { CUSTOM_ELEMENTS_SCHEMA, Component, ElementRef, viewChild } from '@angular/core';
import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/timeline';
import type { VehicleSilTimelineElement } from '@vehicle-sil/gui-client/timeline';

@Component({
  selector: 'vsil-angular-negative',
  standalone: true,
  schemas: [CUSTOM_ELEMENTS_SCHEMA],
  template: ` <vehicle-sil-timeline #tlRef (cursorchange)="onCursor(123)"></vehicle-sil-timeline> `,
})
export class AngularNegativeComponent {
  readonly tlRef = viewChild<ElementRef<VehicleSilTimelineElement>>('tlRef');

  onCursor(event: Event): void {
    const tl = this.tlRef()?.nativeElement;
    void [tl, event];
  }
}
