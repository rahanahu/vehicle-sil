import { httpResource } from '@angular/common/http';
import { Injectable, computed, inject } from '@angular/core';
import { readAppInfo } from '../api';
import { AuthToken } from '../auth/auth-token';

/** 上限を知らないとき（サーバーの答えの前）の値。ランチャーと同じ。 */
export const DEFAULT_LIMITS = {
  speed_min: 0.000001,
  speed_max: 1000000,
  step_ms_max: 1e12,
} as const;

/**
 * サーバーとランチャーの情報（GET api/app）。アプリ全体で 1 つ。ランチャーの `features`（何ができるか）を、画面の
 * どの部品からも同じ signal で読む。古いランチャーが持たない機能の操作は、ここを見て出さない。
 */
@Injectable({ providedIn: 'root' })
export class ServerInfo {
  private readonly auth = inject(AuthToken);

  /** GET api/app（トークンがあるときだけ）。 */
  readonly app = httpResource(() => (this.auth.token() ? 'api/app' : undefined), {
    parse: readAppInfo,
  });

  /** ランチャーの `version --json` の features（まだわからなければ空）。 */
  readonly features = computed<readonly string[]>(() =>
    this.app.hasValue() ? this.app.value().launcher.features : [],
  );

  /** 速さの範囲とステップ送りの上限（サーバーが言う。ランチャーと同じ）。 */
  readonly limits = computed(() =>
    this.app.hasValue() ? this.app.value().limits : DEFAULT_LIMITS,
  );

  /** ランチャーがその機能を持つか（`speed`、`start-paused`、`time-commands` など）。 */
  has(feature: string): boolean {
    return this.features().includes(feature);
  }
}
