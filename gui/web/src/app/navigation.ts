/** ナビゲーションの項目。 */
export interface NavItem {
  /** 経路（例: '/'）。 */
  path: string;
  /** 表示する名前（$localize で印を付ける）。 */
  label: string;
  /** 24×24 の SVG のパス。 */
  icon: string;
}

/** ナビゲーションに出す項目。画面を足したら、ここに足す。 */
export const NAV_ITEMS: readonly NavItem[] = [
  {
    path: '/',
    label: $localize`実行`,
    // 再生の三角
    icon: 'M8 5v14l11-7z',
  },
];
