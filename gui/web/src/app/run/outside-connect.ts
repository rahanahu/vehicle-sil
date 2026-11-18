import {
  Component,
  DestroyRef,
  Directive,
  ElementRef,
  OnInit,
  Signal,
  computed,
  inject,
  input,
  signal,
} from '@angular/core';
import { MatCardModule } from '@angular/material/card';
import { CheckResult, Front } from '@vehicle-sil/gui-client';
import { pageIntent } from '@vehicle-sil/gui-client/elements';
import { GUI_FRONT, readableSignal } from '../core/session/gui-client';

/** 外につなげるネットワークの種類（照合の `networks[].kind`）。 */
const CONNECTABLE = ['can', 'ethernet'];

/**
 * クライアントのライブラリの部品（`<vsil-connect>`、`<vsil-outside>`）を作り、この要素の中に置く。部品は、ページに
 * 入ったときに描く。そのとき接続がなければ自分の接続を開き（この画面の接続と 2 本になる）、`network` がなければ
 * 誤りを出す。Angular の束縛（`[client]`、`[attr.network]`）は要素がページに入った後に当たるので、使わない: 接続と
 * 属性を渡してから置く。属性は作ったときのまま（ネットワークかファイルが替わるときは、`@for` の track で部品ごと
 * 替わる）。
 */
@Directive({ selector: '[appPart]' })
export class PartHost implements OnInit {
  private readonly host = inject<ElementRef<HTMLElement>>(ElementRef).nativeElement;
  private readonly front = inject(GUI_FRONT);

  /** 部品の要素名。 */
  readonly appPart = input.required<'vsil-connect' | 'vsil-outside'>();
  /** 部品の `network` 属性（`<vsil-connect>`）。 */
  readonly partNetwork = input<string | null>(null);
  /** 部品の `file` 属性（`<vsil-connect>`: 一覧を取り、照合する構成ファイル）。 */
  readonly partFile = input<string | null>(null);

  ngOnInit(): void {
    const part = document.createElement(this.appPart()) as HTMLElement & { client: Front | null };
    part.client = this.front;
    const network = this.partNetwork();
    if (network !== null) part.setAttribute('network', network);
    const file = this.partFile();
    if (file !== null) part.setAttribute('file', file);
    // 部品の語は英語のまま（部品の約束）
    part.lang = 'en';
    this.host.append(part);
  }
}

const sameConnect = (a: Readonly<Record<string, string>>, b: Readonly<Record<string, string>>) =>
  JSON.stringify(Object.entries(a).sort()) === JSON.stringify(Object.entries(b).sort());

/**
 * 部品が選んだ、次の実行で外につなぐ先（`pageIntent.connect`。ネットワーク名から先へ。つながないネットワークはない）。
 * 注入の文脈で呼ぶ。部品は、ページから外れるときに自分の選択を消すので、部品を置く画面より長く購読する。
 */
export function connectChoice(): Signal<Readonly<Record<string, string>>> {
  const value = signal(pageIntent.get().connect ?? {}, { equal: sameConnect });
  const stop = pageIntent.subscribe((intent) => value.set(intent.connect ?? {}));
  inject(DestroyRef).onDestroy(stop);
  return value.asReadonly();
}

/**
 * 次の実行で外につなぐネットワークを選ぶ（ネットワークごとに `<vsil-connect>`）。照合の結果の `networks[]` のうち、
 * 種類が can か ethernet のもの。最初は「つながない」で、先の一覧は、利用者が選択を開いたときに部品が取る。
 * 選んだ先は `pageIntent` に入り、画面が `connectChoice()` で読む。選択は部品がページにある間だけ残る: 走行中は
 * `hidden` で隠し（外さない）、構成ファイルを替えると部品ごと替わって消える。保存はしない（読み込み直すと「つながない」）。
 */
@Component({
  selector: 'app-outside-choice',
  imports: [PartHost],
  template: `
    @if (networks().length > 0) {
      <h3 i18n>外につなぐ</h3>
      <p class="hint" i18n>つなぐ先の一覧は、GUI サーバーが動いている PC のものです。</p>
      @for (network of networks(); track network.key) {
        <!-- eslint-disable-next-line @angular-eslint/template/label-has-associated-control -- the select is drawn inside the part -->
        <label class="network" [attr.data-testid]="'connect-' + network.name">
          <span class="name">
            {{ network.name }} <span class="kind" lang="en">({{ network.kind }})</span>
          </span>
          <span
            [appPart]="'vsil-connect'"
            [partNetwork]="network.name"
            [partFile]="network.file"
          ></span>
        </label>
      }
    }
  `,
  styles: `
    :host {
      display: grid;
      grid-template-columns: minmax(0, 1fr);
      gap: var(--app-space-2);
      min-inline-size: 0;
      margin-block-start: var(--app-space-2);
    }
    :host:empty,
    :host([hidden]) {
      display: none;
    }
    h3 {
      font: var(--mat-sys-title-medium);
      margin: 0;
    }
    .hint {
      margin: 0;
      color: var(--mat-sys-on-surface-variant);
    }
    .network {
      display: grid;
      grid-template-columns: minmax(0, 1fr);
      gap: var(--app-space-1);
      min-inline-size: 0;
      overflow-wrap: anywhere;
    }
    .kind {
      color: var(--mat-sys-on-surface-variant);
    }
    /*
     * 部品にはスタイルがない: 選択の箱は幅に収める（先の説明が長くても、横にはみ出さない）。部品の中の要素は
     * Angular が作らないので、閉じ込めたスタイルが届かない（::ng-deep を :host の下だけに使う）
     */
    :host ::ng-deep select {
      max-inline-size: 100%;
      font: inherit;
    }
    :host ::ng-deep ul {
      margin: 0;
      padding-inline-start: var(--app-space-4);
    }
  `,
})
export class OutsideChoice {
  /** 見せている照合の結果（照合し直している間も前の結果。ファイルを替えたら null）。 */
  readonly check = input.required<CheckResult | null>();
  /** 次に始める構成ファイル（root からの相対。部品が一覧を取り、照合するファイル）。 */
  readonly file = input.required<string | null>();

  protected readonly networks = computed(() => {
    const file = this.file();
    if (file === null) return [];
    return (this.check()?.networks ?? []).flatMap((network) =>
      network.name !== null && CONNECTABLE.includes(network.kind ?? '')
        ? [{ key: `${file}\n${network.name}`, file, name: network.name, kind: network.kind }]
        : [],
    );
  });
}

/** 実行が外につないだネットワーク（`<vsil-outside>`。1 つ 1 行）。なければ何も出さない。 */
@Component({
  selector: 'app-outside-rows',
  imports: [MatCardModule, PartHost],
  template: `
    @if (rows().length > 0) {
      <mat-card appearance="outlined" data-testid="outside-card">
        <mat-card-header>
          <mat-card-title><h2 i18n>外につないだネットワーク</h2></mat-card-title>
        </mat-card-header>
        <mat-card-content>
          <div [appPart]="'vsil-outside'" data-testid="outside"></div>
        </mat-card-content>
      </mat-card>
    }
  `,
  styles: `
    :host {
      display: contents;
    }
    h2 {
      font: var(--mat-sys-title-large);
      margin: 0;
    }
    mat-card {
      min-inline-size: 0;
    }
    mat-card-content {
      padding-block-start: var(--app-space-2);
      overflow-wrap: anywhere;
    }
  `,
})
export class OutsideRows {
  protected readonly rows = readableSignal(inject(GUI_FRONT).outside);
}

/** 外につなぐ先の 1 行（「ETH_EXT → fake0」を並べたもの）。つながないなら null。 */
export function connectText(
  connect: Readonly<Record<string, string>>,
  list: Intl.ListFormat,
): string | null {
  const pairs = Object.entries(connect).map(([network, target]) => `${network} → ${target}`);
  return pairs.length > 0 ? list.format(pairs) : null;
}
