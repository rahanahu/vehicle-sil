// レイアウトの検査。画面を足したら、その経路を e2e/layout.spec.ts の ROUTES に足す。
import { Page } from '@playwright/test';

export type NavMode = 'drawer' | 'rail' | 'full';

export interface Viewport {
  width: number;
  height: number;
  navMode: NavMode;
}

/** 試す幅。3 つの区分と、その境目の前後、狭い窓。 */
export const VIEWPORTS: readonly Viewport[] = [
  { width: 360, height: 740, navMode: 'drawer' },
  { width: 480, height: 720, navMode: 'drawer' },
  { width: 599, height: 800, navMode: 'drawer' },
  { width: 600, height: 800, navMode: 'rail' },
  { width: 1024, height: 768, navMode: 'rail' },
  { width: 1199, height: 800, navMode: 'rail' },
  { width: 1200, height: 800, navMode: 'full' },
  { width: 1920, height: 1080, navMode: 'full' },
];

/**
 * 崩れを探して、見つけたものを文で返す（なければ空）。
 * - ページ、またはスクロールする要素（overflow-x が auto か scroll）が横にはみ出している。
 *   わざと横にスクロールさせる場所は data-scroll-x を付ける。
 * - main の中の要素が main の外に出ている。
 * - カード（mat-card、data-layout-item）の中の要素がカードの外に出ている。
 * - カードどうしが重なっている。
 * - ナビゲーション（引き出し以外）と中身が重なっている。
 */
function evaluateLayout(viewport?: Viewport): string[] {
  const problems: string[] = [];
  const slack = 1;
  const name = (el: Element) => {
    const id = el.getAttribute('data-testid');
    const cls = typeof el.className === 'string' ? el.className.trim().split(/\s+/)[0] : '';
    return `<${el.tagName.toLowerCase()}${id ? ` data-testid=${id}` : ''}${cls ? ` .${cls}` : ''}>`;
  };
  const shown = (el: Element) => {
    const rect = el.getBoundingClientRect();
    const style = getComputedStyle(el);
    return rect.width > 0 && rect.height > 0 && style.visibility !== 'hidden';
  };
  const outside = (inner: DOMRect, outer: DOMRect) =>
    inner.left < outer.left - slack || inner.right > outer.right + slack;

  const content = viewport ? document.querySelector('mat-sidenav-content') : null;
  if (viewport && !content) return ['no <mat-sidenav-content>'];
  if (content) content.scrollTo(0, 0);

  const root = document.documentElement;
  if (root.scrollWidth > root.clientWidth + slack) {
    problems.push(`the page scrolls sideways (${root.scrollWidth} > ${root.clientWidth})`);
  }
  for (const el of document.querySelectorAll('body *')) {
    if (!shown(el) || el.closest('[data-scroll-x]')) continue;
    const overflow = getComputedStyle(el).overflowX;
    if ((overflow === 'auto' || overflow === 'scroll') && el.scrollWidth > el.clientWidth + slack) {
      problems.push(`${name(el)} scrolls sideways (${el.scrollWidth} > ${el.clientWidth})`);
    }
  }

  const main = document.querySelector('main');
  if (!main) return ['no <main>'];
  const mainRect = main.getBoundingClientRect();
  for (const el of main.querySelectorAll('*')) {
    if (
      shown(el) &&
      !el.closest('[data-scroll-x]') &&
      outside(el.getBoundingClientRect(), mainRect)
    ) {
      problems.push(`${name(el)} sticks out of <main>`);
    }
  }

  const items = [...main.querySelectorAll('mat-card, [data-layout-item]')].filter(shown);
  for (const item of items) {
    const itemRect = item.getBoundingClientRect();
    for (const el of item.querySelectorAll('*')) {
      if (
        shown(el) &&
        !el.closest('[data-scroll-x]') &&
        outside(el.getBoundingClientRect(), itemRect)
      ) {
        problems.push(`${name(el)} sticks out of ${name(item)}`);
      }
    }
  }
  const overlap = (a: DOMRect, b: DOMRect) =>
    a.right - slack > b.left &&
    b.right - slack > a.left &&
    a.bottom - slack > b.top &&
    b.bottom - slack > a.top;
  for (let i = 0; i < items.length; i++) {
    for (let j = i + 1; j < items.length; j++) {
      if (items[i].contains(items[j]) || items[j].contains(items[i])) continue;
      if (overlap(items[i].getBoundingClientRect(), items[j].getBoundingClientRect())) {
        problems.push(`${name(items[i])} #${i} overlaps ${name(items[j])} #${j}`);
      }
    }
  }

  const nav = document.querySelector('[data-testid="nav"]');
  if (nav && shown(nav) && nav.getAttribute('data-nav-mode') !== 'drawer') {
    if (overlap(nav.getBoundingClientRect(), mainRect)) {
      problems.push('the navigation overlaps <main>');
    }
  }

  if (viewport && content) {
    const checkInViewport = (where: 'top' | 'bottom') => {
      for (const id of ['start', 'stop', 'outcome']) {
        const matches = document.querySelectorAll(`[data-testid="${id}"]`);
        if (matches.length !== 1) {
          problems.push(
            `${viewport.width} px, ${where}: [data-testid="${id}"] matched ${matches.length} elements`,
          );
          continue;
        }
        const el = matches[0];
        if (!shown(el)) {
          problems.push(`${viewport.width} px, ${where}: [data-testid="${id}"] is hidden`);
          continue;
        }
        const rect = el.getBoundingClientRect();
        if (
          rect.left < 0 ||
          rect.top < 0 ||
          rect.right > window.innerWidth ||
          rect.bottom > window.innerHeight
        ) {
          problems.push(
            `${viewport.width} px, ${where}: [data-testid="${id}"] is outside viewport`,
          );
          continue;
        }
        for (let ancestor = el.parentElement; ancestor; ancestor = ancestor.parentElement) {
          const style = getComputedStyle(ancestor);
          if (style.display === 'contents') continue;
          const clipX = style.overflowX !== 'visible';
          const clipY = style.overflowY !== 'visible';
          if (!clipX && !clipY) continue;
          const aRect = ancestor.getBoundingClientRect();
          if (
            (clipX && (rect.left < aRect.left || rect.right > aRect.right)) ||
            (clipY && (rect.top < aRect.top || rect.bottom > aRect.bottom))
          ) {
            problems.push(
              `${viewport.width} px, ${where}: [data-testid="${id}"] is clipped by ${name(ancestor)}`,
            );
            break;
          }
        }
      }
    };

    checkInViewport('top');
    const band = document.querySelector('app-run-status');
    const bandHeight = band ? band.getBoundingClientRect().height : 0;
    if (!band || bandHeight > viewport.height / 3) {
      problems.push(
        `${viewport.width} px: app-run-status height ${bandHeight} > ${viewport.height / 3}`,
      );
    }
    content.scrollTo(0, content.scrollHeight);
    checkInViewport('bottom');
  }

  return [...new Set(problems)].slice(0, 20);
}

export function findLayoutProblems(page: Page): Promise<string[]> {
  return page.evaluate(evaluateLayout);
}

export function runViewportProblems(page: Page, viewport: Viewport): Promise<string[]> {
  return page.evaluate(evaluateLayout, viewport);
}
