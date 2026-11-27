import type { Front } from '@vehicle-sil/gui-client';
import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/react';
import '@vehicle-sil/gui-client/timeline';

export function PositiveReact19({ front }: { front: Front }) {
  return (
    <div>
      <vsil-status client={front} />
      <vsil-run-controls
        client={front}
        intent={{ speed: 'max' }}
        label-start="Run"
        labelStop="Halt"
        labelForceStop="Kill"
      />
      <vsil-time-controls
        client={front}
        speeds="0.5 1 max"
        stepMs={25}
        step-ms="10"
        labelPause="P"
        labelResume="R"
        labelStep="S"
        labelRunUntil="U"
      />
      <vsil-value client={front} variable="Dash.d.speed" on="HI" off="lo" no-unit noUnit={true} />
      <vsil-slider client={front} variable="Engine.e.throttle" />
      <vsil-write client={front} variable="Engine.e.flag" value={false} label="Cut" />
      <vsil-write client={front} variable="Engine.e.throttle" value={0} />
      <vsil-write client={front} variable="Engine.e.mode" value="eco" />
      <vsil-scenario-select client={front} />
      <vsil-scenario-result client={front} />
      <vsil-timeline client={front} />
      <vsil-connect client={front} network="CAN1" file="bench.yaml" />
      <vsil-outside client={front} />
      <vehicle-sil-timeline
        compact={true}
        variables={['Body.*']}
        theme="dark"
        data={{ vehicle_sil_timeline: 1 }}
        onload={(e) => {
          const status: string = e.detail.status;
          void status;
        }}
        onerror={(e) => {
          const message: string = e.detail.message;
          void message;
        }}
        oncursorchange={(e) => {
          const cursor: number | null = e.detail.cursor;
          void cursor;
        }}
        onstepselect={(e) => {
          const step: number | null = e.detail.step;
          void step;
        }}
        onviewchange={(e) => {
          const from: number = e.detail.from;
          void from;
        }}
      />
      <vehicle-sil-timeline variables="Body.*" compact="" hide="help tables" />
    </div>
  );
}
