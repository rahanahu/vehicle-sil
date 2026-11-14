// 「起きるはずのことを待つ上限」の倍率（環境変数 VEHICLE_SIL_TEST_TIMEOUT_SCALE。未設定なら 1）。C++ の試験と GUI サーバーの
// 試験と同じ変数で、遅い PC、負荷のかかった PC、CI で大きくする。通る試験は待つものが起きた時点で進むので、遅くならない。
// 「起きないこと」を確かめる待ちと、画面やサーバーに与える時間（ガードの長さなど）には掛けない（試験の意味が変わる）。
// 設定ファイル（playwright*.config.ts）と試験を動かすプロセスの両方が読む。

function readScale(): number {
  const text = process.env['VEHICLE_SIL_TEST_TIMEOUT_SCALE'] ?? '';
  const value = text === '' ? 1 : Number(text);
  if (!Number.isFinite(value) || value <= 0) {
    throw new Error(
      `VEHICLE_SIL_TEST_TIMEOUT_SCALE="${text}" is not a number greater than 0: set it to a factor such as 3 ` +
        '(every upper bound of waiting in the tests 3 times as long), or unset it',
    );
  }
  return value;
}

export const TIMEOUT_SCALE = readScale();

/** 起きるはずのことを待つ上限（ミリ秒）に倍率を掛ける。 */
export function scaled(ms: number): number {
  return Math.ceil(ms * TIMEOUT_SCALE);
}
