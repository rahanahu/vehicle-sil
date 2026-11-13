import { Injectable, computed, inject, resource } from '@angular/core';
import { AppInfo, CheckRequest, CheckResult, ConfigList } from '@vehicle-sil/gui-client';
import { AuthToken } from '../auth/auth-token';
import { GUI_CLIENT, readableSignal } from './gui-client';

/**
 * サーバーとランチャーの情報（GET api/app）と、1 回で終わる要求（構成ファイルの一覧、照合）の、Angular の包み。
 * アプリ全体で 1 つ。ランチャーの `features`（何ができるか）を、画面のどの部品からも同じ signal で読む。古い
 * ランチャーが持たない機能の操作は、ここを見て出さない。中身はクライアントのライブラリ（`@vehicle-sil/gui-client`）。
 */
@Injectable({ providedIn: 'root' })
export class ServerInfo {
  private readonly auth = inject(AuthToken);
  private readonly client = inject(GUI_CLIENT);
  private readonly info = readableSignal(this.client.app);

  /** GET api/app（トークンがあるときだけ）。読めたら、ライブラリの app にも入る。 */
  readonly app = resource<AppInfo, string | undefined>({
    params: () => this.auth.token() ?? undefined,
    loader: () => this.client.loadApp(),
  });

  /** ランチャーの `version --json` の features（まだわからなければ空）。 */
  readonly features = computed<readonly string[]>(() => this.info()?.launcher.features ?? []);

  /**
   * いま実行を始めたら ECU の変数を読み書きできるか（`available`）と、できない理由（`reason`。英語）。
   * まだわからなければ null。
   */
  readonly variables = computed(() => this.info()?.variables ?? null);

  /** 速さの範囲とステップ送りの上限（サーバーが言う。ランチャーと同じ）。 */
  readonly limits = readableSignal(this.client.limits);

  /** ランチャーがその機能を持つか（`speed`、`start-paused`、`time-commands` など）。 */
  has(feature: string): boolean {
    return this.features().includes(feature);
  }

  /** GET api/app をもう一度（WebSocket の握手で断られた理由を確かめるとき）。失敗は HttpError。 */
  fetchApp(): Promise<AppInfo> {
    return this.client.loadApp();
  }

  /** GET api/configs: root の下の構成ファイル。 */
  configs(): Promise<ConfigList> {
    return this.client.configs();
  }

  /** POST api/check: 構成ファイルを照合する。 */
  check(request: CheckRequest): Promise<CheckResult> {
    return this.client.check(request);
  }
}
