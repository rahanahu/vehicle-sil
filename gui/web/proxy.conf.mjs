// 開発サーバー（ng serve）から GUI サーバー（gui/server）へ転送する。
// 転送先のポートは環境変数 GUI_PORT で変えられる（既定は 8765）。GUI サーバーは同じポートと --dev-origin で起動する。
// 鍵は前方一致なので、/api/ と書いて /apix などを含めない。^ で始まる鍵は正規表現。
const target = `http://127.0.0.1:${process.env.GUI_PORT ?? '8765'}`;

export default {
  '/api/': { target },
  '^/ws$': { target, ws: true },
};
