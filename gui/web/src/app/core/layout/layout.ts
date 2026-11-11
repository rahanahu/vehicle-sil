import { Injectable, computed } from '@angular/core';
import { mediaQueries } from '../streams/media-queries';

/** 画面の幅の区分の境目（px、以上）。区分はここだけで決める。シェルが CSS の変数としても渡す。 */
export const BREAKPOINTS = { medium: 600, wide: 1200 } as const;

/** compact: 狭い窓（ナビゲーションは引き出し）、medium: 中くらい（アイコンだけの帯）、wide: 広い（文字つき）。 */
export type WidthClass = 'compact' | 'medium' | 'wide';

/** 画面の幅の区分を 1 か所で見張り、signal で渡す。読むのはシェルだけ。部品は自分の幅（container query）で決める。 */
@Injectable({ providedIn: 'root' })
export class Layout {
  private readonly matches = mediaQueries({
    medium: `(min-width: ${BREAKPOINTS.medium}px)`,
    wide: `(min-width: ${BREAKPOINTS.wide}px)`,
  });

  readonly widthClass = computed<WidthClass>(() => {
    const { medium, wide } = this.matches();
    return wide ? 'wide' : medium ? 'medium' : 'compact';
  });
}
