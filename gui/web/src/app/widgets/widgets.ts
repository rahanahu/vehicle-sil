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
 * The kinds of widget and their components. The dashboard (DashboardView) makes its widgets from here and passes the
 * inputs `ref` (VariableRef) and `label`. To add a widget, write a component with the same inputs and add it to
 * WidgetKind and here ("Add a widget" in docs/gui.md).
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
