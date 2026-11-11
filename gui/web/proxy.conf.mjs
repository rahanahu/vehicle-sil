// 開発サーバー（ng serve）から Python のサーバーへ転送する。
// 転送先のポートは環境変数 STUB_PORT で変えられる（既定は 8765）。
// 鍵は前方一致なので、/api/ と書いて /apix などを含めない。^ で始まる鍵は正規表現。
const target = `http://127.0.0.1:${process.env.STUB_PORT ?? '8765'}`;

export default {
  '/api/': { target },
  '^/ws$': { target, ws: true },
};
