// 画面の試験のための、`--describe` にだけ答える偽の ECU（実行はしない。C++ のビルドは要らない）。
// 使い方: node describe-only-ecu.mjs <Ethernet のチャネルの名前（コンマ区切り）> <最初の MAC の末尾> --describe [--name <名前>]
// 版（`vehicle_sil`）は書かない（書かなければ、ランチャーは版を比べない）。申告は、ランチャーが読む `vehicle_sil_ecu` の形（チャネルごとに 1 つの Ethernet の口）。
const [channels, base, ...rest] = process.argv.slice(2);
if (!rest.includes('--describe')) process.exit(4);
const at = rest.indexOf('--name');
const name = at >= 0 ? rest[at + 1] : 'X';
const networks = channels.split(',').map((channel, i) => ({
  name: channel,
  kind: 'ethernet',
  use: 'ethernet',
  nodes: [],
  mac: `02:00:00:00:00:${(Number(base) + i).toString(16).padStart(2, '0')}`,
}));
console.log(
  JSON.stringify({
    vehicle_sil_ecu: 1,
    name,
    step_ns: 1_000_000,
    networks,
    variables: [],
    features: ['map-network'],
  }),
);
