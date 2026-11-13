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
  {
    path: '/variables',
    label: $localize`変数`,
    // つまみの並び
    icon: 'M3 17v2h6v-2H3zM3 5v2h10V5H3zm10 16v-2h8v-2h-8v-2h-2v6h2zM7 9v2H3v2h4v2h2V9H7zm14 4v-2H11v2h10zm-6-4h2V7h4V5h-4V3h-2v6z',
  },
];
