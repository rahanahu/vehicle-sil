import '@vehicle-sil/gui-client/elements';
import '@vehicle-sil/gui-client/react';
import '@vehicle-sil/gui-client/timeline';

export function NegativeReact18() {
  return (
    <div>
      {/* @ts-expect-error noUnit requires boolean */}
      <vsil-value noUnit={123} />
      {/* @ts-expect-error kebab-case no-unit rejects false (use noUnit={false}) */}
      <vsil-value no-unit={false} />
      {/* @ts-expect-error value rejects objects */}
      <vsil-write value={{ bad: 1 }} />
      {/* @ts-expect-error stepMs rejects boolean */}
      <vsil-time-controls stepMs={true} />
      {/* @ts-expect-error theme rejects unknown literal */}
      <vehicle-sil-timeline theme="neon" />
      {/* @ts-expect-error variables rejects number[] */}
      <vehicle-sil-timeline variables={[123]} />
      {/* @ts-expect-error React synthetic onLoad is omitted (use lowercase onload or ref) */}
      <vehicle-sil-timeline onLoad={() => undefined} />
      {/* @ts-expect-error React synthetic onError is omitted (use lowercase onerror or ref) */}
      <vehicle-sil-timeline onError={() => undefined} />
      <vehicle-sil-timeline
        onload={(e) => {
          // @ts-expect-error status is string, not number
          const bad: number = e.detail.status;
          void bad;
        }}
      />
      <vehicle-sil-timeline
        oncursorchange={(e) => {
          // @ts-expect-error cursor is number | null, not string
          const bad: string = e.detail.cursor;
          void bad;
        }}
      />
    </div>
  );
}
