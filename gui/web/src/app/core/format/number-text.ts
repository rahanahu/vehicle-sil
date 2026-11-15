/**
 * 数の欄に打った文字を、数として読む前にそろえる: 全角の数字と記号（日本語入力の「１．５」「－３」）を半角にし、
 * 前後の空白を除く。数の欄（変数に書く値、ステップ送りの量、倍率）はどれもこれを通す。
 */
export function numberText(text: string): string {
  return text.normalize('NFKC').trim();
}
