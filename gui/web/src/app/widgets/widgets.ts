import { Type } from '@angular/core';
import { WidgetKind } from './dashboard-spec';
import { GraphWidget } from './graph-widget';
import { LampWidget } from './lamp-widget';
import { NumberWidget } from './number-widget';
import { SelectWidget } from './select-widget';
import { SliderWidget } from './slider-widget';
import { SwitchWidget } from './switch-widget';
import { ValueWidget } from './value-widget';

/**
 * 部品の種類と、そのコンポーネント。ダッシュボード（DashboardView）は、ここから部品を作り、input の `ref`
 * （VariableRef）と `label` を渡す。部品を足すときは、同じ input を持つコンポーネントを作り、WidgetKind と
 * ここに足す（docs/gui.md の「部品を足す」）。
 */
export const WIDGETS: Readonly<Record<WidgetKind, Type<unknown>>> = {
  value: ValueWidget,
  number: NumberWidget,
  slider: SliderWidget,
  switch: SwitchWidget,
  lamp: LampWidget,
  select: SelectWidget,
  graph: GraphWidget,
};
